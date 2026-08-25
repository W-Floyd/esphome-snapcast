#include "snapcast_client.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#ifdef CLOCK_SYNC_TSF_ACTIVE
#include "esphome/components/json/json_util.h"
#endif

#include <esp_timer.h>
#ifdef USE_SNAPCLIENT_OPUS
#include <esp_system.h>
#endif
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#ifdef USE_MDNS
#include <mdns.h>
#endif

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome::snapclient {

static const char *const TAG = "snapclient.client";

// Sanity cap on a single message payload; the largest legitimate payloads are FLAC
// wire chunks (a few KB) and codec headers.
static constexpr uint32_t MAX_PAYLOAD_SIZE = 262144;

#ifdef USE_SNAPCLIENT_OPUS
// Snapcast's Opus codec header is a 12-byte "pseudo header": raw Opus packets carry no
// sample format, so the server states the one it resampled to. Magic is the uint32
// 0x4F505553 written little-endian, i.e. the bytes "SUPO" on the wire.
static constexpr size_t OPUS_HEADER_SIZE = 12;
static constexpr uint32_t OPUS_HEADER_MAGIC = 0x4F505553;
// Opus decodes at most 120 ms per packet. snapserver emits 10/20/40/60 ms packets, but
// sizing for the codec maximum costs 23 KB at 48 kHz stereo and removes a failure mode.
static constexpr uint32_t OPUS_MAX_PACKET_MS = 120;
#endif

// Time-sync cadence: a burst on (re)connect for fast filter convergence, then steady
// state. Mirrors the reference web client / embedded snapclient startup behavior.
static constexpr uint32_t TIME_SYNC_BURST_COUNT = 10;
static constexpr uint32_t TIME_SYNC_BURST_INTERVAL_MS = 100;
// While no stream is active, sync only often enough to keep the clock estimate warm
static constexpr uint32_t TIME_SYNC_IDLE_INTERVAL_MS = 2000;

static constexpr uint32_t RECONNECT_DELAY_MS = 2000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;

// Minimum interval between mDNS server scans; each blocks the network task ~3 s, so
// the list refreshes on reconnects rather than on a timer of its own
static constexpr int64_t SERVER_SCAN_MIN_INTERVAL_US = 60000000;

// Deadline slack before the first playback feedback arrives: the first chunk is
// released this far ahead of its deadline so the pipeline has time to spin up, and
// the first feedback-based correction absorbs the remainder.
static constexpr int64_t STARTUP_LEAD_US = 150000;

// Soft-sync correction limit: at most 1/128 of a chunk's frames (~0.2 ms per 26 ms
// chunk) are inserted/dropped per chunk, inaudible but converging ~8 ms per second.
static constexpr uint32_t SOFT_CORRECTION_DIVISOR = 128;

// Above this median error, correct proportionally (post-stall catch-up); below it,
// the steering servo trims single frames. Must sit well above the measurement noise
// and its lag dynamics: a lower threshold put proportional gain inside the loop's
// oscillation amplitude and produced audible warble (~1100 splices/s, observed).
static constexpr int64_t SOFT_CORRECTION_AGGRESSIVE_US = 10000;

// Steering size while muted pre-convergence (~7 ms/s); audibility is not a
// constraint through silence, so lock happens in ~1-2 s instead of up to ~10 s.
//
// Do not raise this to speed up the coarse phase without measuring the whole
// convergence: it was tried at 48 alongside higher muted trim gains and the fine
// stage got WORSE, not better -- 12 s of a 26 s boot, with the median moving away
// from zero (1010 -> 1376 -> 1804 us) before overshooting, then drifting steadily
// past the deadband after unmute. Two of four devices never recovered at all.
// Expressed as a SLEW RATE, not a frame count. The old form was 8 frames per chunk,
// which is only a rate once the chunk size is fixed -- and it is not ours to fix. A
// chunk is one CODEC BLOCK, so its size comes from the encoder, not from any client
// setting and not from the server's buffer (that is the ~1 s of total buffering the
// report calls "buffered"). Measured on four devices from the report cadence:
// 26.2 ms, i.e. ~1152 frames at 44.1 kHz, which is FLAC's default block size.
//
// Because a block is a fixed FRAME COUNT per codec but a fixed DURATION only at a
// given rate, 8 frames is ~6800 ppm at 44.1 kHz and proportionally less as the rate
// rises -- muted convergence silently lost over half its slew at 96 kHz, with nothing
// in the code or the logs to say so. Deriving the count from the chunk actually in
// hand keeps the slew constant across formats and block sizes alike.
static constexpr float STARTUP_STEER_PPM = 6800.0f;

/// Frames to splice per chunk to slew at STARTUP_STEER_PPM, given this chunk's size.
/// At least 1, so the coarse stage always makes progress on a very short chunk.
static uint32_t startup_steer_frames(uint32_t chunk_frames) {
  return std::max<uint32_t>(1, static_cast<uint32_t>(chunk_frames * (STARTUP_STEER_PPM * 1e-6f) + 0.5f));
}

// Closes the playout accounting against a direct observation of the pipeline.
//
// `pushed - played` is an ACCUMULATOR: it integrates two independent counters and has
// no self-correcting term, so any frame miscounted once stays miscounted. The reported
// fill is an OBSERVATION of the same physical quantity -- and with every stage of the
// chain now reported (source ring, mixer transfer buffer, output ring, DMA descriptors)
// the two measure exactly the same audio by two routes. They must agree; where they do
// not, the accumulator is wrong, because an observation cannot latch.
//
// Correction = fill - accounted, applied to the prediction. Zero when the accounting is
// honest, so this is inert on a healthy device rather than a tuning knob. Signed:
// phantom frames INFLATE accounted, making the prediction too late, so the device aims
// early and renders early -- the correction is negative and cancels it. An earlier
// version of this rejected negative samples on the assumption the error only ran one
// way, which made it silently inert; the sign must not be assumed.
//
// Measured before the chain was complete: devices that had been running sat 76-83 ms
// above their reported fill -- more than the 50 ms transfer buffer can physically hold,
// so the excess was frames that existed nowhere -- while devices just restarted sat at
// 52-55 ms, matching TRANSFER_BUFFER_DURATION_MS exactly. The pair carrying the excess
// were the ones audibly behind, across five separate episodes.
//
// Sampled every few chunks, not per chunk: the fill moves on the mixer task's cadence
// (~25 ms) and the query walks the whole listener chain. Smoothed because it differences
// two independently-sampled quantities and a noisy servo target is worse than a slightly
// stale one.
// One RAW line per chunk (~38/s at 44.1 kHz FLAC). See the emit site for the airtime
// trade-off; raise to sample less often on a congested link.
static constexpr uint32_t RAW_SAMPLE_EVERY_CHUNKS = 1;

static constexpr uint32_t FILL_SAMPLE_EVERY_CHUNKS = 8;
static constexpr float FILL_EWMA_ALPHA = 0.25f;
// Bound, symmetric: past this the reading is bad, not a real disagreement
static constexpr int64_t FILL_CORR_MAX_US = 400000;

// Unmute gate width, in multiples of sync_deadband (128 us default -> ~1 ms).
//
// Was 2x. That was calibrated against a loop running KP = 0.5 everywhere, which held the
// common-mode error to sd ~74 us so 256 us was a comfortable fit. The run gain is now 0.1
// (see TRIM_KP_RUN_PPM_PER_US), deliberately chosen to STOP tracking the common-mode
// error, which then wanders to sd 649 us and peaks near 2.8 ms. A gate an order of
// magnitude tighter than the error the loop is designed to permit cannot be met: measured
// with only 54-57% of medians in band, and unmute needs MEDIAN_WINDOW *consecutive* in
// band, so the counter kept resetting and audio took 31-35 s to start.
//
// Widening is sound on the gate's own stated justification -- "corrections inside the
// band are trim-only and inaudible". That holds all the way to converge_fine (2 ms), not
// just to 2x deadband: while muted and inside converge_fine the PI is engaged, trim_holds
// is true, and the splice path is suppressed entirely. So nothing audible happens anywhere
// inside 1 ms; the old value was simply tighter than it needed to be.
//
// The obvious worry -- two boards unmuting at opposite edges of a wider band, i.e. 2 ms of
// differential at start -- does not apply. The gate tests each board's own median, and
// those are 0.84-0.98 correlated between boards with a differential sd of ~30 us. They
// unmute at nearly the same error, which is the same reason the common-mode wander is
// inaudible in the first place.
static constexpr int64_t UNMUTE_BAND_DEADBANDS = 8;

// Median error below which our playout counts as tracking the timebase, reported
// to the TSF layer for leader eligibility. Generous: this gates "am I fit to
// publish the group timebase", not servo precision.
static constexpr int64_t PLAYOUT_HEALTHY_US = 5000;

#ifdef USE_I2S_RATE_LOCK
// Rate-lock PI gains. The plant is an integrator -- queue depth integrates any
// rate mismatch, so the error's *slope* is the trim -- which is why a stepping
// bang-bang trim (a second integrator) limit-cycled structurally on hardware
// (observed: +-250 ppm / +-3 ms swings, matching the double-integrator prediction
// sqrt(2*e0*slew)). Error in us, trim in ppm (1 ppm = 1 us/s of error slope).
//
// Bandwidth is set by disturbance tracking, not settling: the clock-offset estimate
// (and feedback pivot) wander ~100 us/s with wifi jitter, and the loop trails a
// ramp by rate/KP (KP = 0.1 measured +-1-2 ms excursions on hardware -- pure
// tracking lag, not instability). KP = 0.5 bounds that to ~200 us, with ~50 deg
// phase margin against the ~0.85 s measurement lag (feedback-pivot EWMA + median
// window). KI = KP^2/4: critically damped; the integrator absorbs the crystal
// offset so P holds the error at zero.
//
// 0.5 -> 0.1 DELIBERATELY TRADES absolute tracking for inter-device tracking, because
// the two are not the same quantity and only the second is audible in a stereo pair.
// The +-1-2 ms above is the cost, and it was previously read as the reason NOT to lower
// KP. That judgement used the median -- i.e. the COMMON-MODE error, which paired boards
// share (corr(median_a, median_b) = 0.84-0.98 across sessions) and which therefore
// cancels on the wire. What lands between two speakers is the DIFFERENTIAL, and the
// whole chain from it to the audible skew has now been measured link by link, with a
// logic analyser on the I2S lines cross-referenced against both boards' logs:
//
//   differential median   sd 43.0 us
//     x KP = 0.5           -> 21.5 ppm predicted
//   differential trim     sd 20.9 ppm measured
//     -> wire rate         sd 15.2 ppm measured, corr -0.889 (slope -0.65)
//     -> offset excursions 40-90 us, ~2.4/min, every one of them one-sided
//
// (Negative correlation is the correct sign: positive trim plays faster, so the leading
// board renders EARLIER and B-A falls. The 0.65 gain rather than 1.0 is the report-time
// trim snapshot being compared against a wire rate averaged over +-1.6 s.)
//
// Differential trim is KP * differential median to within a few percent in every session
// measured, so this scales the excursions linearly: ~4.2 ppm and ~10-20 us at KP = 0.1.
// Judge this on the analyser and on the differential, NOT on the median -- the median is
// expected to get worse, and that is the trade being made, not a regression.
//
// Do not "fix" the resulting median by rate-limiting or quantising the trim. Both have
// been tried and both limit-cycle structurally, for the reason in the paragraph above
// this one: see the REVERTED note below the KI definition.
// ACQUIRE runs while muted, where the job is to null the error fast enough that the
// unmute gate (MEDIAN_WINDOW consecutive chunks inside 2x sync_deadband) can be met, and
// nothing is audible yet so amplified noise costs nothing. RUN takes over once unmuted,
// where the differential is the only thing that matters. See the switch in the PI block.
static constexpr float TRIM_KP_ACQUIRE_PPM_PER_US = 0.5f;
static constexpr float TRIM_KP_RUN_PPM_PER_US = 0.1f;
// Authority is derived from the ACQUIRE gain deliberately: the clamp exists so the PI can
// express its proportional term at the converge_fine handoff, which is an acquisition
// question. Deriving it from the run gain would shrink the headroom available for a
// large excursion for no benefit.
static constexpr float TRIM_KP_PPM_PER_US = TRIM_KP_ACQUIRE_PPM_PER_US;
// KI = KP^2/4 (critical damping) is COMPUTED AT THE POINT OF USE from whichever gain is
// active, not held in a constant here. A literal lets the two drift apart silently --
// that already happened once, a KP change whose KI had to be recomputed by hand, and
// getting it wrong changes the damping with no compile error and no obvious symptom.
// With a switched KP a single constant would be wrong for one of the two phases by
// construction, which is the same failure wearing a different hat.
// Slew limit on the applied trim, once unmuted. The loop's job here is to cancel THIS
// board's crystal offset -- a hardware property that moves with temperature, i.e. over
// minutes. Anything that demands a fast rate change is by construction not a crystal
// error, and the measured disturbance is COMMON MODE: paired boards see the same error
// at the same instant (corr(median_a, median_b) = 0.981, corr(trim_a, trim_b) = 0.986
// over 42 matched reports), so the phase error it is chasing would cancel between them
// on its own if the loop simply left it alone.
//
// What does NOT cancel is the small mismatch in how two boards answer that shared
// disturbance. Measured: common-mode median sd 75.6 us (range -163..+179) against a
// differential of sd 14.9 us, and a differential TRIM of sd 7.3 ppm, peak 22 ppm.
// Sustained across one ~3.3 s report window that integrates to 24 us sd / 73 us peak
// of real inter-device skew -- which is exactly the 50-100 us excursions seen on the
// logic analyser, appearing there as an instantaneous RATE step (0 -> +42, +40, -73
// ppm) that then decays. No frame corrections fired during any of them; only the clock
// moved. So the audible defect is the loop's own answer to a disturbance that was
// harmless until it was corrected.
//
// REVERTED: rate-limiting the trim was tried and made every measured quantity worse.
//
// The reasoning was sound as far as it went. The disturbance driving the trim is COMMON
// MODE -- paired boards see the same error at the same instant (corr(median_a, median_b)
// = 0.981, corr(trim_a, trim_b) = 0.986 over 42 matched reports) -- so the phase error
// would cancel between them if the loop left it alone, and what lands on the wire is only
// the mismatch in how the two boards answer it: differential trim sd 7.3 ppm, peak 22,
// integrating over a ~3.3 s report to 24 us sd / 73 us peak of real skew. That matched
// the 50-100 us excursions on the analyser, which appear there as an instantaneous RATE
// step (0 -> +42, +40, -73 ppm) that then decays, with no frame corrections anywhere near
// them. The diagnosis stands; the remedy did not.
//
// A flat 2 ppm/s bound, measured on hardware against those same numbers:
//
//   common-mode median   sd  75.6 ->  409.1 us
//   differential median  sd  14.9 ->  329.1 us
//   differential trim    sd   7.3 ->   24.9 ppm  (i.e. 24 -> 82 us per report)
//
// and both boards entered a ~40 s limit cycle swinging +-1.5 ms. That is the textbook
// consequence of rate-limiting inside a feedback loop: while the limiter binds it is a
// phase lag, and here it binds essentially always. The error wanders ~100 us/s, so the P
// term alone legitimately needs ~50 ppm/s (KP * 100) to track it; any bound far below
// that is active almost continuously. Scaling the bound with |error| does not rescue it
// -- at the 180 us common-mode range it still only allows ~2 ppm/s, ~25x too tight.
//
// The general lesson: this loop's output cannot be rate-limited. Differential trim noise
// has to be attacked at the input (filter the error) or through the gain, not at the
// output. Note that differential trim sd 7.3 ppm is almost exactly KP * differential
// median sd (0.5 * 14.9 = 7.45), so it scales linearly with KP -- which makes KP, not a
// limiter, the lever with a predictable effect.
// The clamp is DERIVED, not chosen. The PI takes over at converge_fine, so for it to
// act as a linear controller anywhere in that band the output must be able to express
// the proportional term at the handoff:
//
//   clamp >= KP * converge_fine
//
// A fixed 500 ppm against the 2 ms default violated that by 2x (0.5 * 2000 = 1000),
// so the whole upper half of the fine band was saturated BY CONSTRUCTION: every
// recovery entered the PI stage already railed and stayed there until the error
// halved. Measured mid-recovery at median 922 us: p_term = 461 ppm, trim +489.77,
// span +442..+500, railed 25/48 -- P alone saturating, the integral contributing 29.
//
// This is also why both attempts to tune around it failed. Raising KP moved
// saturation EARLIER (KP 1.25 rails at 400 us); raising the clamp alone to 3000 ppm
// removed the saturation but let the integral wind 6x further and traded it for
// overshoot. The three constants have to move together, so tie them.
//
// Audibility is not the binding constraint here: 1000 ppm is 0.1% pitch against a
// ~0.5% JND, and it is only approached transiently while the error is ~converge_fine.
// Bounded at both ends. The floor keeps the historical authority when converge_fine
// is set tighter than the band the clamp already covered. The ceiling is audibility:
// converge_fine is configurable up to 50 ms, which would derive 25000 ppm = 2.5%
// pitch -- plainly audible, and above the rate lock's own backstop. 2000 ppm is 0.2%,
// still comfortably under the ~0.5% JND. Between the two, the clamp tracks the band.
static constexpr float TRIM_CLAMP_MIN_PPM = 500.0f;
static constexpr float TRIM_CLAMP_MAX_PPM = 2000.0f;

static float trim_clamp_ppm(int64_t converge_fine_us) {
  return std::clamp(TRIM_KP_PPM_PER_US * static_cast<float>(converge_fine_us), TRIM_CLAMP_MIN_PPM,
                    TRIM_CLAMP_MAX_PPM);
}

// REVERTED: raising these while muted made convergence worse, not better.
//
// The reasoning was that neither bound applies through silence -- 500 ppm is an
// audibility bound (0.05% pitch, well under the ~0.5% JND) and KP = 0.5 is a
// phase-margin bound -- so muted, with the loop lag cut, KP could go to 1.25 at
// ~73 deg margin and the clamp to 3000 ppm. That lag figure was the flaw: it was
// derived from the feedback EWMA being ~0.64 s, which assumed callbacks arrive
// every ~10 ms, inferred from a "max feedback gap 10 ms" log line rather than
// measured. If the real interval is longer the lag is larger and KP = 1.25 sits
// below the margin claimed.
//
// Measured on hardware with KP*2.5 / 3000 ppm / a 5-sample muted median: the fine
// stage it was meant to shorten took 12 s of a 26 s boot and OSCILLATED -- median
// 1010 -> 1376 -> 1804 us moving away from zero, overshoot to -642, then a steady
// post-unmute drift to +2334 us and climbing. Two of four devices never converged
// at all. Do not retry without first MEASURING the feedback callback interval and
// the resulting loop lag.
#endif

// A playback-feedback gap this long means the pipeline stopped (and flushed its
// buffers on restart); triggers the frame-accounting re-baseline in
// notify_audio_played()
static constexpr int64_t PIPELINE_FLUSH_GAP_US = 500000;

// Keepalive silence while no chunk is available, pushed in these slices so the
// speaker's no-data timeout (500 ms) cannot tear the pipeline down.
static constexpr int64_t KEEPALIVE_SLICE_US = 50000;

// At most one hard-resync log line this often; the sync report carries full counts
static constexpr int64_t RESYNC_LOG_INTERVAL_US = 2000000;
// How long the stream may stay staler than the server's whole buffer before the
// player gives up on it and forces a reconnect. Long enough that a burst of
// congestion which TCP eventually outruns is ridden out rather than punished
// (recovery needs delivery faster than real time, which a clearing radio provides
// in well under a second), short enough that the silent spiral is bounded.
static constexpr int64_t STALE_BAILOUT_US = 3000000;

// Padding counts as drained below this. Not zero: the DMA holds a fractional buffer of silence
// almost always, and waiting for exactly zero would leave the debt outstanding indefinitely. Half a
// DMA buffer at the rates in use, so the residue left behind is under the servo's own deadband.
static constexpr int64_t PADDING_DRAINED_US = 5000;

// Re-mute policy for hard resyncs. Mute-until-synced exists because CONVERGENCE
// splices repeatedly for seconds, which is audible as a correction storm -- not
// because any single splice is. A one-shot catch-up is a |error| ms skip; muting for
// it costs a full re-lock, measured at 7.2-13.3 s on this fleet. Dropping two chunks
// beats seven seconds of silence, so a lone excursion now corrects audibly.
//
// The threshold is read off the logs, which separate cleanly by two orders of
// magnitude: genuine one-shots produced 1, 2, 3 and 5 resyncs, while every real
// storm produced 29, 37, 67, 101 or a window-saturating 128. Nothing landed in
// between, so 8 sits in an empty gap rather than on a guess.
// Self-repair for a split between the ACCOUNTED queue (pushed-played) and the
// MEASURED fill the sink reports. A split is a silent, permanent timing offset: the
// prediction is wrong by the difference, the servo steers real audio to the wrong time,
// and every on-device metric agrees with itself because the error is measured against
// that same prediction. Observed on hardware: a starvation re-baseline anchored to the
// sink's reported fill, which excludes whatever the mixer's output ring and the I2S DMA
// held at that instant, so the clamp in notify_audio_played() then permanently absorbed
// the shortfall -- drift stepped +8 -> +51 ms and stayed there, playing ~43 ms early.
//
// Drift that counts as a split.
//
// A repair subtracts its whole magnitude from the accounted queue in one step, and the
// servo then walks that back audibly -- measured in the field as corrections of -66 to
// -220 ms, one per repair, every repair. So this must only fire on a number that is
// certainly right, and being slow to act is far cheaper than acting on noise.
static constexpr int32_t DRIFT_REPAIR_US = 20000;
// A real split is STEADY: the original was observed rock-steady at +50.7 ms for 18
// minutes. Measurement artefacts are not -- with a mixer in the chain, drift sawtooths
// between ~0 and -100 ms as a source ring fills and drains, and a threshold test alone
// happily fires on the peaks. Requiring the spread across the hold window to stay inside
// this band is what distinguishes the two, and it is the property the hold was always
// meant to test.
static constexpr int32_t DRIFT_STEADY_BAND_US = 10000;
// Held this long before acting: a real split is rock-steady (18 minutes at +50.7),
// while a refill transient is not, and repairing a transient would inject the error
// it is meant to remove.
static constexpr int64_t DRIFT_REPAIR_HOLD_US = 10000000;

static constexpr uint32_t RESYNC_STORM_COUNT = 8;
static constexpr int64_t RESYNC_STORM_WINDOW_US = 2000000;

#ifdef CLOCK_SYNC_TSF_ACTIVE
// TSF unicast roster refresh cadence (only while no stream is active; blocking RPC)
static constexpr int64_t TSF_PEER_REFRESH_US = 60000000;
#endif

// Time-sync RTT gating (see handle_time_reply_)
static constexpr int64_t RTT_GATE_US = 20000;      // reject samples this far above the floor
static constexpr int64_t RTT_FLOOR_LEAK_US = 500;  // floor rises this much per sample (~0.5 ms/s)

static inline int64_t now_us() { return esp_timer_get_time(); }

int64_t SnapcastClient::now_us_public() { return now_us(); }

SnapcastClient::~SnapcastClient() {
  // Components are never destructed in practice; best-effort teardown.
  this->shutdown_.store(true, std::memory_order_relaxed);
  this->close_socket_();
  if (this->network_task_handle_ != nullptr) {
    vTaskDelete(this->network_task_handle_);
  }
  if (this->player_task_handle_ != nullptr) {
    vTaskDelete(this->player_task_handle_);
  }
}

bool SnapcastClient::start() {
  this->pcm_ring_ = ring_buffer::RingBuffer::create(this->config_.buffer_size);
  if (this->pcm_ring_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %zu byte PCM buffer", this->config_.buffer_size);
    return false;
  }
  this->slice_buffer_ = std::make_unique<uint8_t[]>(SLICE_BUFFER_SIZE);

#ifdef USE_I2S_RATE_LOCK
  this->rate_lock_ = std::make_unique<RateLock>(this->config_.rate_lock_i2s_port);
#endif

#ifdef CLOCK_SYNC_TSF_ACTIVE
  // Plausibility gate mirrors the hard-resync threshold: a shared mapping that far
  // from our own estimate would hard-resync us -- reject it instead
  this->tsf_sync_ =
      std::make_unique<TsfSync>(static_cast<int64_t>(this->config_.hard_resync_threshold_ms) * 1000);
#endif

  this->control_session_ = std::make_unique<ControlSession>(this->config_.client_id);

  this->event_queue_ = xQueueCreate(8, sizeof(Event));
  this->record_queue_ = xQueueCreate(160, sizeof(ChunkRecord));
  if (this->event_queue_ == nullptr || this->record_queue_ == nullptr) {
    return false;
  }

  // The player outranks the network task so decode bursts cannot starve playout.
  if (xTaskCreate(SnapcastClient::player_task_trampoline, "snap_player", 6144, this, 8, &this->player_task_handle_) !=
      pdPASS) {
    return false;
  }
  if (xTaskCreate(SnapcastClient::network_task_trampoline, "snap_net", 8192, this, 5, &this->network_task_handle_) !=
      pdPASS) {
    return false;
  }
  return true;
}

// THREAD CONTEXT: Main loop (called from the hub's loop())
void SnapcastClient::loop() {
  Event event;
  while (xQueueReceive(this->event_queue_, &event, 0) == pdTRUE) {
    if (this->listener_ == nullptr) {
      continue;
    }
    switch (event.type) {
      case EventType::CONNECTED:
        this->listener_->on_connection_changed(true);
        break;
      case EventType::DISCONNECTED:
        this->listener_->on_connection_changed(false);
        break;
      case EventType::SERVER_SETTINGS:
        this->settings_main_ = event.settings;
        this->listener_->on_server_settings(event.settings);
        break;
      case EventType::STREAM_START:
        this->listener_->on_stream_start(event.params);
        break;
      case EventType::STREAM_END:
        this->listener_->on_stream_end();
        break;
    }
  }

  // Discovered-server list: built on the network task, handed over under the mutex
  // (its strings can't ride the byte-copying event queue)
  this->server_mutex_.lock();
  const bool dirty = this->discovered_dirty_;
  std::vector<ServerCandidate> servers;
  if (dirty) {
    servers = this->discovered_servers_;
    this->discovered_dirty_ = false;
  }
  this->server_mutex_.unlock();
  if (dirty && this->listener_ != nullptr) {
    this->listener_->on_servers_discovered(servers);
  }

  // Stream metadata from the control session (network task -> main loop handoff)
  if (this->control_session_ != nullptr && this->listener_ != nullptr) {
    StreamMetadata metadata;
    if (this->control_session_->take_metadata(metadata)) {
      this->listener_->on_stream_metadata(metadata);
    }
  }
}

// THREAD CONTEXT: Main loop
void SnapcastClient::set_output_active(bool active) {
  if (active && !this->output_active_.load(std::memory_order_relaxed)) {
    // The downstream pipeline restarts when the source is (re)activated, so frames
    // pushed in an earlier session no longer relate to the playback feedback.
    this->playout_mutex_.lock();
    this->playout_valid_ = false;
    this->played_frames_total_ = 0;
    this->pushed_frames_total_ = 0;
    this->fb_samples_ = 0;
    this->clear_playout_history_();
    this->playout_mutex_.unlock();
  }
  this->output_active_.store(active, std::memory_order_relaxed);
}

// THREAD CONTEXT: Main loop
void SnapcastClient::set_server_latency(int32_t latency_ms) {
  this->client_info_mutex_.lock();
  this->latency_dirty_ = true;
  this->latency_pending_ms_ = latency_ms;
  this->client_info_mutex_.unlock();
}

// THREAD CONTEXT: Main loop
void SnapcastClient::send_client_info(uint8_t volume_percent, bool muted) {
  this->client_info_mutex_.lock();
  this->client_info_dirty_ = true;
  this->client_info_volume_ = volume_percent;
  this->client_info_muted_ = muted;
  this->client_info_mutex_.unlock();
}

// THREAD CONTEXT: Main loop
void SnapcastClient::set_server_override(const std::string &host, uint16_t port) {
  this->server_mutex_.lock();
  const bool changed = host != this->override_host_ || port != this->override_port_;
  this->override_host_ = host;
  this->override_port_ = port;
  this->server_mutex_.unlock();
  if (changed) {
    // Drop the current session; the network task reconnects to the new target
    // (or back to configured/discovered when the override was cleared)
    this->reconnect_requested_.store(true, std::memory_order_relaxed);
  }
}

// Ring append. Newest wins on a tie: two events at the same microsecond are ordered by arrival, and
// the later one carries the later level.
void SnapcastClient::mark_playout_(PlayoutMark *history, size_t &next, int64_t ts_us, int64_t total) {
  history[next] = PlayoutMark{ts_us, total};
  next = (next + 1) % PLAYOUT_HISTORY;
}

void SnapcastClient::clear_playout_history_() {
  for (size_t i = 0; i < PLAYOUT_HISTORY; i++) {
    this->pushed_history_[i] = PlayoutMark{0, 0};
    this->played_history_[i] = PlayoutMark{0, 0};
  }
  this->pushed_history_next_ = 0;
  this->played_history_next_ = 0;
}

// Newest mark at or before `as_of_us`. Scans the whole ring rather than assuming it is sorted: the
// two histories are each monotone in their own timestamps, but nothing guarantees a caller's `as_of`
// falls after the oldest slot, and a linear scan of 32 entries twice per report is free.
bool SnapcastClient::playout_level_at_(const PlayoutMark *history, int64_t as_of_us, int64_t &total) {
  int64_t best_ts = 0;
  bool found = false;
  for (size_t i = 0; i < PLAYOUT_HISTORY; i++) {
    if (history[i].ts_us == 0 || history[i].ts_us > as_of_us) {
      continue;
    }
    if (!found || history[i].ts_us >= best_ts) {
      best_ts = history[i].ts_us;
      total = history[i].total;
      found = true;
    }
  }
  return found;
}

bool SnapcastClient::accounted_at_(int64_t as_of_us, int64_t &frames) const {
  int64_t pushed_at = 0;
  int64_t played_at = 0;
  if (!playout_level_at_(this->pushed_history_, as_of_us, pushed_at) ||
      !playout_level_at_(this->played_history_, as_of_us, played_at)) {
    // The reading is older than anything we still remember, so there is no honest comparison to
    // make. Refusing is the only safe answer: substituting the current levels is exactly the bug.
    return false;
  }

  // Both counters are read as STEP functions, and `played` deliberately so. Interpolating `played`
  // between credits at the nominal rate was tried, on the theory that the DAC drains continuously
  // while credits arrive in lumps. It is wrong: a sink's reported depth steps at the same instants
  // its credits do -- both are updated from the same task iteration -- so the stepped reconstruction
  // is already aligned with what is being compared against. Interpolating breaks that alignment.
  // Measured in QEMU, it took a drift of 0 to -1 us and made it -9209 us median.
  frames = pushed_at - played_at;
  return true;
}

// THREAD CONTEXT: Speaker playback callback thread
void SnapcastClient::notify_audio_played(uint32_t frames, int64_t timestamp_us) {
  bool rebaselined = false;
  this->playout_mutex_.lock();
  if (this->playout_valid_) {
    // A gap well beyond the speaker's DMA cadence means the DAC was starved
    // (pipeline underrun); surfaced in the periodic sync report for diagnostics
    const int64_t gap = timestamp_us - this->played_last_ts_us_;
#ifdef USE_SNAPCLIENT_TIMING_DIAG
    // Report-only. `gap` itself is functional below (the flush detector), but its max and
    // mean exist purely for the sync report.
    this->max_feedback_gap_us_ = std::max(this->max_feedback_gap_us_, gap);
    if (gap > 0 && gap < PIPELINE_FLUSH_GAP_US) {
      // Flush-sized gaps are outages, not cadence; they would skew the mean
      this->fb_gap_sum_us_ += gap;
      this->fb_gap_count_++;
    }
#endif  // USE_SNAPCLIENT_TIMING_DIAG
    if (gap > PIPELINE_FLUSH_GAP_US) {
      // The pipeline stopped and restarted; the orchestrator recreates its ring on
      // restart, silently DISCARDING pushed-but-unplayed frames. Without this
      // re-baseline, those frames stay counted as queued and the prediction is
      // permanently late by the discarded amount — every chunk hard-drops, which
      // starves the pipeline into another flush: an unrecoverable death spiral.
      // At resume the pipeline is empty, so played == pushed is ground truth.
      //
      // Deliberately NOT switched to the measured-fill anchor the starvation path now
      // uses. Two reasons: this path's premise is observed rather than assumed (a
      // >500 ms feedback gap IS a stop and restart, and a restart discards), and this
      // runs on the speaker callback thread while on_query_latency() is documented
      // player-task-only. Querying from here would break that contract to replace a
      // sound premise.
      this->pushed_frames_total_ = this->played_frames_total_ + frames;
      this->fb_samples_ = 0;
      rebaselined = true;
#ifdef USE_I2S_RATE_LOCK
      // The pipeline restart may have reprogrammed the I2S clock divider; re-read
      // the baseline before the next trim (the requested trim itself stays valid --
      // it is the learned crystal offset, a property of the hardware)
      if (this->rate_lock_ != nullptr) {
        this->rate_lock_->invalidate_baseline();
      }
#endif
    }
  }
  // Physical invariant: played can never exceed pushed. During a source starvation
  // the pipeline keeps reporting playback progress (it is playing fill, not our
  // audio) with no feedback gap, so the flush detector cannot fire; counting those
  // phantom frames permanently offsets the accounting by the starvation length
  // (observed: pipeline depth -268 ms during a receive stall, then a clean-looking
  // steady state playing ~230 ms audibly late). Clamp the excess -- the pivot then
  // tracks the stall truthfully and accounting stays exact through recovery.
  const int64_t available_frames = this->pushed_frames_total_ - this->played_frames_total_;
  if (static_cast<int64_t>(frames) > available_frames) {
    if (available_frames <= 0 && frames > 0 && !this->starved_latched_) {
      // Complete drain: the framework tears its pipeline down and restarts it with
      // an unpredictable buffer fill level between this feedback point and the DAC
      // -- invisible to the accounting, so playback would settle audibly offset
      // (~100-250 ms observed) with clean-looking sync reports. Flag the player to
      // re-baseline from scratch when audio resumes. Latched: every zero-clamped
      // callback during one drain would otherwise re-fire the flag and the player
      // would re-baseline in a tight loop (observed: 9 resets in 261 ms).
      this->starved_latched_ = true;
      this->pipeline_starved_.store(true, std::memory_order_relaxed);
    }
    // TEMPORARY DIAGNOSTIC: attribute the silent half of this clamp. Time-throttled because this
    // runs on the speaker callback thread at the DMA cadence, and a pathological case would
    // otherwise flood the link.
    const int64_t dbg_excess = static_cast<int64_t>(frames) - std::max<int64_t>(available_frames, 0);
    this->dbg_clamped_frames_ += dbg_excess;
    this->dbg_clamp_events_++;
    if (timestamp_us - this->dbg_clamp_last_log_us_ >= 1000000) {
      this->dbg_clamp_last_log_us_ = timestamp_us;
      ESP_LOGW(TAG,
               "CLAMPDBG discarded %" PRId64 " frames (credit %" PRIu32 ", available %" PRId64 "); total %" PRId64
               " frames in %" PRIu32 " events",
               dbg_excess, frames, available_frames, this->dbg_clamped_frames_, this->dbg_clamp_events_);
    }
    frames = static_cast<uint32_t>(std::max<int64_t>(available_frames, 0));
  } else if (frames > 0) {
    this->starved_latched_ = false;  // real audio flowing again
  }
  this->played_frames_total_ += frames;
  this->played_last_ts_us_ = timestamp_us;
  this->playout_valid_ = true;

  // Record the new level against the instant the audio RENDERED, not the instant this callback ran:
  // a sink reading stamped `as_of` excludes exactly the frames that had rendered by `as_of`, so the
  // level has to be attributed the same way for the two to line up. A re-baseline discards the past,
  // so its history goes with it -- an older level says nothing about the counters after it.
  if (rebaselined) {
    this->clear_playout_history_();
    this->mark_playout_(this->pushed_history_, this->pushed_history_next_, timestamp_us, this->pushed_frames_total_);
  }
  this->mark_playout_(this->played_history_, this->played_history_next_, timestamp_us, this->played_frames_total_);

  // Exponentially-weighted means of (frame index, DAC time); see the member comment.
  // Prediction extrapolates through this pivot along the exact nominal sample rate:
  // fitting a slope too would amplify its estimation noise over the pivot-to-now
  // lever arm into millisecond wobble, while real DAC-vs-esp_timer clock drift only
  // moves the pivot slowly — which the steering servo absorbs by design.
  // This is the dominant term in the loop lag that bounds the trim gain for phase
  // margin. A muted-only 1/8 was tried, to buy a higher muted KP; convergence got
  // worse and it was reverted (see the trim-gain comment). Note the time constant
  // depends on the callback interval, which has never actually been measured --
  // measure it before deriving any gain from this value.
  constexpr double ALPHA = 1.0 / 64.0;
  const double f = static_cast<double>(this->played_frames_total_);
  const double t = static_cast<double>(timestamp_us);
  if (this->fb_samples_ == 0) {
    this->fb_mean_frames_ = f;
    this->fb_mean_ts_ = t;
  } else {
    this->fb_mean_frames_ += ALPHA * (f - this->fb_mean_frames_);
    this->fb_mean_ts_ += ALPHA * (t - this->fb_mean_ts_);
  }
  this->fb_samples_++;
  this->playout_mutex_.unlock();
}

// THREAD CONTEXT: Main loop (diagnostics)
bool SnapcastClient::audio_flowing() const {
  const int64_t last = this->last_chunk_us_.load(std::memory_order_relaxed);
  if (last == 0) {
    return false;
  }
  return now_us() - last < static_cast<int64_t>(this->config_.stream_idle_timeout_ms) * 1000;
}

float SnapcastClient::get_clock_offset_ms() {
  this->filter_mutex_.lock();
  float offset = this->time_filter_.has_estimate()
                     ? static_cast<float>(this->time_filter_.get_offset(now_us() / 1000.0))
                     : 0.0f;
  this->filter_mutex_.unlock();
  return offset;
}

// ============================== Network task ==============================

void SnapcastClient::network_task_() {
  while (!this->shutdown_.load(std::memory_order_relaxed)) {
    if (!this->network_ready_.load(std::memory_order_relaxed)) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    this->connection_session_();
    // No backoff when the session ended because the user changed the target
    if (!this->shutdown_.load(std::memory_order_relaxed) &&
        !this->reconnect_requested_.load(std::memory_order_relaxed)) {
      vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
  }
  vTaskDelete(nullptr);
}

void SnapcastClient::connection_session_() {
  this->reconnect_requested_.store(false, std::memory_order_relaxed);

  // Target precedence: override (manual/select entities) > configured server > mDNS
  std::string host;
  uint16_t port = this->config_.server_port;
  this->server_mutex_.lock();
  if (!this->override_host_.empty()) {
    host = this->override_host_;
    if (this->override_port_ != 0) {
      port = this->override_port_;
    }
  }
  this->server_mutex_.unlock();
  if (host.empty()) {
    host = this->config_.server_host;
  }

  // Scan on every attempt while no target is known -- a boot-time scan often comes
  // up empty (mDNS races the network coming up) and must retry on the reconnect
  // cadence, not a scan timer. When a target IS known and the server select wants
  // the list, refresh it opportunistically on reconnects (rate-limited -- each scan
  // blocks ~3 s). An empty scan keeps the previous list (last-known-good fallback).
  const bool need_discovery = host.empty();
  if (need_discovery ||
      (this->discovery_enabled_.load(std::memory_order_relaxed) &&
       (this->last_scan_us_ == 0 || now_us() - this->last_scan_us_ >= SERVER_SCAN_MIN_INTERVAL_US))) {
    this->scan_servers_();
  }
  if (need_discovery) {
    this->server_mutex_.lock();
    if (!this->discovered_servers_.empty()) {
      host = this->discovered_servers_[0].host;
      port = this->discovered_servers_[0].port;
    }
    this->server_mutex_.unlock();
    if (host.empty()) {
      return;  // nothing found; retried after the reconnect delay
    }
    ESP_LOGI(TAG, "Discovered snapserver at %s:%u", host.c_str(), port);
  }
  this->active_host_ = host;
  this->server_id_hash_ = fnv1_hash(host + ":" + std::to_string(port));
#ifdef CLOCK_SYNC_TSF_ACTIVE
  this->last_peer_refresh_us_ = 0;  // fresh roster per session
#endif

  if (!this->connect_socket_(host, port)) {
    this->close_socket_();
    return;
  }

  ESP_LOGI(TAG, "Connected to %s:%u", host.c_str(), port);
  this->connected_.store(true, std::memory_order_relaxed);
  this->post_event_(Event{.type = EventType::CONNECTED});

  // Reset per-connection state. The Kalman filter's learned noise estimate survives
  // reset() by design, so re-sync converges quickly.
  this->filter_mutex_.lock();
  this->time_filter_.reset();
  this->filter_mutex_.unlock();
  this->codec_ = Codec::NONE;
  this->stream_params_ = StreamParams{};
  this->time_sync_burst_remaining_ = TIME_SYNC_BURST_COUNT;
  this->next_time_sync_us_ = 0;
  this->last_chunk_us_.store(0, std::memory_order_relaxed);

  std::string hello = build_hello_payload(this->config_.client_id.c_str(), this->config_.hostname);
  bool ok = this->send_message_(MessageType::HELLO, reinterpret_cast<const uint8_t *>(hello.data()), hello.size());

  uint8_t header[BaseMessage::WIRE_SIZE];
  while (ok && !this->shutdown_.load(std::memory_order_relaxed)) {
    if (!this->recv_exact_(header, sizeof(header))) {
      break;
    }
    const int64_t recv_us = now_us();
    BaseMessage base = BaseMessage::deserialize(header);
    if (base.size > MAX_PAYLOAD_SIZE) {
      ESP_LOGE(TAG, "Oversized message: type=%u size=%" PRIu32, base.type, base.size);
      break;
    }
    this->rx_buffer_.resize(base.size);
    if (base.size > 0 && !this->recv_exact_(this->rx_buffer_.data(), base.size)) {
      break;
    }

    switch (static_cast<MessageType>(base.type)) {
      case MessageType::WIRE_CHUNK:
        this->handle_wire_chunk_(this->rx_buffer_.data(), base.size);
        break;
      case MessageType::TIME:
        this->handle_time_reply_(base, this->rx_buffer_.data(), base.size, recv_us);
        break;
      case MessageType::CODEC_HEADER:
        this->handle_codec_header_(this->rx_buffer_.data(), base.size);
        break;
      case MessageType::SERVER_SETTINGS: {
        ServerSettings settings;
        if (ServerSettings::parse(this->rx_buffer_.data(), base.size, settings)) {
          ESP_LOGD(TAG, "Server settings: buffer=%" PRId32 " ms latency=%" PRId32 " ms volume=%u%% muted=%s",
                   settings.buffer_ms, settings.latency, settings.volume, YESNO(settings.muted));
          this->buffer_ms_.store(settings.buffer_ms, std::memory_order_relaxed);
          this->server_latency_ms_.store(settings.latency, std::memory_order_relaxed);
          this->post_event_(Event{.type = EventType::SERVER_SETTINGS, .settings = settings});
        }
        break;
      }
      default:
        ESP_LOGV(TAG, "Unhandled message type %u (%" PRIu32 " bytes)", base.type, base.size);
        break;
    }

    this->service_tx_();
  }

  this->close_socket_();
  if (this->control_session_ != nullptr) {
    this->control_session_->close();  // tied to the stream session's lifecycle
  }
  this->connected_.store(false, std::memory_order_relaxed);
  this->set_stream_active_(false);
  this->post_event_(Event{.type = EventType::DISCONNECTED});
  ESP_LOGW(TAG, "Disconnected from server");
}

// THREAD CONTEXT: Network task. The mDNS query blocks for up to its timeout; the
// espressif mdns library is safe to call from any task once initialized (ESPHome's
// mdns component initializes it when the network comes up, which is before
// network_ready_ lets us get here — a not-yet-ready stack just returns an error and
// we retry after the reconnect delay).
bool SnapcastClient::scan_servers_() {
#ifdef USE_MDNS
  this->last_scan_us_ = now_us();
  mdns_result_t *results = nullptr;
  esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 3000, 8, &results);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS query failed: %d", err);
    return false;
  }
  std::vector<ServerCandidate> servers;
  for (mdns_result_t *r = results; r != nullptr; r = r->next) {
    for (mdns_ip_addr_t *a = r->addr; a != nullptr; a = a->next) {
      if (a->addr.type == ESP_IPADDR_TYPE_V4) {
        char buf[16];
        esp_ip4addr_ntoa(&a->addr.u_addr.ip4, buf, sizeof(buf));
        ServerCandidate c;
        c.host = buf;
        c.port = r->port != 0 ? r->port : this->config_.server_port;
        if (r->instance_name != nullptr && r->instance_name[0] != '\0') {
          c.name = r->instance_name;
        } else if (r->hostname != nullptr && r->hostname[0] != '\0') {
          c.name = r->hostname;
        } else {
          c.name = c.host;
        }
        servers.push_back(std::move(c));
        break;
      }
    }
  }
  mdns_query_results_free(results);
  if (servers.empty()) {
    ESP_LOGW(TAG, "No snapserver found via mDNS (_snapcast._tcp)");
    return false;
  }
  this->server_mutex_.lock();
  // Dirty-flag only real changes so the listener isn't re-notified every reconnect
  if (servers != this->discovered_servers_) {
    this->discovered_servers_ = std::move(servers);
    this->discovered_dirty_ = true;
  }
  this->server_mutex_.unlock();
  return true;
#else
  ESP_LOGE(TAG, "No server configured and mDNS support is not compiled in");
  vTaskDelay(pdMS_TO_TICKS(5000));
  return false;
#endif
}

bool SnapcastClient::connect_socket_(const std::string &host, uint16_t port) {
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%u", port);

  struct addrinfo *res = nullptr;
  int err = getaddrinfo(host.c_str(), port_str, &hints, &res);
  if (err != 0 || res == nullptr) {
    ESP_LOGW(TAG, "DNS lookup for '%s' failed: %d", host.c_str(), err);
    return false;
  }

  this->sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (this->sock_ < 0) {
    freeaddrinfo(res);
    return false;
  }

  // Non-blocking connect with timeout
  int flags = fcntl(this->sock_, F_GETFL, 0);
  fcntl(this->sock_, F_SETFL, flags | O_NONBLOCK);
  err = connect(this->sock_, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (err != 0 && errno != EINPROGRESS) {
    ESP_LOGW(TAG, "Connect to %s failed: errno %d", host.c_str(), errno);
    return false;
  }
  if (err != 0) {
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(this->sock_, &write_set);
    struct timeval timeout = {.tv_sec = CONNECT_TIMEOUT_MS / 1000, .tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000};
    if (select(this->sock_ + 1, nullptr, &write_set, nullptr, &timeout) <= 0) {
      ESP_LOGW(TAG, "Connect to %s timed out", host.c_str());
      return false;
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(this->sock_, SOL_SOCKET, SO_ERROR, &so_error, &len);
    if (so_error != 0) {
      ESP_LOGW(TAG, "Connect to %s failed: errno %d", host.c_str(), so_error);
      return false;
    }
  }
  fcntl(this->sock_, F_SETFL, flags);

  // Time-sync accuracy benefits from unbatched small messages
  int nodelay = 1;
  setsockopt(this->sock_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  return true;
}

void SnapcastClient::close_socket_() {
  if (this->sock_ >= 0) {
    close(this->sock_);
    this->sock_ = -1;
  }
}

bool SnapcastClient::send_message_(MessageType type, const uint8_t *payload, size_t len, uint16_t refers_to) {
  BaseMessage base;
  base.type = static_cast<uint16_t>(type);
  base.id = ++this->next_message_id_;
  base.refers_to = refers_to;
  base.sent = TimeVal::from_us(now_us());
  base.size = len;

  uint8_t header[BaseMessage::WIRE_SIZE];
  base.serialize(header);

  // Header and payload in one buffer so TCP_NODELAY doesn't split them into two segments
  uint8_t stack_buf[BaseMessage::WIRE_SIZE + TimePayload::WIRE_SIZE];
  const uint8_t *buf;
  std::vector<uint8_t> heap_buf;
  size_t total = sizeof(header) + len;
  if (total <= sizeof(stack_buf)) {
    memcpy(stack_buf, header, sizeof(header));
    if (len > 0) {
      memcpy(stack_buf + sizeof(header), payload, len);
    }
    buf = stack_buf;
  } else {
    heap_buf.resize(total);
    memcpy(heap_buf.data(), header, sizeof(header));
    memcpy(heap_buf.data() + sizeof(header), payload, len);
    buf = heap_buf.data();
  }

  size_t sent = 0;
  while (sent < total) {
    int n = send(this->sock_, buf + sent, total - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += n;
  }
  return true;
}

bool SnapcastClient::recv_exact_(uint8_t *buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    if (this->shutdown_.load(std::memory_order_relaxed) || this->sock_ < 0 ||
        this->reconnect_requested_.load(std::memory_order_relaxed)) {
      return false;
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(this->sock_, &read_set);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    int ready = select(this->sock_ + 1, &read_set, nullptr, nullptr, &timeout);
    if (ready < 0) {
      return false;
    }
    if (ready == 0) {
      // Idle wait: keep time sync and ClientInfo flowing even when no stream is playing
      this->service_tx_();
      continue;
    }
    int n = recv(this->sock_, buf + got, len - got, 0);
    if (n <= 0) {
      return false;
    }
    got += n;
  }
  return true;
}

void SnapcastClient::service_tx_() {
  const int64_t now = now_us();

  // Time sync request. Adaptive cadence: the configured (fast) interval while a
  // stream is active — playout accuracy is being consumed — and a slow interval while
  // idle, which keeps the clock warm for instant stream starts without denying the
  // modem its power-save sleep windows.
  if (now >= this->next_time_sync_us_) {
    uint8_t payload[TimePayload::WIRE_SIZE];
    TimePayload{}.serialize(payload);
    this->send_message_(MessageType::TIME, payload, sizeof(payload));
    uint32_t interval_ms;
    if (this->time_sync_burst_remaining_ > 0) {
      this->time_sync_burst_remaining_--;
      interval_ms = TIME_SYNC_BURST_INTERVAL_MS;
    } else if (this->stream_active_) {
      interval_ms = this->config_.time_sync_interval_ms;
    } else {
      interval_ms = std::max(this->config_.time_sync_interval_ms, TIME_SYNC_IDLE_INTERVAL_MS);
    }
    this->next_time_sync_us_ = now + static_cast<int64_t>(interval_ms) * 1000;
  }

  // Pending ClientInfo (local volume/mute change)
  this->client_info_mutex_.lock();
  bool dirty = this->client_info_dirty_;
  uint8_t volume = this->client_info_volume_;
  bool muted = this->client_info_muted_;
  this->client_info_dirty_ = false;
  this->client_info_mutex_.unlock();
  if (dirty) {
    std::string payload = build_client_info_payload(volume, muted);
    this->send_message_(MessageType::CLIENT_INFO, reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
  }

  // Pending server-latency change (control API)
  this->client_info_mutex_.lock();
  const bool latency_dirty = this->latency_dirty_;
  const int32_t latency_ms = this->latency_pending_ms_;
  this->latency_dirty_ = false;
  this->client_info_mutex_.unlock();
  if (latency_dirty) {
    // Prefer the persistent control session; one-shot socket as the fallback
    // (control port may answer one-shots even when the session is mid-reconnect)
    if (this->control_session_ == nullptr || !this->control_session_->send_set_latency(latency_ms, now)) {
      this->send_set_latency_rpc_(latency_ms);
    }
  }

  // Persistent control session: metadata + roster + control RPCs (non-blocking)
  if (this->control_session_ != nullptr) {
    this->control_session_->service(now, this->active_host_);
#ifdef CLOCK_SYNC_TSF_ACTIVE
    if (this->tsf_sync_ != nullptr) {
      std::vector<uint32_t> peers;
      if (this->control_session_->take_peers(peers)) {
        this->tsf_sync_->set_peers(std::move(peers));
      }
    }
#endif
  }

  // Stream idle detection: the server stops sending chunks when the group stream
  // goes idle.
  //
  // Two very different things hide behind "no chunks":
  //
  //   An inter-track gap or short pause -- 17 s and 18 s measured here. Ending the
  //   stream tears the pipeline down (media source -> IDLE -> output inactive) and
  //   costs a mute plus 7-16 s of re-lock on resume, for nothing. Two such gaps 29 s
  //   apart produced a 31 ms error that corrected silently and a 61 ms one that
  //   tripped the hard-resync threshold: identical events either side of a coin flip.
  //   Worth bridging with keepalive silence, which keeps playout phase, the frame
  //   accounting and the TSF mapping live so a resuming stream needs no correction.
  //
  //   A finished listening session, where holding on is actively wrong: it keeps the
  //   DAC fed, the radio in high-performance mode and TSF beaconing for hours, and the
  //   accounting goes stale. Held across a 7.5 h overnight silence, resumption came
  //   back with a 24888016 ms stale deadline on both devices within 61 ms of each
  //   other, taking 9.6 s and 16 s to re-lock -- worse than the teardown it avoided.
  //
  // So bridge up to keepalive_hold and release beyond it. stream_idle_timeout still
  // governs the disconnected case, where there is nothing to wait for.
  //
  // keepalive_hold = 0 ("never") holds the pipeline for the whole session, so the
  // speaker stays ready to play in sync. Be aware of what that does NOT buy: the
  // overnight event above happened WITH the pipeline held, so holding it is not by
  // itself sufficient for instant resumption -- the residual cost was the servo
  // settling and a TSF re-election, not the teardown.
  int64_t idle_limit_us;
  if (!this->connected_.load(std::memory_order_relaxed)) {
    idle_limit_us = static_cast<int64_t>(this->config_.stream_idle_timeout_ms) * 1000;
  } else if (this->config_.keepalive_hold_ms == 0) {
    idle_limit_us = INT64_MAX;  // never release
  } else {
    idle_limit_us = static_cast<int64_t>(this->config_.keepalive_hold_ms) * 1000;
  }
  if (this->stream_active_ && now - this->last_chunk_us_.load(std::memory_order_relaxed) > idle_limit_us) {
    ESP_LOGD(TAG, "Stream idle for %" PRIu32 " ms, ending stream", this->config_.stream_idle_timeout_ms);
    this->set_stream_active_(false);
  }

#ifdef CLOCK_SYNC_TSF_ACTIVE
  if (this->tsf_sync_ != nullptr) {
    // Unicast roster: the control session feeds it live (non-blocking); the
    // blocking one-shot fetch remains only as the fallback when the control port
    // is unavailable -- once at session start, then refreshed only off-stream
    // (the RPC blocks up to ~2 s, which mid-stream would starve playout)
    const bool session_feeds_roster = this->control_session_ != nullptr && this->control_session_->connected();
    if (!session_feeds_roster &&
        (this->last_peer_refresh_us_ == 0 ||
         (!this->stream_active_ && now - this->last_peer_refresh_us_ >= TSF_PEER_REFRESH_US))) {
      this->refresh_tsf_peers_();
    }
    // Elections/beacons only while a stream is active: that is when deadlines are
    // computed AND when the hub holds high-performance wifi. While idle, modem
    // power save makes TSF reads fail intermittently (observed: sporadic beacons
    // and "TSF unreadable" role flapping on an idle pair). Roles freeze across
    // idle gaps; the leader resumes beaconing on the first active tick, and stale
    // mappings expire into the Kalman fallback on their own.
    if (this->stream_active_) {
      TsfSync::Estimate est;
      this->filter_mutex_.lock();
      est.valid = this->time_filter_.has_estimate();
      if (est.valid) {
        est.mature = this->time_filter_.is_settled() && this->time_sync_burst_remaining_ == 0;
        est.offset_ms = this->time_filter_.get_offset(now / 1000.0);
        est.drift = this->time_filter_.get_drift();
      }
      this->filter_mutex_.unlock();
      this->tsf_sync_->service(now, est, this->server_id_hash_);
    }
  }
#endif
}

// THREAD CONTEXT: Network task. A short-lived connection to the JSON-RPC control
// port; failures are logged and dropped (the entity re-syncs from the next
// ServerSettings push either way).
void SnapcastClient::send_set_latency_rpc_(int32_t latency_ms) {
  if (this->active_host_.empty()) {
    return;
  }
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  if (getaddrinfo(this->active_host_.c_str(), "1705", &hints, &res) != 0 || res == nullptr) {
    ESP_LOGW(TAG, "Control API DNS lookup failed");
    return;
  }
  int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock >= 0) {
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) == 0) {
      char req[192];
      const int len = snprintf(req, sizeof(req),
                               "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"Client.SetLatency\","
                               "\"params\":{\"id\":\"%s\",\"latency\":%" PRId32 "}}\r\n",
                               this->config_.client_id.c_str(), latency_ms);
      if (send(sock, req, len, 0) == len) {
        char reply[128];
        recv(sock, reply, sizeof(reply), 0);  // best-effort; drained for TCP hygiene
        ESP_LOGD(TAG, "Requested server latency %" PRId32 " ms", latency_ms);
      } else {
        ESP_LOGW(TAG, "Control API send failed");
      }
    } else {
      ESP_LOGW(TAG, "Control API connect to %s:1705 failed", this->active_host_.c_str());
    }
    close(sock);
  }
  freeaddrinfo(res);
}

#ifdef CLOCK_SYNC_TSF_ACTIVE
// THREAD CONTEXT: Network task. Fetches the server's client roster so the TSF
// leader can unicast beacons to every peer -- client-to-client multicast is
// unreliable on many APs (isolation, IGMP snooping, mesh filtering), while unicast
// works wherever snapcast itself does. Blocking (1 s timeouts), so callers only
// invoke it while no stream is active; the session-start call is absorbed by the
// playout buffer.
void SnapcastClient::refresh_tsf_peers_() {
  this->last_peer_refresh_us_ = now_us();  // set even on failure: no hammering
  if (this->active_host_.empty() || this->tsf_sync_ == nullptr) {
    return;
  }
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  if (getaddrinfo(this->active_host_.c_str(), "1705", &hints, &res) != 0 || res == nullptr) {
    return;
  }
  std::string response;
  int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock >= 0) {
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) == 0) {
      static const char REQ[] = "{\"id\":2,\"jsonrpc\":\"2.0\",\"method\":\"Server.GetStatus\"}\r\n";
      if (send(sock, REQ, sizeof(REQ) - 1, 0) == static_cast<ssize_t>(sizeof(REQ) - 1)) {
        // Newline-delimited JSON-RPC; the full status can be several KB
        char buf[512];
        response.reserve(2048);
        while (response.size() < 24576) {
          const int n = recv(sock, buf, sizeof(buf), 0);
          if (n <= 0) {
            break;
          }
          response.append(buf, n);
          if (memchr(buf, '\n', n) != nullptr) {
            break;
          }
        }
      }
    }
    close(sock);
  }
  freeaddrinfo(res);
  if (response.empty()) {
    ESP_LOGD(TAG, "Server.GetStatus fetch failed; TSF peer roster unchanged");
    return;
  }

  // Filtered parse: keep only connected + host.ip of each client
  JsonDocument filter;
  filter["result"]["server"]["groups"][0]["clients"][0]["connected"] = true;
  filter["result"]["server"]["groups"][0]["clients"][0]["host"]["ip"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, response, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    ESP_LOGD(TAG, "Server.GetStatus parse failed; TSF peer roster unchanged");
    return;
  }
  std::vector<uint32_t> peers;
  // Implicit JsonArray/JsonVariant conversions: portable across ArduinoJson 7.x
  // (as<JsonArray>() on nested member proxies fails to compile on some versions);
  // a null array iterates as empty
  JsonArray groups = doc["result"]["server"]["groups"];
  for (JsonVariant group : groups) {
    JsonArray clients = group["clients"];
    for (JsonVariant client : clients) {
      if (!(client["connected"] | false)) {
        continue;
      }
      const char *ip = client["host"]["ip"];
      if (ip == nullptr) {
        continue;
      }
      if (strncmp(ip, "::ffff:", 7) == 0) {
        ip += 7;  // snapserver reports IPv4-mapped IPv6 addresses
      }
      const in_addr_t addr = inet_addr(ip);
      if (addr == INADDR_NONE) {
        continue;  // plain IPv6 or garbage
      }
      // Our own address may be included; the beacon's own-mac check drops it
      peers.push_back(addr);
    }
  }
  this->tsf_sync_->set_peers(std::move(peers));
}
#endif

void SnapcastClient::handle_time_reply_(const BaseMessage &base, const uint8_t *payload, size_t len, int64_t recv_us) {
  TimePayload time_payload;
  if (!TimePayload::parse(payload, len, time_payload)) {
    return;
  }
  // Server reply: payload.latency = client-to-server (server receive - client send),
  // base.sent = server send time. s2c = client receive - server send.
  // The offset measurement (server - client) is (c2s - s2c) / 2.
  const int64_t c2s_us = time_payload.latency.to_us();
  const int64_t s2c_us = recv_us - base.sent.to_us();

  // RTT gate: c2s + s2c is the round trip, with the clock offsets cancelled out.
  // Samples far above the observed minimum are congestion/roam artifacts whose
  // asymmetric delay would walk the offset estimate away from truth — a burst of
  // them (e.g. right after a wifi roam) defeats even the Huber weighting, leaving
  // playback consistently behind until clean samples slowly win back. The floor
  // leaks upward slowly so a genuinely changed network re-baselines within minutes.
  const int64_t rtt_us = c2s_us + s2c_us;
  this->min_rtt_us_ = std::min(rtt_us, this->min_rtt_us_ + RTT_FLOOR_LEAK_US);
  if (rtt_us > this->min_rtt_us_ + RTT_GATE_US) {
    ESP_LOGV(TAG, "Time sync sample rejected: rtt %" PRId64 " us (floor %" PRId64 " us)", rtt_us, this->min_rtt_us_);
    return;
  }

  const double measurement_ms = static_cast<double>(c2s_us - s2c_us) / 2000.0;

  this->filter_mutex_.lock();
  this->time_filter_.insert(measurement_ms, recv_us / 1000.0);
  this->filter_mutex_.unlock();
}

void SnapcastClient::handle_codec_header_(const uint8_t *payload, size_t len) {
  CodecHeaderView header;
  if (!CodecHeaderView::parse(payload, len, header)) {
    ESP_LOGE(TAG, "Malformed codec header");
    return;
  }
  ESP_LOGI(TAG, "Codec: %.*s (%zu byte header)", static_cast<int>(header.codec_len), header.codec,
           header.payload_len);

  // A new codec header means a new stream; anything mid-flight belongs to the old one
  this->set_stream_active_(false);
  this->codec_ = Codec::NONE;
  this->stream_params_ = StreamParams{};
#ifdef USE_SNAPCLIENT_OPUS
  // ~150 KB of decoder state and pseudostack; do not hold it across a codec change
  this->opus_decoder_.reset();
  this->opus_output_.reset();
  this->opus_output_samples_ = 0;
#endif

  if (header.codec_is("pcm")) {
    // Payload is a RIFF/WAVE header; the PCM format lives in the fmt chunk at fixed offsets
    if (header.payload_len < 44) {
      ESP_LOGE(TAG, "PCM codec header too short");
      return;
    }
    const uint8_t *p = header.payload;
    this->stream_params_.channels = p[22] | (p[23] << 8);
    this->stream_params_.sample_rate =
        static_cast<uint32_t>(p[24]) | (p[25] << 8) | (static_cast<uint32_t>(p[26]) << 16) |
        (static_cast<uint32_t>(p[27]) << 24);
    this->stream_params_.bits_per_sample = p[34] | (p[35] << 8);
    if (!this->stream_params_.valid() || this->stream_params_.bits_per_sample != 16) {
      ESP_LOGE(TAG, "Unsupported PCM format: %" PRIu32 " Hz, %u bit, %u ch", this->stream_params_.sample_rate,
               this->stream_params_.bits_per_sample, this->stream_params_.channels);
      this->stream_params_ = StreamParams{};
      return;
    }
    this->codec_ = Codec::PCM;
  } else if (header.codec_is("flac")) {
#ifdef USE_SNAPCLIENT_FLAC
    this->flac_decoder_ = std::make_unique<micro_flac::FLACDecoder>();
    this->flac_header_done_ = false;
    this->flac_input_.assign(header.payload, header.payload + header.payload_len);
    this->flac_output_.clear();
    this->decode_flac_input_(-1);
    if (this->flac_header_done_) {
      this->codec_ = Codec::FLAC;
    } else {
      ESP_LOGE(TAG, "FLAC stream header did not parse");
      this->flac_decoder_.reset();
    }
#else
    ESP_LOGE(TAG, "FLAC stream received but FLAC support is disabled (set `flac: true` on the snapclient component)");
#endif
  } else if (header.codec_is("opus")) {
#ifdef USE_SNAPCLIENT_OPUS
    if (header.payload_len < OPUS_HEADER_SIZE) {
      ESP_LOGE(TAG, "Opus codec header too short (%zu bytes)", header.payload_len);
      return;
    }
    uint32_t magic;
    memcpy(&magic, header.payload, sizeof(magic));
    if (magic != OPUS_HEADER_MAGIC) {
      ESP_LOGE(TAG, "Not an Opus pseudo header (magic 0x%08" PRIX32 ")", magic);
      return;
    }
    uint32_t rate;
    uint16_t bits;
    uint16_t channels;
    memcpy(&rate, header.payload + 4, sizeof(rate));
    memcpy(&bits, header.payload + 8, sizeof(bits));
    memcpy(&channels, header.payload + 10, sizeof(channels));
    // Validate before narrowing into StreamParams' uint8_t fields, or a nonsense
    // header could truncate into a plausible one. opus_decoder_create accepts only
    // these rates; snapserver always resamples to 48 kHz/16-bit stereo for Opus, so
    // anything else here means a header we do not understand.
    const bool rate_ok = rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
    if (!rate_ok || bits != 16 || channels < 1 || channels > 2) {
      ESP_LOGE(TAG, "Unsupported Opus format: %" PRIu32 " Hz, %u bit, %u ch", rate, bits, channels);
      return;
    }
    this->stream_params_.sample_rate = rate;
    this->stream_params_.bits_per_sample = static_cast<uint8_t>(bits);
    this->stream_params_.channels = static_cast<uint8_t>(channels);
    int error = OPUS_OK;
    this->opus_decoder_.reset(opus_decoder_create(static_cast<opus_int32>(rate), channels, &error));
    if (this->opus_decoder_ == nullptr || error != OPUS_OK) {
      ESP_LOGE(TAG, "Failed to create Opus decoder: %s", opus_strerror(error));
      this->opus_decoder_.reset();
      this->stream_params_ = StreamParams{};
      return;
    }
    const size_t samples = static_cast<size_t>(rate) / 1000 * OPUS_MAX_PACKET_MS * channels;
    RAMAllocator<int16_t> allocator;
    this->opus_output_.reset(allocator.allocate(samples));
    if (this->opus_output_ == nullptr) {
      ESP_LOGE(TAG, "No memory for the %zu byte Opus packet buffer (%" PRIu32 " bytes free)",
               samples * sizeof(int16_t), static_cast<uint32_t>(esp_get_free_heap_size()));
      this->opus_decoder_.reset();
      this->stream_params_ = StreamParams{};
      return;
    }
    this->opus_output_samples_ = samples;
    this->codec_ = Codec::OPUS;
#else
    ESP_LOGE(TAG, "Opus stream received but Opus support is disabled (set `opus: true` on the snapclient component)");
#endif
  } else {
    ESP_LOGE(TAG, "Unsupported codec '%.*s' — set the snapserver stream codec to flac, pcm or opus",
             static_cast<int>(header.codec_len), header.codec);
  }

  if (this->codec_ != Codec::NONE) {
    ESP_LOGI(TAG, "Stream format: %" PRIu32 " Hz, %u bit, %u ch", this->stream_params_.sample_rate,
             this->stream_params_.bits_per_sample, this->stream_params_.channels);
  }
}

void SnapcastClient::handle_wire_chunk_(const uint8_t *payload, size_t len) {
  WireChunkView chunk;
  if (!WireChunkView::parse(payload, len, chunk)) {
    return;
  }
  if (this->codec_ == Codec::NONE || !this->stream_params_.valid()) {
    return;
  }

  this->last_chunk_us_.store(now_us(), std::memory_order_relaxed);
  if (!this->stream_active_) {
    this->set_stream_active_(true);
  }

  const int64_t server_ts_us = chunk.timestamp.to_us();
  switch (this->codec_) {
    case Codec::PCM:
      this->emit_pcm_(chunk.payload, chunk.payload_len, server_ts_us);
      break;
#ifdef USE_SNAPCLIENT_FLAC
    case Codec::FLAC:
      this->flac_input_.insert(this->flac_input_.end(), chunk.payload, chunk.payload + chunk.payload_len);
      this->decode_flac_input_(server_ts_us);
      break;
#endif
#ifdef USE_SNAPCLIENT_OPUS
    case Codec::OPUS:
      this->decode_opus_packet_(chunk.payload, chunk.payload_len, server_ts_us);
      break;
#endif
    default:
      break;
  }
}

#ifdef USE_SNAPCLIENT_FLAC
void SnapcastClient::decode_flac_input_(int64_t server_ts_us) {
  // snapserver flushes the FLAC encoder at every chunk boundary, so in practice each
  // wire chunk decodes to exactly its own samples. The carry-over buffer makes frame
  // spans across chunk boundaries safe anyway; output emitted while processing a chunk
  // is stamped with that chunk's timestamp.
  size_t offset = 0;
  while (offset < this->flac_input_.size()) {
    size_t bytes_consumed = 0;
    size_t samples_decoded = 0;
    micro_flac::FLACDecoderResult result = this->flac_decoder_->decode(
        this->flac_input_.data() + offset, this->flac_input_.size() - offset, this->flac_output_.data(),
        this->flac_output_.size(), bytes_consumed, samples_decoded);

    if (result == micro_flac::FLAC_DECODER_SUCCESS) {
      offset += bytes_consumed;
      if (samples_decoded > 0 && server_ts_us >= 0) {
        const size_t bytes = samples_decoded * (this->stream_params_.bits_per_sample / 8);
        this->emit_pcm_(this->flac_output_.data(), bytes, server_ts_us);
      }
    } else if (result == micro_flac::FLAC_DECODER_HEADER_READY) {
      offset += bytes_consumed;
      const auto &info = this->flac_decoder_->get_stream_info();
      this->stream_params_.sample_rate = info.sample_rate();
      this->stream_params_.bits_per_sample = info.bits_per_sample();
      this->stream_params_.channels = info.num_channels();
      this->flac_output_.resize(this->flac_decoder_->get_output_buffer_size_samples() * info.bytes_per_sample());
      this->flac_header_done_ = true;
    } else if (result == micro_flac::FLAC_DECODER_ERROR_OUTPUT_TOO_SMALL) {
      const auto &info = this->flac_decoder_->get_stream_info();
      this->flac_output_.resize(this->flac_decoder_->get_output_buffer_size_samples() * info.bytes_per_sample());
    } else if (result == micro_flac::FLAC_DECODER_NEED_MORE_DATA) {
      offset += bytes_consumed;
      break;
    } else {
      ESP_LOGW(TAG, "FLAC decode error %d, discarding buffered input", static_cast<int>(result));
      offset = this->flac_input_.size();
      break;
    }
  }
  this->flac_input_.erase(this->flac_input_.begin(), this->flac_input_.begin() + offset);
}
#endif

#ifdef USE_SNAPCLIENT_OPUS
void SnapcastClient::decode_opus_packet_(const uint8_t *data, size_t len, int64_t server_ts_us) {
  // One wire chunk is exactly one opus_encode() output (snapserver splits every PCM
  // chunk into whole 60/40/20/10 ms packets and carries the remainder itself), so
  // there is nothing to buffer across chunks and each packet keeps its own timestamp.
  const int max_frames = static_cast<int>(this->opus_output_samples_ / this->stream_params_.channels);
  const int frames = opus_decode(this->opus_decoder_.get(), data, static_cast<opus_int32>(len),
                                 this->opus_output_.get(), max_frames, 0);
  if (frames < 0) {
    // The transport is TCP, so this is a malformed packet rather than a lost one:
    // dropping it (instead of asking for packet-loss concealment) keeps the decoder
    // state honest, and the servo absorbs the resulting gap.
    ESP_LOGW(TAG, "Opus decode error %s, discarding %zu byte packet", opus_strerror(frames), len);
    return;
  }
  const size_t bytes = static_cast<size_t>(frames) * this->stream_params_.channels * sizeof(int16_t);
  this->emit_pcm_(reinterpret_cast<const uint8_t *>(this->opus_output_.get()), bytes, server_ts_us);
}
#endif

void SnapcastClient::emit_pcm_(const uint8_t *data, size_t len, int64_t server_ts_us) {
  if (len == 0) {
    return;
  }
  // Write the PCM first, then post the record: the player may then rely on a popped
  // record's bytes being fully present in the ring. A full ring blocks here, which
  // backpressures the TCP connection exactly like a desktop snapclient.
  size_t written = 0;
  while (written < len && !this->shutdown_.load(std::memory_order_relaxed)) {
    written += this->pcm_ring_->write_without_replacement(data + written, len - written, pdMS_TO_TICKS(100));
  }
  if (written < len) {
    return;
  }
  ChunkRecord record{.server_ts_us = server_ts_us, .bytes = static_cast<uint32_t>(len), .params = this->stream_params_};
  while (xQueueSend(this->record_queue_, &record, pdMS_TO_TICKS(100)) != pdTRUE) {
    if (this->shutdown_.load(std::memory_order_relaxed)) {
      return;
    }
  }
}

void SnapcastClient::post_event_(const Event &event) {
  // The main loop drains this queue every iteration; drop rather than block if it
  // somehow backs up.
  xQueueSend(this->event_queue_, &event, pdMS_TO_TICKS(50));
}

void SnapcastClient::set_stream_active_(bool active) {
  if (active == this->stream_active_) {
    return;
  }
  this->stream_active_ = active;
  if (active) {
    // Accuracy starts being consumed now: engage the fast cadence immediately with a
    // short burst to refresh the estimate after a possibly long idle stretch
    this->time_sync_burst_remaining_ = std::max<uint32_t>(this->time_sync_burst_remaining_, 3);
    this->next_time_sync_us_ = 0;
    this->post_event_(Event{.type = EventType::STREAM_START, .params = this->stream_params_});
  } else {
    this->post_event_(Event{.type = EventType::STREAM_END});
  }
}

// ============================== Player task ==============================

void SnapcastClient::player_task_() {
  // All servo state lives in one struct so the loop below can be split into named
  // steps; see ServoState for what each field is and why. rate_lock_ok starts from
  // whether the hardware lock exists at all.
  ServoState st;
#ifdef USE_I2S_RATE_LOCK
  st.rate_lock_ok = this->rate_lock_ != nullptr;
#endif
  while (!this->shutdown_.load(std::memory_order_relaxed)) {
    ChunkRecord rec;
    if (xQueueReceive(this->record_queue_, &rec, pdMS_TO_TICKS(100)) != pdTRUE) {
      // No chunk available: keep the downstream pipeline FED rather than idle.
      // The speaker and mixer stop themselves after `timeout` (500 ms default)
      // without data, which tears down and rebuilds their ring buffers -- turning
      // a sub-second delivery hiccup into a multi-second dropout (pipeline
      // restart, unobservable refill, accounting re-baseline, mute, re-lock).
      // Silence occupies exactly the missing duration, so the playout timeline
      // stays continuous and a live stream resumes with no correction at all.
      // Runs for as long as the session is up -- an inter-track gap is measured in
      // tens of seconds and any cap short of that reintroduces the teardown.
      if (st.keepalive_params.valid() && this->output_active_.load(std::memory_order_relaxed) &&
          this->is_connected()) {
        const uint32_t frames = st.keepalive_params.sample_rate / (1000000 / KEEPALIVE_SLICE_US);
        this->push_silence_(frames, st.keepalive_params);
      }
      continue;
    }
    st.keepalive_params = rec.params;

    const uint32_t frame_bytes = rec.params.frame_bytes();
    if (frame_bytes == 0) {
      this->discard_ring_bytes_(rec.bytes);
      continue;
    }

#ifdef USE_I2S_RATE_LOCK
    if (st.rate_lock_ok && rec.params.sample_rate != st.rate_lock_rate) {
      // The speaker reprograms the I2S clock for a new stream format; re-read the
      // divider baseline once the new clock is running. The rate is what lets the
      // lock compute the IDEAL divider rather than inherit the driver's rounding of
      // it, which can otherwise eat the servo's whole trim authority.
      this->rate_lock_->set_output_rate(rec.params.sample_rate);
      this->rate_lock_->invalidate_baseline();
      st.rate_lock_rate = rec.params.sample_rate;
    }
#endif

    if (!this->output_active_.load(std::memory_order_relaxed)) {
      // No consumer: discard immediately. New chunks arrive continuously, so playback
      // starts in sync as soon as the source is activated.
      this->discard_ring_bytes_(rec.bytes);
      continue;
    }

    this->rebaseline_after_starvation_(st, rec, frame_bytes);

    // Repay the re-baseline's padding debt as soon as the padding it was seeded with has drained.
    // Checked only while a debt is outstanding, which is a handful of chunks after a starvation, so
    // the listener walk costs nothing in steady state.
    if (st.padding_debt_frames > 0) {
      audio::AudioDepth lat_now, own_now;
      if (this->audio_listener_ != nullptr && this->audio_listener_->on_query_latency(lat_now) &&
          this->audio_listener_->on_query_audio(own_now)) {
        const int64_t pad_now_us =
            lat_now.microseconds > own_now.microseconds ? lat_now.microseconds - own_now.microseconds : 0;
        if (pad_now_us <= PADDING_DRAINED_US) {
          const int64_t pad_now_frames = pad_now_us * rec.params.sample_rate / 1000000;
          this->playout_mutex_.lock();
          // NEVER below `played`. The debt is a frame count recorded at seed time, but by the time
          // it is repaid the DAC may already have consumed everything the seed covered -- and then
          // subtracting it whole drives pushed under played, which is not merely untidy: it makes
          // available_frames NEGATIVE, so the clamp in notify_audio_played() discards EVERY
          // subsequent credit, `played` stops advancing, and the starvation latch re-fires forever.
          // Observed on hardware as acct_after=-10000 us, then "available -441" on the very next
          // credit, then a reconnect and seven minutes of silence with the sink idle
          // (written == completed, nothing in flight) until the device was power-cycled.
          const int64_t repay =
              std::min(st.padding_debt_frames - pad_now_frames, this->pushed_frames_total_ - this->played_frames_total_);
          if (repay > 0) {
            this->pushed_frames_total_ -= repay;
            // The counter stepped; levels recorded against the old value would read as a split.
            this->clear_playout_history_();
            this->mark_playout_(this->pushed_history_, this->pushed_history_next_, now_us(),
                                this->pushed_frames_total_);
            this->mark_playout_(this->played_history_, this->played_history_next_, now_us(),
                                this->played_frames_total_);
          }
          const int64_t dbg_pushed_after = this->pushed_frames_total_;
          const int64_t dbg_played_after = this->played_frames_total_;
          this->playout_mutex_.unlock();
          if (repay > 0) {
            // Everything needed to see whether the residual exists BEFORE this repayment or is
            // created BY it: the accounting either side, and the chain reading it is compared to.
            const int64_t acct_after =
                (dbg_pushed_after - dbg_played_after) * 1000000 / static_cast<int64_t>(rec.params.sample_rate);
            ESP_LOGD(TAG,
                     "PAYDBG debt=%" PRId64 " pad_now=%" PRId64 " repay=%" PRId64 " pushed=%" PRId64 " played=%" PRId64
                     " acct_after=%" PRId64 " lat=%" PRIu32 " own=%" PRIu32 " resid=%" PRId64,
                     st.padding_debt_frames, pad_now_us, repay, dbg_pushed_after, dbg_played_after, acct_after,
                     lat_now.microseconds, own_now.microseconds,
                     acct_after - static_cast<int64_t>(own_now.microseconds));
            ESP_LOGD(TAG, "Repaid re-baseline padding debt: %" PRId64 " frames (%" PRId64 " us), %s",
                     repay, repay * 1000000 / static_cast<int64_t>(rec.params.sample_rate),
                     st.converged ? "AUDIBLE (already unmuted)" : "silent (still muted)");
          }
          st.padding_debt_frames = 0;
        }
      }
    }

    // TEST HOOK: see inject_starvation(). Discarding here is exactly what a stale chunk gets, so the
    // pipeline drains through its real path rather than through a shortcut.
    const int64_t starve_until = this->starve_until_us_.load(std::memory_order_relaxed);
    if (starve_until != 0) {
      if (now_us() < starve_until) {
        this->discard_ring_bytes_(rec.bytes);
        continue;
      }
      this->starve_until_us_.store(0, std::memory_order_relaxed);
      ESP_LOGW(TAG, "Injected starvation window ended");
    }

    const int64_t deadline = this->chunk_deadline_us_(rec);
    const int64_t hard_us = static_cast<int64_t>(this->config_.hard_resync_threshold_ms) * 1000;
    const uint32_t frames = rec.bytes / frame_bytes;
    const int64_t predicted = this->predict_next_play_us_(rec.params.sample_rate);

    if (predicted < 0) {
      // No playback feedback yet: gate the first push by wall clock, leaving the
      // pipeline STARTUP_LEAD_US to spin up; feedback-based correction takes over
      // after the speaker's first output callback.
      if (now_us() > deadline) {
        this->discard_ring_bytes_(rec.bytes);
        continue;
      }
      while (now_us() < deadline - STARTUP_LEAD_US && this->output_active_.load(std::memory_order_relaxed) &&
             !this->shutdown_.load(std::memory_order_relaxed)) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if (!st.warned_no_sync) {
        this->filter_mutex_.lock();
        st.warned_no_sync = !this->time_filter_.has_estimate();
        this->filter_mutex_.unlock();
        if (st.warned_no_sync) {
          ESP_LOGW(TAG, "Starting playback before first time sync; expect a hard resync");
        }
      }
      this->push_chunk_(rec, 0, true);
      // TEMPORARY DIAGNOSTIC: the startup pushes go through HERE, not the main path below, so
      // without this the instrument only starts watching after ~12 chunks are already pushed --
      // which is exactly the window the offset forms in.
      this->dbg_early_recon_(rec, "boot");
      continue;
    }

    // Refresh the padding estimate: measured fill (rings + FULL DMA span, padding
    // included) minus what the accounting believes is outstanding (real frames only).
    // Diagnostics-only since the correction is no longer applied, so the listener-chain
    // walk and mutex are not worth paying for unless timing diagnostics are wanted.
#ifdef USE_SNAPCLIENT_TIMING_DIAG
    if (st.fill_sample_countdown == 0) {
      st.fill_sample_countdown = FILL_SAMPLE_EVERY_CHUNKS;
      // Own-audio, for the same reason as the drift column: this compares against the accounted
      // queue, which counts only frames we wrote. And against the accounted queue AS IT STOOD at the
      // instant the reading describes -- see accounted_at_().
      audio::AudioDepth measured;
      int64_t accounted_frames = 0;
      bool comparable = false;
      if (this->audio_listener_ != nullptr && frame_bytes > 0 &&
          this->audio_listener_->on_query_audio(measured)) {
        this->playout_mutex_.lock();
        comparable = this->accounted_at_(measured.as_of_us, accounted_frames);
        this->playout_mutex_.unlock();
      }
      if (comparable) {
        const int64_t accounted_us =
            accounted_frames * 1000000 / static_cast<int64_t>(rec.params.sample_rate);
        const int64_t sample_us = static_cast<int64_t>(measured.microseconds) - accounted_us;
        if (std::abs(sample_us) <= FILL_CORR_MAX_US) {
          st.fill_corr_us = st.fill_corr_valid
                             ? st.fill_corr_us + FILL_EWMA_ALPHA * (static_cast<float>(sample_us) - st.fill_corr_us)
                             : static_cast<float>(sample_us);
          st.fill_corr_valid = true;
        }
      }
    } else {
      st.fill_sample_countdown--;
    }
#endif  // USE_SNAPCLIENT_TIMING_DIAG

    // st.fill_corr_us is MEASURED AND REPORTED BUT NOT APPLIED. Applying it was wrong and
    // measurably harmful: it manufactured the very offset it was meant to remove.
    //
    // The premise was that a disagreement between the accumulator (pushed - played) and
    // the observed fill meant the accumulator had latched an error. Once every stage of
    // the chain was reported, though, the residual stopped being unmeasured audio and
    // became measurement offset -- non-atomic sampling of two quantities while audio
    // flows, DMA quantisation, publish staleness. Correcting for that displaces real
    // audio by the size of a measurement artefact, and because the artefact differs per
    // device it produces a RELATIVE offset, which is the only kind that is audible.
    //
    // Caught by raw-sync.py, which measures inter-device rendering from direct
    // observations and so is immune to this: devices carrying corr -10 ms rendered
    // 9.3-10.0 ms later than one carrying corr 0, matching across all three pairings,
    // and a device at corr -52 ms was ~52 ms late -- which is exactly the "distinctly
    // 50 ms+" that was audible. Every on-device metric read clean throughout, medians
    // inside 90 us, because the servo was faithfully hitting a target that had been
    // moved.
    const int64_t error_us = predicted - deadline;  // >0: this chunk would play late

#ifdef CLOCK_SYNC_TSF_ACTIVE
    // RAW timing sample, for offline cross-device analysis. Deliberately built from
    // DIRECT OBSERVATIONS only -- no servo state, no predicted playout -- because every
    // wrong diagnosis in this area came from trusting a model of when audio renders.
    //
    // The feedback pair (played, played_ts) is ground truth: that many frames HAD rendered
    // at that local time. (s_ts, pushed) anchors the frame count to server audio time,
    // which is the same number on every device for the same audio. (tsf, tsf_local)
    // converts local time to the one clock the devices provably share. So offline:
    //
    //   server_time_of_last_rendered_frame = s_ts - (pushed - played) * 1e6 / rate
    //   tsf_time_of_that_frame             = played_ts + (tsf - tsf_local)
    //
    // Differencing tsf_time between devices at equal server_time is the true relative
    // offset, with the servo and the prediction model entirely out of the measurement.
    //
    // Rate: confidence on the fitted offset improves as sqrt(n), so a per-chunk sample
    // (~38/s at 44.1 kHz FLAC) resolves in seconds what a per-report sample needed
    // minutes for. The cost is log traffic on a link that is often the bottleneck --
    // streamed logs compete with audio for the radio, which is why hard-resync logging is
    // throttled. Raise the divisor if the fleet is on a congested channel.
    // Behind timing_diagnostics, not the log level. ESP_LOGD compiles away below DEBUG but
    // the work would not -- raw_tsf_sample() costs up to 5 TSF reads of 45-81 us each plus
    // a mutex, per chunk. More importantly the two concerns differ: this emits ~38 lines/s,
    // and chasing dropouts with DEBUG logs is exactly when that traffic hurts most.
#ifdef USE_SNAPCLIENT_TIMING_DIAG
    if (this->tsf_sync_ != nullptr && --st.raw_sample_countdown == 0) {
      st.raw_sample_countdown = RAW_SAMPLE_EVERY_CHUNKS;
      int64_t raw_tsf = 0, raw_local = 0, raw_width = 0;
      if (TsfSync::raw_tsf_sample(raw_tsf, raw_local, raw_width)) {
        this->playout_mutex_.lock();
        const int64_t r_played = this->played_frames_total_;
        const int64_t r_pushed = this->pushed_frames_total_;
        const int64_t r_played_ts = this->played_last_ts_us_;
        this->playout_mutex_.unlock();
        ESP_LOGD(TAG,
                 "RAW s_ts=%" PRId64 " pushed=%" PRId64 " played=%" PRId64 " played_ts=%" PRId64 " tsf=%" PRId64
                 " tsf_local=%" PRId64 " sw=%" PRId64 " rate=%" PRIu32,
                 rec.server_ts_us, r_pushed, r_played, r_played_ts, raw_tsf, raw_local, raw_width,
                 rec.params.sample_rate);
      }
    }
#endif  // USE_SNAPCLIENT_TIMING_DIAG
#endif  // CLOCK_SYNC_TSF_ACTIVE


    st.err_accum_us += error_us;
    st.err_peak_us = std::max(st.err_peak_us, std::abs(error_us));

    st.err_window[st.err_window_idx] = error_us;
    st.err_window_idx = (st.err_window_idx + 1) % MEDIAN_WINDOW;
    if (st.err_window_filled < MEDIAN_WINDOW) {
      st.err_window_filled++;
    }
    int64_t median_err_us = error_us;
    if (st.err_window_filled == MEDIAN_WINDOW) {
      int64_t sorted[MEDIAN_WINDOW];
      memcpy(sorted, st.err_window, sizeof(sorted));
      std::nth_element(sorted, sorted + MEDIAN_WINDOW / 2, sorted + MEDIAN_WINDOW);
      median_err_us = sorted[MEDIAN_WINDOW / 2];
    }

    this->log_sync_report_(st, rec, frame_bytes, median_err_us);

    // Past the server's own bufferMs every chunk in flight is stale by definition.
    const int64_t stale_us =
        std::max<int64_t>(static_cast<int64_t>(this->buffer_ms_.load(std::memory_order_relaxed)),
                          static_cast<int64_t>(this->config_.hard_resync_threshold_ms)) *
        1000;
    this->check_stale_bailout_(st, error_us, stale_us);

    // Decide ONCE, for both directions, whether this excursion should mute. Muting is
    // for storms; a lone splice is cheaper than a re-lock. Magnitude still overrides
    // the count: both devices once logged a simultaneous 24888016 ms error (a ~6.9 h
    // timebase step) with only 5 resyncs, and playing audibly toward something that
    // far outside the server's buffer is meaningless -- so anything past bufferMs
    // mutes on the spot, and the bailout above reconnects if it persists.
    bool mute_now = false;
    if (std::abs(error_us) > hard_us) {
      if (now_us() - st.storm_window_us > RESYNC_STORM_WINDOW_US) {
        st.storm_window_us = now_us();
        st.storm_resyncs = 0;
      }
      st.storm_resyncs++;
      mute_now = st.storm_resyncs >= RESYNC_STORM_COUNT || std::abs(error_us) > stale_us;
      if (std::abs(error_us) > stale_us) {
        // Past the server's own bufferMs the DEADLINE is wrong, not our clock, and it is wrong
        // for the whole group at once -- measured on a pause/resume as a 2111 ms and a 2091 ms
        // excursion on two devices within 3 ms of each other. Latch it: the splice absorbs the
        // error within a window or two, so an instantaneous test stops being true long before
        // the re-lock finishes, and a leader in that gap demotes for something that was never
        // its fault. Cleared on convergence, below.
        st.deadline_implausible = true;
      }
      if (st.converged && !mute_now && st.storm_resyncs == 1) {
        // INFO because it IS audible -- a skip of roughly this length. Logged only for
        // the first of a window so a storm cannot flood the link on its way to muting.
        ESP_LOGI(TAG, "Hard resync %" PRId64 " ms: correcting audibly, staying unmuted", error_us / 1000);
      }
    }

    // Hard-resync logging is throttled to one line per RESYNC_LOG_INTERVAL_US: during
    // a recovery storm this fires per chunk, and when logs stream over the api the
    // log traffic competes with the audio stream on the already-congested link — a
    // feedback loop that prolongs the outage. The periodic sync report carries the
    // full per-window count either way.
    if (error_us > hard_us) {
      // Hard resync, late: drop whole chunks until we catch back up
      if (now_us() - st.last_resync_log_us >= RESYNC_LOG_INTERVAL_US) {
        st.last_resync_log_us = now_us();
        ESP_LOGD(TAG, "Hard resync: %" PRId64 " ms late, dropping chunks (throttled log)", error_us / 1000);
      }
      st.hard_resyncs++;
      st.err_window_filled = 0;
      st.steer_dir = 0;
      // INFO on the true->false edge: this is the moment audio goes silent, and it
      // is the only user-audible event in the loop. Logging only the re-lock (which
      // is INFO) made a dropout look like a spontaneous "Sync locked" with no cause,
      // since the resync line above is DEBUG and throttled. One line per gap.
      if (st.converged && mute_now) {
        ESP_LOGI(TAG, "Muting: hard resync, %" PRId64 " ms late (%" PRIu32
                      " in %" PRId64 " s) -- audible gap until re-lock",
                 error_us / 1000, st.storm_resyncs, RESYNC_STORM_WINDOW_US / 1000000);
      }
      st.converged = st.converged && !mute_now;
      this->discard_ring_bytes_(rec.bytes);
      continue;
    }

    uint32_t drop_frames = 0;
    if (error_us < -hard_us) {
      // Hard resync, early: fill the gap with silence (bounded per chunk so the
      // loop stays responsive), keeping the DAC fed and continuous
      const int64_t gap_frames = (-error_us) * rec.params.sample_rate / 1000000;
      const uint32_t fill = std::min<int64_t>(gap_frames, rec.params.sample_rate / 2);
      if (now_us() - st.last_resync_log_us >= RESYNC_LOG_INTERVAL_US) {
        st.last_resync_log_us = now_us();
        ESP_LOGD(TAG, "Hard resync: %" PRId64 " ms early, inserting silence (throttled log)", -error_us / 1000);
      }
      st.hard_resyncs++;
      st.err_window_filled = 0;
      st.steer_dir = 0;
      if (st.converged && mute_now) {
        ESP_LOGI(TAG, "Muting: hard resync, %" PRId64 " ms early (%" PRIu32
                      " in %" PRId64 " s) -- audible gap until re-lock",
                 -error_us / 1000, st.storm_resyncs, RESYNC_STORM_WINDOW_US / 1000000);
      }
      st.converged = st.converged && !mute_now;
      this->push_silence_(fill, rec.params);
    } else if (std::abs(median_err_us) > SOFT_CORRECTION_AGGRESSIVE_US) {
      // Post-stall catch-up: frames/32 bursts (~33 ms/s convergence) so a backlog
      // doesn't leave playback audibly behind for long
      const int32_t adjust_frames =
          static_cast<int32_t>(median_err_us * static_cast<int64_t>(rec.params.sample_rate) / 1000000);
      const int32_t max_adjust = std::max<int32_t>(1, frames / (SOFT_CORRECTION_DIVISOR / 4));
      const int32_t adjust = std::clamp(adjust_frames, -max_adjust, max_adjust);
      if (adjust > 0) {
        drop_frames = adjust;
        st.soft_dropped_frames += adjust;
      } else if (adjust < 0) {
        st.soft_inserted_frames += -adjust;
        this->push_silence_(-adjust, rec.params);
      }
      st.steer_dir = 0;
    } else if (st.err_window_filled == MEDIAN_WINDOW) {
      // Steering servo (reference design): engage when the median error exceeds
      // sync_deadband, then trim exactly one frame (~23 us splice, inaudible) per
      // chunk until it crosses back inside half the threshold. Continuous hold near
      // zero is what keeps a stereo pair's image pinned.
      const int64_t engage_us = this->config_.sync_deadband_us;
      if (st.steer_dir == 0) {
        if (median_err_us > engage_us) {
          st.steer_dir = 1;
        } else if (median_err_us < -engage_us) {
          st.steer_dir = -1;
        }
      } else if ((st.steer_dir > 0 && median_err_us < engage_us / 2) ||
                 (st.steer_dir < 0 && median_err_us > -engage_us / 2)) {
        st.steer_dir = 0;
      }
      bool trim_holds = false;
#ifdef USE_I2S_RATE_LOCK
      // Steady-state rate lock: steer the I2S clock instead of splicing frames.
      // Continuous PI on the median error, no deadband -- trims are inaudible, and
      // gating them through the hysteresis band re-creates the limit cycle. The
      // hysteresis/st.steer_dir path above still drives the splice fallback. Muted
      // convergence uses hard splices while far out (much faster than the trim
      // slew), handing off to the PI for the end-game so the error actually
      // settles inside the band instead of splice-limit-cycling around it.
      if (st.rate_lock_ok && (st.converged || std::abs(median_err_us) <= this->config_.converge_fine_us)) {
        const float dt_s = static_cast<float>(frames) / rec.params.sample_rate;
        const float clamp_ppm = trim_clamp_ppm(this->config_.converge_fine_us);
        // Acquisition and steady state want OPPOSITE gains, so the loop switches.
        //
        // Running on the low gain everywhere delayed audio start badly: the unmute gate
        // needs MEDIAN_WINDOW *consecutive* chunks inside 2x sync_deadband, and the low
        // gain no longer holds the common-mode error in that band -- measured 54%/57% of
        // medians in band, so the counter kept resetting and lock took 31-33 s from boot.
        // The low gain is chosen precisely BECAUSE it stops chasing the common-mode error
        // (see TRIM_KP_RUN_PPM_PER_US), so it is structurally in tension with a gate that
        // is defined on that error. Acquisition needs the error nulled fast; steady state
        // needs the differential noise not amplified. One gain cannot do both.
        const float kp = st.converged ? TRIM_KP_RUN_PPM_PER_US : TRIM_KP_ACQUIRE_PPM_PER_US;
        // Bumpless transfer. The gains differ 5x, so switching would step the output by
        // (kp_hi - kp_lo) * error -- up to ~100 ppm at the unmute threshold, i.e. a real
        // rate step on one board at the exact moment it becomes audible, which is the
        // defect this whole change exists to remove. Move the difference into the
        // integrator instead so the commanded trim is continuous across the switch.
        if (st.converged != st.trim_run_gain) {
          const float from = st.trim_run_gain ? TRIM_KP_RUN_PPM_PER_US : TRIM_KP_ACQUIRE_PPM_PER_US;
          st.trim_integral_ppm =
              std::clamp(st.trim_integral_ppm + (from - kp) * static_cast<float>(median_err_us), -clamp_ppm, clamp_ppm);
          st.trim_run_gain = st.converged;
        }
        const float p_term = kp * static_cast<float>(median_err_us);
        // Conditional integration (anti-windup): winding the integral while the
        // output rails just schedules a rail-to-rail relaxation oscillation
        // (observed post-boot: trim flipping +-500 ppm with +-5 ms medians for
        // ~90 s, with audible correction bursts). Freeze the integral whenever the
        // output is saturated in the error's own direction.
        const float unclamped = p_term + st.trim_integral_ppm;
        if (std::abs(unclamped) < clamp_ppm || (unclamped > 0.0f) != (median_err_us > 0)) {
          // KI tracks the ACTIVE gain: the critical-damping relationship is KI = KP^2/4,
          // so a switched KP with a fixed KI would change the damping at the switch --
          // the exact silent-failure mode the KI note near the gain definitions warns about.
          st.trim_integral_ppm = std::clamp(
              st.trim_integral_ppm + (kp * kp / 4.0f) * static_cast<float>(median_err_us) * dt_s, -clamp_ppm,
              clamp_ppm);
        }
        const float trim_ppm = std::clamp(p_term + st.trim_integral_ppm, -clamp_ppm, clamp_ppm);
        // What the PI asked for, kept so the report can show it next to what the divider
        // actually realised. Measured on hardware they agree to ~0.2 ppm, which is how
        // divider quantisation was ruled out as a source of the inter-device excursions.
        st.trim_applied_ppm = trim_ppm;
#ifdef USE_SNAPCLIENT_TIMING_DIAG
        // Report-only: span shows whether the loop tracks a slow offset or chases
        // something it cannot, and railed counts saturation.
        if (st.trim_samples == 0) {
          st.trim_min_ppm = st.trim_max_ppm = trim_ppm;
        } else {
          st.trim_min_ppm = std::min(st.trim_min_ppm, trim_ppm);
          st.trim_max_ppm = std::max(st.trim_max_ppm, trim_ppm);
        }
        st.trim_samples++;
        if (std::abs(trim_ppm) >= clamp_ppm - 0.5f) {
          st.trim_railed++;
        }
#endif
        trim_holds = this->rate_lock_->set_trim_ppm(trim_ppm);
        if (!trim_holds) {
          st.rate_lock_ok = false;
          ESP_LOGW(TAG, "Rate lock unavailable, falling back to frame-splice corrections");
        }
      } else if (st.rate_lock_ok) {
        // Outside the fine band the PI does not run, which previously left the LAST
        // trim applied to the hardware for the whole excursion. That is an
        // uncontrolled rate offset in an arbitrary direction: measured during a
        // delivery stall, the clock sat frozen at -258.56 ppm across four
        // consecutive reports (~10 s) -- playing SLOW while the device was already
        // seconds late, i.e. actively widening the error the coarse mechanism was
        // fighting. Hold nominal rate instead: we have no valid rate estimate out
        // here, so zero is the only defensible value, and the chunk drops/splices
        // below do the correcting.
        //
        // Only reachable while muted (the condition above is
        // `st.converged || in-band`, so this branch means !st.converged && out-of-band),
        // so the rate change cannot be audible. trim_holds stays false on purpose,
        // to keep the coarse splice path engaged.
        this->rate_lock_->set_trim_ppm(0.0f);
        // The hardware is back at nominal, so the slew limiter must start from nominal
        // too -- otherwise the next re-entry ramps away from a value that is no longer
        // programmed, at 2 ppm/s, and the trim silently lags the demand for minutes.
        st.trim_applied_ppm = 0.0f;
      }
#endif
      // While muted (pre-convergence) audibility doesn't constrain splice size, so
      // steer hard to reach the band quickly, then single frames for the end-game
      if (!trim_holds) {
        const uint32_t steer_frames = (st.converged || std::abs(median_err_us) <= this->config_.converge_fine_us)
                                          ? 1
                                          : startup_steer_frames(frames);
        if (st.steer_dir > 0) {
          drop_frames = steer_frames;
          st.soft_dropped_frames += steer_frames;
        } else if (st.steer_dir < 0) {
          st.soft_inserted_frames += steer_frames;
          if (st.converged) {
            this->push_repeat_frame_(rec.params);
          } else {
            this->push_silence_(steer_frames, rec.params);
          }
        }
      }
    }

#ifdef CLOCK_SYNC_TSF_ACTIVE
    // Report our own tracking quality to the TSF layer: a leader publishes the
    // timebase the whole group follows, so it must hand off while its own playout
    // is diverged (observed: a device stuck in a degraded buffer state kept
    // leading, with every peer following its mapping)
    if (this->tsf_sync_ != nullptr) {
      // The second argument is the LATCH, not the live median. A leader must hold its timer for
      // as long as it is recovering from a group-wide bad deadline, which is the whole re-lock --
      // not merely while the median is still enormous. Observed before this: both clients logged
      // "Stepping down (own playout unsynced)" after a pause, the group lost its only publisher,
      // and every follower sat muted for ~47 s with its own servo already in band, because the
      // unmute gate rightly refuses to unmute a follower onto a dead timebase.
      this->tsf_sync_->set_playout_healthy(
          st.converged && st.err_window_filled == MEDIAN_WINDOW && std::abs(median_err_us) < PLAYOUT_HEALTHY_US,
          st.deadline_implausible || std::abs(median_err_us) > stale_us);
    }
#endif

    // Mute-until-synced (reference behavior): convergence corrections are chunky and
    // audible (drops of 14 frames/chunk in the proportional band), so the audio is
    // replaced with silence until the median error holds inside the servo band for a
    // full median window -- a single in-band median mid-convergence is a transient
    // (observed: unmuting on one produced ~90 s of audible post-unmute corrections).
    // Hard resyncs re-mute, turning recovery storms into silent gaps.
    // Unmute needs "no audible corrections pending", not servo-engagement
    // precision: fine-stage medians routinely wobble past the deadband while the
    // PI settles, and requiring consecutive sub-deadband medians stretched
    // post-boot mutes to ~20 s of counter resets. Corrections inside the gate (UNMUTE_BAND_DEADBANDS)
    // are trim-only and inaudible.
    if (std::abs(median_err_us) <= UNMUTE_BAND_DEADBANDS * this->config_.sync_deadband_us) {
#ifdef CLOCK_SYNC_TSF_ACTIVE
      // Don't unmute onto a provisional timebase: a follower still on its Kalman
      // fallback (leader's mapping rejected while our own estimate is raw) will
      // step by up to the plausibility bound when it finally adopts the shared
      // mapping -- audible corrections right after unmute on every speaker join.
      const bool timebase_settled = this->tsf_sync_ == nullptr ||
                                    this->tsf_sync_->role() != TsfSync::Role::FOLLOWER ||
                                    this->deadline_on_shared_tsf_;
#else
      const bool timebase_settled = true;
#endif
      if (!st.converged && st.err_window_filled == MEDIAN_WINDOW && timebase_settled && ++st.in_band_chunks >= MEDIAN_WINDOW) {
        st.converged = true;
        // Recovery is over, so the leader-side hold is released with it.
        st.deadline_implausible = false;
        ESP_LOGI(TAG, "Sync locked (median %" PRId64 " us), unmuting", median_err_us);
      }
    } else {
      st.in_band_chunks = 0;
    }

    this->push_chunk_(rec, drop_frames, !st.converged);

    // TEMPORARY DIAGNOSTIC: see dbg_early_recon_ -- this is the post-startup half.
    // Every term comes from ONE snapshot and the accounting is evaluated at that same instant, so
    // this is the RECON line at chunk resolution rather than once per 128 chunks. Bounded to the
    // first few seconds and sampled every 4th chunk, which is ~10 lines/s for ~4 s.
    this->dbg_early_recon_(rec, "run");

    // Accumulate the accounted queue per chunk so the group cross-check can be handed a MEAN rather
    // than a single sample of a sawtooth. One extra lock per chunk (~38/s) buys an order of
    // magnitude on that comparison's noise floor.
    this->playout_mutex_.lock();
    st.depth_accum_frames += this->pushed_frames_total_ - this->played_frames_total_;
    this->playout_mutex_.unlock();
    st.depth_samples++;
  }
  vTaskDelete(nullptr);
}

// TEMPORARY DIAGNOSTIC: one-instant reconciliation of the accounting against the chain, from the
// very first chunk. Every term is from ONE snapshot and the accounting is evaluated at that
// snapshot's instant, so nothing here is aligned by guesswork. Bounded and sampled so it cannot
// flood. Remove once the startup offset is explained.
void SnapcastClient::dbg_early_recon_(const ChunkRecord &rec, const char *phase) {
  if (this->dbg_early_chunks_ >= 240) {
    return;
  }
  const uint32_t n = this->dbg_early_chunks_++;
  if ((n % 2) != 0) {
    return;
  }
  audio::AudioDepth m;
  if (this->audio_listener_ == nullptr || !this->audio_listener_->on_query_audio(m)) {
    ESP_LOGD(TAG, "EARLY[%" PRIu32 "] %s: sink cannot report yet", n, phase);
    return;
  }
  int64_t acct_frames = 0;
  this->playout_mutex_.lock();
  const bool ok = this->accounted_at_(m.as_of_us, acct_frames);
  const int64_t p_now = this->pushed_frames_total_;
  const int64_t pl_now = this->played_frames_total_;
  this->playout_mutex_.unlock();
  const int64_t rate = static_cast<int64_t>(rec.params.sample_rate);
  // CONSERVATION RESIDUALS, in frames. Every boundary must satisfy received == passed-on + held, so a
  // non-zero residual names the stage that is losing audio. All terms come from one snapshot, so
  // these are exact rather than differences of separately-sampled counters.
  //   r_push : what we think we handed over, against what the source ring says it took
  //   r_src  : the source ring -- taken in, minus given to the mixer, minus what it still holds
  //   r_sink : the sink ring -- taken in, minus written to DMA, minus what it still holds
  const int64_t own_frames = static_cast<int64_t>(m.dbg_own_us) * rate / 1000000;
  const int64_t queued_frames = static_cast<int64_t>(m.dbg_queued_us) * rate / 1000000;
  const int64_t xfer_frames = static_cast<int64_t>(m.dbg_xfer_us) * rate / 1000000;
  const int64_t r_push = p_now - static_cast<int64_t>(m.dbg_src_received);
  const int64_t r_src = static_cast<int64_t>(m.dbg_src_received) - static_cast<int64_t>(m.dbg_src_consumed) -
                        own_frames;
  const int64_t r_mix = static_cast<int64_t>(m.dbg_src_consumed) - static_cast<int64_t>(m.dbg_sink_received) -
                        xfer_frames;
  const int64_t r_sink = static_cast<int64_t>(m.dbg_sink_received) - queued_frames;
  ESP_LOGD(TAG,
           "EARLY[%" PRIu32 "] %s ok=%d acct=%" PRId64 " live=%" PRId64 " meas=%" PRIu32 " own=%" PRIu32
           " xfer=%" PRIu32 " queued=%" PRIu32 " dma=%" PRIu32 " pushed=%" PRId64 " played=%" PRId64
           " | srcrx=%" PRIu32 " srctx=%" PRIu32 " sinkrx=%" PRIu32 " r_push=%" PRId64 " r_src=%" PRId64
           " r_mix=%" PRId64 " r_sink=%" PRId64 " age=%" PRId64,
           n, phase, ok ? 1 : 0, ok ? acct_frames * 1000000 / rate : -1, (p_now - pl_now) * 1000000 / rate,
           m.microseconds, m.dbg_own_us, m.dbg_xfer_us, m.dbg_queued_us, m.dbg_dma_us, p_now, pl_now,
           m.dbg_src_received, m.dbg_src_consumed, m.dbg_sink_received, r_push, r_src, r_mix, r_sink,
           now_us() - m.as_of_us);
}

// THREAD CONTEXT: player task. Consumes the pipeline_starved_ latch.
void SnapcastClient::rebaseline_after_starvation_(ServoState &st, const ChunkRecord &rec, uint32_t frame_bytes) {
    if (this->pipeline_starved_.exchange(false, std::memory_order_relaxed)) {
      // The pipeline fully drained (source starvation). Re-baseline the playout
      // accounting -- but anchor it to the fill the pipeline REPORTS, not to an
      // assumption that it is empty.
      //
      // Assuming empty was the long-standing behaviour and the cause of the
      // silent-offset bug: whatever audio was still in flight went uncounted, so the
      // prediction was wrong by that much and the servo dutifully steered the real
      // audio to the wrong time while its own error read ~0 (it is measured against
      // that same prediction). Measured offsets of 100-250 ms, audible against the
      // other clients, invisible to every metric on the device.
      //
      // on_query_latency() reports how long until audio handed over now would render, so seed
      // pushed as played + that, making the accounted queue equal the measured one. Falls back
      // to the old assume-empty behaviour when the sink cannot report, which is why the query
      // distinguishes "unknown" from "zero".
      //
      // The reported latency includes buffering that holds none of OUR audio -- notably the I2S
      // DMA's silence padding -- and that is deliberate. The prediction this anchors asks when the
      // NEXT pushed frame will render, and padding sits ahead of it in the queue, so it delays our
      // audio just as real frames would. Anchoring to our own audio alone was the earlier behaviour
      // and it under-predicted by exactly the padding.
      //
      // Now whole-chain: source ring, mixer transfer buffer, output ring and DMA span are all
      // included, so the previous caveat about unreported downstream stages no longer applies.
      //
      // Used AS PUBLISHED, deliberately NOT aged forward for the snapshot's staleness. Aging it was
      // tried and it caused a dropout: this is a LATENCY, and its dominant term is the i2s DMA span,
      // which the always-fill model holds permanently full -- every task iteration writes a whole
      // buffer, padding with silence as needed, so the span does not decay with the snapshot's age
      // the way a draining queue would. Subtracting elapsed time from it under-anchors the
      // accounting, the prediction then runs early, the device renders late, and it does not
      // recover: measured on hardware as an anchor of 43 ms where the honest reading was 60, then
      // hard resyncs at 350 ms, 2297 ms and 3581 ms late in four seconds, ending in a reconnect.
      //
      // The other term, the queue ahead of the DMA, would be fair to age -- but this path runs
      // BECAUSE the pipeline drained, so that term is empty by definition and there is nothing there
      // to correct. The staleness that matters for the accounting cross-check is handled where it
      // belongs, in accounted_at_(), which needs no assumption about what drains.
      //
      // The reseed itself was tested by REMOVING it, using inject_starvation() to fire the event on
      // one device on demand rather than waiting for a group-wide one. It cannot go. Without it the
      // same injected starvation left the accounting at -204 ms with nothing able to fix it -- the
      // self-repair only acts on POSITIVE drift -- and the pair swung +-20 ms for about twenty
      // seconds before the servo walked it out. With it: re-locked in 9 s, back to within a frame on
      // a logic analyser. A starvation really does discard, and this seed is what recovers from it.
      audio::AudioDepth latency, own_audio;
      const bool have_fill = this->audio_listener_ != nullptr && frame_bytes > 0 &&
                             this->audio_listener_->on_query_latency(latency);
      const bool have_own = this->audio_listener_ != nullptr && frame_bytes > 0 &&
                            this->audio_listener_->on_query_audio(own_audio);
      const int64_t in_flight_frames =
          have_fill ? static_cast<int64_t>(latency.microseconds) * rec.params.sample_rate / 1000000 : 0;
      // The padding is the difference between the two queries, by definition: latency counts the
      // DMA's silence, own-audio does not. Record it so it can be repaid, not left for the repair.
      st.padding_debt_frames =
          (have_fill && have_own && latency.microseconds > own_audio.microseconds)
              ? static_cast<int64_t>(latency.microseconds - own_audio.microseconds) * rec.params.sample_rate / 1000000
              : 0;
      this->playout_mutex_.lock();
      this->playout_valid_ = false;
      this->played_frames_total_ = 0;
      this->pushed_frames_total_ = in_flight_frames;
      this->fb_samples_ = 0;
      // The counters just jumped; anything remembered against them is now meaningless. Seed the
      // histories at this instant so the next reading has something honest to compare against.
      this->clear_playout_history_();
      this->mark_playout_(this->pushed_history_, this->pushed_history_next_, now_us(), this->pushed_frames_total_);
      this->mark_playout_(this->played_history_, this->played_history_next_, now_us(), this->played_frames_total_);
      this->playout_mutex_.unlock();
      if (have_fill) {
        // The snapshot's age is logged but NOT applied -- see above. It is here because it is the
        // number that would have to be wrong for this anchor to be wrong.
        ESP_LOGD(TAG, "SEEDDBG latency=%" PRIu32 " own=%" PRIu32 " debt=%" PRId64 " seed=%" PRId64
                      " played_was=%" PRId64,
                 latency.microseconds, have_own ? own_audio.microseconds : 0, st.padding_debt_frames,
                 in_flight_frames, this->played_frames_total_);
        ESP_LOGD(TAG, "Re-baseline anchored to measured latency: %" PRIu32 " ms (%" PRId64
                      " frames), snapshot %" PRId64 " ms old",
                 latency.microseconds / 1000, in_flight_frames, (now_us() - latency.as_of_us) / 1000);
      } else {
        // Log the FALLBACK too. Without this the two cases are indistinguishable in a
        // log -- a silent fallback looks exactly like the feature working, which is
        // how the first flash of this code read as "no starvations to anchor" when in
        // fact every one of ~120 starvations had taken this branch.
        ESP_LOGW(TAG, "Re-baseline could not read the pipeline fill; assuming empty (listener=%d)",
                 this->audio_listener_ != nullptr ? 1 : 0);
      }
      st.err_window_filled = 0;
      st.steer_dir = 0;
      st.converged = false;
  #ifdef USE_I2S_RATE_LOCK
      if (this->rate_lock_ != nullptr) {
        this->rate_lock_->invalidate_baseline();
      }
  #endif
      ESP_LOGI(TAG, "Pipeline drained (source starvation); re-baselining playout");
    }
}

// THREAD CONTEXT: player task. Emits the periodic report and, on the same cadence,
// repairs a sustained accounted-vs-measured split. Resets the window counters.
void SnapcastClient::log_sync_report_(ServoState &st, const ChunkRecord &rec, uint32_t frame_bytes,
                                      int64_t median_err_us) {
    if (++st.err_count >= 128) {
      int64_t max_gap_us;
      int64_t pipeline_frames;
      int64_t fb_mean_gap_us;
      this->playout_mutex_.lock();
      max_gap_us = this->max_feedback_gap_us_;
      this->max_feedback_gap_us_ = 0;
      fb_mean_gap_us = this->fb_gap_count_ > 0 ? this->fb_gap_sum_us_ / this->fb_gap_count_ : 0;
      this->fb_gap_sum_us_ = 0;
      this->fb_gap_count_ = 0;
      pipeline_frames = this->pushed_frames_total_ - this->played_frames_total_;
      this->playout_mutex_.unlock();
      // Accounted pipeline depth (pushed-but-unplayed). Sane: a stable few hundred
      // ms (mixer + speaker buffers). Divergence from reality is invisible to the
      // servo -- a value outside ~0..500 ms means the accounting has split from the
      // pipeline (e.g. an unnoticed flush) and playback is audibly offset while the
      // report looks clean.
      const int32_t pipeline_ms =
          static_cast<int32_t>(pipeline_frames * 1000 / static_cast<int64_t>(rec.params.sample_rate));
      // MEASURED pipeline fill, against the accounted one above. The two should track:
      // both claim to be the audio we have handed over but which has not yet played.
      // A persistent gap is the phantom-frame bug -- notify_audio_played()'s clamp only
      // fires when reported frames EXCEED the accounted queue, so while the queue looks
      // healthy the DAC can play something we never pushed (auto_clear fill while the
      // mixer task is behind, ducking, mixer output ahead of our source) and those
      // frames are counted as our audio playing. Accounted then falls below true, the
      // prediction lands early by the difference, and the servo pushes real audio LATE
      // by that much -- permanently, since every metric agrees with itself. Reported as
      // `fill <measured> ms (drift <accounted-measured>)`; a drift that holds for
      // minutes IS the audible offset, and its sign says which way.
      // Drift is a difference of two near-equal quantities, so it needs finer resolution than either
      // operand's display. Differencing the rounded millisecond values carried up to ~2 ms of
      // quantisation -- an order of magnitude above the servo's ~200 us residual and the 128 us
      // deadband, i.e. coarse enough to read as "+0" while the accounting was meaningfully split.
      // Both sides are therefore differenced in microseconds, unrounded.
      int32_t fill_ms = -1;
      int32_t fill_drift_us = 0;
      bool fill_comparable = false;
      {
        // OWN-AUDIO, not latency. `pushed - played` counts only frames we wrote, so differencing it
        // against the latency counts the DMA's silence padding as a split that is not one --
        // measured on hardware as a standing -71 ms and -10 ms on two clients, in the direction the
        // repair cannot even act on.
        //
        // And differenced AT THE READING'S OWN INSTANT. The reading is a snapshot published on the
        // sink's task cadence; `pushed - played` moves continuously on ours. Differencing the two as
        // read measured the phase between those cadences and almost nothing else: on the fleet the
        // result was quantised in whole chunks (26.1 ms at 44.1 kHz, and integer multiples of it),
        // barely autocorrelated where the accounted queue itself is smooth, with a mean that wandered
        // tens of milliseconds over hours and in opposite directions on two clients. A real split does
        // the opposite -- it holds. Evaluating the accounting at `as_of_us` removes the artefact
        // rather than filtering it, which matters because the repair below acts on this number.
        audio::AudioDepth measured;
        if (this->audio_listener_ != nullptr && frame_bytes > 0 &&
            this->audio_listener_->on_query_audio(measured)) {
          fill_ms = static_cast<int32_t>(measured.microseconds / 1000);
          int64_t accounted_then_frames = 0;
          this->playout_mutex_.lock();
          fill_comparable = this->accounted_at_(measured.as_of_us, accounted_then_frames);
          this->playout_mutex_.unlock();
          if (fill_comparable) {
            const int64_t accounted_us =
                accounted_then_frames * 1000000 / static_cast<int64_t>(rec.params.sample_rate);
            fill_drift_us = static_cast<int32_t>(accounted_us - static_cast<int64_t>(measured.microseconds));

            // TEMPORARY DIAGNOSTIC: every term of the comparison at ONE instant. The reading carries
            // the chain's split with it under the same seqlock, and the accounting is evaluated at that
            // same `as_of`, so nothing here is time-aligned by guesswork -- which is exactly what
            // defeated the previous attempt, where the terms were logged hundreds of milliseconds
            // apart while the quantity sought was 20 ms. `sum` is the four terms added back up: if it
            // differs from `meas` the chain is not reporting what it says it is, and if it matches
            // while `drift` does not, the missing audio is in none of the four.
            int64_t dbg_pushed, dbg_played;
            this->playout_mutex_.lock();
            dbg_pushed = this->pushed_frames_total_;
            dbg_played = this->played_frames_total_;
            this->playout_mutex_.unlock();
            // Conservation residuals, in frames: every boundary must satisfy
            // received == passed-on + still-held, so a non-zero one names the stage losing audio.
            // These were only on the boot-phase line before, which cannot see a LATER re-baseline --
            // and the residual under investigation appears after one.
            const int64_t rate_i = static_cast<int64_t>(rec.params.sample_rate);
            const int64_t own_f = static_cast<int64_t>(measured.dbg_own_us) * rate_i / 1000000;
            const int64_t xfer_f = static_cast<int64_t>(measured.dbg_xfer_us) * rate_i / 1000000;
            const int64_t queued_f = static_cast<int64_t>(measured.dbg_queued_us) * rate_i / 1000000;
            ESP_LOGD(TAG,
                     "RECON drift=%" PRId32 " acct=%" PRId64 " meas=%" PRIu32 " sum=%" PRIu32 " own=%" PRIu32
                     " xfer=%" PRIu32 " queued=%" PRIu32 " dma=%" PRIu32 " age=%" PRId64 " pushed=%" PRId64
                     " played=%" PRId64 " clamped=%" PRId64 " | srcrx=%" PRIu32 " srctx=%" PRIu32 " sinkrx=%" PRIu32
                     " r_push=%" PRId64 " r_src=%" PRId64 " r_mix=%" PRId64 " r_sink=%" PRId64,
                     fill_drift_us, accounted_us, measured.microseconds,
                     measured.dbg_own_us + measured.dbg_xfer_us + measured.dbg_queued_us + measured.dbg_dma_us,
                     measured.dbg_own_us, measured.dbg_xfer_us, measured.dbg_queued_us, measured.dbg_dma_us,
                     now_us() - measured.as_of_us, dbg_pushed, dbg_played, this->dbg_clamped_frames_,
                     measured.dbg_src_received, measured.dbg_src_consumed, measured.dbg_sink_received,
                     dbg_pushed - static_cast<int64_t>(measured.dbg_src_received),
                     static_cast<int64_t>(measured.dbg_src_received) -
                         static_cast<int64_t>(measured.dbg_src_consumed) - own_f,
                     static_cast<int64_t>(measured.dbg_src_consumed) -
                         static_cast<int64_t>(measured.dbg_sink_received) - xfer_f,
                     static_cast<int64_t>(measured.dbg_sink_received) - queued_f);
          }
        }
      }
      // Repair a sustained split. Acts only on evidence: the measured latency is an independent
      // witness to the accounted queue, so a gap that holds for DRIFT_REPAIR_HOLD_US is the
      // accounting being wrong, not the pipeline moving.
      //
      // Repairs toward ZERO, with no learned baseline. A baseline was right while the chain was
      // only partly reported -- the sink published its own queue and nothing else, so the mixer
      // transfer buffer and the DMA span showed up as a standing positive drift that differed per
      // board (4.7 ms and 8.0 ms on two clients). Those stages are reported now, so there is no
      // legitimate residue left to learn: measured on hardware, a fully-accounted device sits at a
      // drift median of 7 us.
      //
      // Learning it was also actively harmful. The baseline was an EWMA that absorbed anything
      // within a few ms of itself, so a split growing slower than that per report was ratcheted
      // into "normal for this device" and never repaired -- observed on a client sitting at 72 ms
      // of drift having fired zero repairs across its entire log.
      // fill_comparable, not just fill_ms: a reading older than the playout history cannot be
      // differenced honestly, and a repair driven by a guessed difference injects the very offset it
      // is meant to remove.
      //
      // ARMING and HOLDING are separate tests, and they have to be. Requiring every sample to clear
      // DRIFT_REPAIR_US made a drift sitting exactly ON the threshold unrepairable: measured on
      // hardware, a client held a real 882-frame split for its entire uptime while alternating
      // between 19999 and 20000 us, and every 19999 reset the hold window one sample before it could
      // complete. The value most in need of repair was the one value that could never get it, while
      // its peer -- which came up at 25510 us, comfortably clear -- was repaired within seconds.
      //
      // So arm on a sample that warrants repair, then hold while the drift STAYS PUT, which is what
      // the steadiness band already measures. A dip of 1 us is inside the band and keeps the window;
      // a collapse to zero blows the band and restarts it, disarmed unless that sample independently
      // warrants arming.
      if (fill_comparable && st.converged) {
        if (fill_drift_us >= DRIFT_REPAIR_US && st.drift_excess_since_us == 0) {
          st.drift_excess_since_us = now_us();
          st.drift_excess_min_us = fill_drift_us;
          st.drift_excess_max_us = fill_drift_us;
        }
        if (st.drift_excess_since_us != 0) {
          st.drift_excess_min_us = std::min(st.drift_excess_min_us, fill_drift_us);
          st.drift_excess_max_us = std::max(st.drift_excess_max_us, fill_drift_us);
          if (st.drift_excess_max_us - st.drift_excess_min_us > DRIFT_STEADY_BAND_US) {
            // Moving, so it is a measurement artefact rather than a split. Restart the window from
            // here rather than abandoning it: a genuine split that begins during a noisy patch should
            // still be caught once the noise passes. Restarting DISARMED unless this sample would
            // have armed it on its own -- otherwise a drift that collapsed to zero would keep a
            // window open on the strength of a magnitude it no longer has.
            st.drift_excess_since_us = (fill_drift_us >= DRIFT_REPAIR_US) ? now_us() : 0;
            st.drift_excess_min_us = fill_drift_us;
            st.drift_excess_max_us = fill_drift_us;
          } else if (now_us() - st.drift_excess_since_us >= DRIFT_REPAIR_HOLD_US) {
            // Trust the measurement: drop the accounted queue by the whole drift. Playback was
            // running that far early, so the prediction moves later and the servo walks the phase
            // back through the proportional band.
            const int64_t excess_frames =
                static_cast<int64_t>(fill_drift_us) * static_cast<int64_t>(rec.params.sample_rate) / 1000000;
            this->playout_mutex_.lock();
            this->pushed_frames_total_ -= excess_frames;
            // The counter just stepped; levels recorded against its old value would make the next
            // reading look split by the size of the repair.
            this->clear_playout_history_();
            this->mark_playout_(this->pushed_history_, this->pushed_history_next_, now_us(),
                                this->pushed_frames_total_);
            this->mark_playout_(this->played_history_, this->played_history_next_, now_us(),
                                this->played_frames_total_);
            this->playout_mutex_.unlock();
            ESP_LOGW(TAG,
                     "Accounting split repaired: accounted queue ran %" PRId32 " us over measured latency "
                     "for %" PRId64 " s; playback was that far early",
                     fill_drift_us, DRIFT_REPAIR_HOLD_US / 1000000);
            st.drift_excess_since_us = 0;
          }
        }
      } else {
        // Nothing to compare against, or not yet converged: no window may be open.
        st.drift_excess_since_us = 0;
      }

      char fill_str[96] = "";
      if (fill_ms >= 0 && fill_comparable) {
#ifdef USE_SNAPCLIENT_TIMING_DIAG
        snprintf(fill_str, sizeof(fill_str), ", fill %" PRId32 " ms (drift %+" PRId32 " us, corr %+d us)",
                 fill_ms, fill_drift_us, static_cast<int>(st.fill_corr_us));
#else
        // fill_corr_us is only ever SAMPLED under timing diagnostics, so with them off it
        // is a hard zero -- and printing "corr +0 us" then reads as "measured, and it
        // agreed" when nothing was measured at all. Same failure the branch below exists
        // to avoid; omit the field rather than report a number we did not take.
        snprintf(fill_str, sizeof(fill_str), ", fill %" PRId32 " ms (drift %+" PRId32 " us)", fill_ms, fill_drift_us);
#endif
      } else if (fill_ms >= 0) {
        // Distinguish "no honest comparison available" from "compared, and it agreed". Silently
        // printing +0 for the first is how a broken instrument reads as a healthy device.
        snprintf(fill_str, sizeof(fill_str), ", fill %" PRId32 " ms (drift stale)", fill_ms);
      }
      // Ring occupancy shows how much dropout cushion is actually held client-side
      const uint32_t buffered_ms = static_cast<uint32_t>(
          static_cast<uint64_t>(this->pcm_ring_->available()) * 1000 / (frame_bytes * rec.params.sample_rate));
      char trim_str[112] = "";
  #ifdef USE_I2S_RATE_LOCK
      if (st.rate_lock_ok) {
#ifdef USE_SNAPCLIENT_TIMING_DIAG
        if (st.trim_samples > 0) {
          snprintf(trim_str, sizeof(trim_str),
                   ", trim %+.2f ppm (span %+.0f..%+.0f, railed %" PRIu32 "/%" PRIu32 ")",
                   this->rate_lock_->applied_ppm(), st.trim_min_ppm, st.trim_max_ppm, st.trim_railed, st.trim_samples);
        } else {
          snprintf(trim_str, sizeof(trim_str), ", trim %+.2f ppm (idle)", this->rate_lock_->applied_ppm());
        }
#else
        // span/railed are accumulated under timing diagnostics only, so with them off
        // trim_samples is permanently 0 and every report claimed "(idle)" while the loop
        // was in fact steering the clock by tens of ppm -- which is how the trim's role
        // in the inter-device excursions stayed invisible. Print the demand alongside
        // what the divider could actually realise; those two are the whole story here,
        // and neither needs the diag build.
        snprintf(trim_str, sizeof(trim_str), ", trim %+.2f ppm (want %+.2f)", this->rate_lock_->applied_ppm(),
                 st.trim_applied_ppm);
#endif
      }
  #endif
      char tsf_str[64] = "";
  #ifdef CLOCK_SYNC_TSF_ACTIVE
      if (this->tsf_sync_ != nullptr) {
        // Publish our depth so the group can cross-check it (see TsfSync)
        // Microseconds: this delta is the only instrument that can see an absolute playout
        // offset, and the alignment it has to resolve is ~100 us, so a millisecond grid read
        // +0 across the entire range that matters.
        // The WINDOW MEAN, not the instant: see depth_accum_frames. Falls back to the
        // instantaneous value only before the first chunk of a window has been counted.
        const int64_t depth_frames =
            st.depth_samples > 0 ? st.depth_accum_frames / static_cast<int64_t>(st.depth_samples) : pipeline_frames;
        st.depth_accum_frames = 0;
        st.depth_samples = 0;
        this->tsf_sync_->set_pipeline_us(
            static_cast<int32_t>(depth_frames * 1000000 / static_cast<int64_t>(rec.params.sample_rate)));
        const TsfSync::Role role = this->tsf_sync_->role();
        if (role == TsfSync::Role::LEADER) {
          snprintf(tsf_str, sizeof(tsf_str), ", tsf=leader(peers %u)", this->tsf_sync_->peer_count());
        } else if (role == TsfSync::Role::FOLLOWER) {
          // depth delta vs the leader: the only visibility we have into an absolute
          // playout offset, which the median above cannot show by construction
          const int32_t depth_delta = this->tsf_sync_->pipeline_delta_us();
          if (depth_delta == INT32_MIN) {
            snprintf(tsf_str, sizeof(tsf_str), ", tsf=follower(%.1fs)", this->tsf_sync_->mapping_age_s(now_us()));
          } else {
            snprintf(tsf_str, sizeof(tsf_str), ", tsf=follower(%.1fs, depth %+" PRId32 " us)",
                     this->tsf_sync_->mapping_age_s(now_us()), depth_delta);
          }
        } else {
          // Roleless does NOT imply no shared timebase: a leader that handed off keeps
          // its mapping for the election, and deadlines still come from it. Printing
          // nothing here read as "fell back to Kalman" and sent an earlier diagnosis
          // down the wrong path, so say which it is.
          const float age_s = this->tsf_sync_->mapping_age_s(now_us());
          if (age_s >= 0.0f) {
            snprintf(tsf_str, sizeof(tsf_str), ", tsf=roleless(mapping %.1fs)", age_s);
          } else {
            snprintf(tsf_str, sizeof(tsf_str), ", tsf=inactive(kalman)");
          }
        }
      }
  #endif
      ESP_LOGD(TAG,
               "Sync: avg %" PRId64 " us, peak %" PRId64 " us, median %" PRId64
               " us | corrected -%" PRIu32 "/+%" PRIu32 " frames, %" PRIu32 " hard resyncs, feedback %" PRId64
               " us mean / %" PRId64 " ms max, buffered %" PRIu32 " ms, pipeline %" PRId32 " ms%s%s%s over %" PRIu32
               " chunks",
               st.err_accum_us / st.err_count, st.err_peak_us, median_err_us, st.soft_dropped_frames, st.soft_inserted_frames,
               st.hard_resyncs, fb_mean_gap_us, max_gap_us / 1000, buffered_ms, pipeline_ms, fill_str, trim_str,
               tsf_str, st.err_count);
      st.err_accum_us = 0;
      st.err_peak_us = 0;
      st.err_count = 0;
      st.soft_dropped_frames = 0;
      st.soft_inserted_frames = 0;
      st.hard_resyncs = 0;
  #ifdef USE_I2S_RATE_LOCK
      st.trim_samples = 0;
      st.trim_railed = 0;
  #endif
    }
}

// THREAD CONTEXT: player task.
void SnapcastClient::check_stale_bailout_(ServoState &st, int64_t error_us, int64_t stale_us) {
    // Bail out of a backlog that cannot be caught up. Dropping chunks only closes a
    // gap when chunks arrive FASTER than real time; when the radio is the bottleneck
    // the client receives a trickle, discards all of it, and stays exactly as far
    // behind. Lateness then grows linearly for as long as the congestion lasts
    // (observed on hardware: 456 ms -> 18.4 s over 19 s, no recovery, no sync reports
    // because fewer than one report's worth of chunks arrived in that time).
    //
    // Past the server's own bufferMs every chunk still in flight is stale by
    // definition, so there is nothing left worth playing toward. Reconnecting resets
    // the stream to now: ~1-2 s of silence against an unbounded silent spiral. The
    // ring's remaining stale chunks are not purged -- the player discards them on the
    // next passes, which costs no pushes and drains in well under the reconnect.
    if (error_us > stale_us) {
      if (st.stale_since_us == 0) {
        st.stale_since_us = now_us();
      } else if (now_us() - st.stale_since_us >= STALE_BAILOUT_US) {
        ESP_LOGW(TAG, "Stream %" PRId64 " ms late for %" PRId64 " s and not catching up: reconnecting",
                 error_us / 1000, STALE_BAILOUT_US / 1000000);
        st.stale_since_us = 0;
        // Breaks recv_exact_ out of the session; the network task reconnects with no
        // backoff, and connection_session_() clears the flag and resets the time filter
        this->reconnect_requested_.store(true, std::memory_order_relaxed);
      }
    } else {
      st.stale_since_us = 0;
    }
}

int64_t SnapcastClient::predict_next_play_us_(uint32_t sample_rate) {
  this->playout_mutex_.lock();
  int64_t predicted = -1;
  if (this->playout_valid_) {
    const double nominal_slope = 1e6 / static_cast<double>(sample_rate);
    if (this->fb_samples_ >= 8) {
      // Smoothed pivot + exact nominal slope: averages away feedback quantization
      // without a fitted slope's lever-arm instability (see notify_audio_played)
      predicted = static_cast<int64_t>(
          this->fb_mean_ts_ +
          nominal_slope * (static_cast<double>(this->pushed_frames_total_) - this->fb_mean_frames_));
    } else {
      const int64_t queued_frames = this->pushed_frames_total_ - this->played_frames_total_;
      predicted = this->played_last_ts_us_ + queued_frames * 1000000 / static_cast<int64_t>(sample_rate);
    }
  }
  this->playout_mutex_.unlock();
  return predicted;
}

int64_t SnapcastClient::chunk_deadline_us_(const ChunkRecord &rec) {
  // Effective playout buffer, matching the reference client (controller.cpp):
  // max(0, bufferMs - serverLatency - localLatency)
  const int64_t buffer_us =
      std::max<int64_t>(0, static_cast<int64_t>(this->buffer_ms_.load(std::memory_order_relaxed)) -
                               this->server_latency_ms_.load(std::memory_order_relaxed) -
                               this->static_delay_ms_.load(std::memory_order_relaxed)) *
      1000;

#ifdef CLOCK_SYNC_TSF_ACTIVE
  // TSF group sync: prefer the AP-shared server->TSF mapping so every same-AP
  // client derives identical deadlines (estimate wander becomes common-mode)
  if (this->tsf_sync_ != nullptr) {
    int64_t shared_offset_us;
    if (this->tsf_sync_->shared_server_offset_us(now_us(), shared_offset_us)) {
      this->deadline_on_shared_tsf_ = true;
      return rec.server_ts_us + buffer_us - shared_offset_us;
    }
    this->deadline_on_shared_tsf_ = false;
  }
#endif

  this->filter_mutex_.lock();
  const double offset_ms = this->time_filter_.has_estimate() ? this->time_filter_.get_offset(now_us() / 1000.0) : 0.0;
  this->filter_mutex_.unlock();
  return rec.server_ts_us + buffer_us - static_cast<int64_t>(offset_ms * 1000.0);
}

void SnapcastClient::discard_ring_bytes_(size_t bytes) {
  while (bytes > 0 && !this->shutdown_.load(std::memory_order_relaxed)) {
    size_t n = this->pcm_ring_->read(this->slice_buffer_.get(), std::min(bytes, SLICE_BUFFER_SIZE),
                                     pdMS_TO_TICKS(100));
    bytes -= n;
  }
}

uint32_t SnapcastClient::push_silence_(uint32_t frames, const StreamParams &params) {
  if (this->audio_listener_ == nullptr) {
    return 0;
  }
  const uint32_t frame_bytes = params.frame_bytes();
  memset(this->slice_buffer_.get(), 0, SLICE_BUFFER_SIZE);
  uint32_t pushed = 0;
  while (pushed < frames && this->output_active_.load(std::memory_order_relaxed) &&
         !this->shutdown_.load(std::memory_order_relaxed)) {
    const uint32_t batch = std::min<uint32_t>(frames - pushed, SLICE_BUFFER_SIZE / frame_bytes);
    size_t written = this->audio_listener_->on_audio_write(this->slice_buffer_.get(), batch * frame_bytes, 100, params);
    if (written == 0) {
      break;
    }
    const uint32_t written_frames = written / frame_bytes;
    pushed += written_frames;
    this->playout_mutex_.lock();
    this->pushed_frames_total_ += written_frames;
    this->mark_playout_(this->pushed_history_, this->pushed_history_next_, now_us(), this->pushed_frames_total_);
    this->playout_mutex_.unlock();
  }
  return pushed;
}

// Polarity inversion with INT16_MIN clamp (negating INT16_MIN overflows)
static inline int16_t invert_sample(int16_t s) { return s == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-s); }

// THREAD CONTEXT: Player task. Slices are always frame-aligned: chunk sizes and
// SLICE_BUFFER_SIZE are multiples of the frame size, and reads are sequential.
void SnapcastClient::apply_channel_mode_(uint8_t *data, size_t len, const StreamParams &params) {
  if (params.bits_per_sample != 16) {
    return;
  }
  auto *samples = reinterpret_cast<int16_t *>(data);

  // Channel routing first: phase inversion below refers to the *output* channels
  const auto mode = static_cast<ChannelMode>(this->channel_mode_.load(std::memory_order_relaxed));
  if (mode != ChannelMode::STEREO && params.channels == 2) {
    const size_t frames = len / 4;
    switch (mode) {
      case ChannelMode::LEFT_ONLY:
        for (size_t i = 0; i < frames; i++) {
          samples[2 * i + 1] = samples[2 * i];
        }
        break;
      case ChannelMode::RIGHT_ONLY:
        for (size_t i = 0; i < frames; i++) {
          samples[2 * i] = samples[2 * i + 1];
        }
        break;
      case ChannelMode::MONO:
        for (size_t i = 0; i < frames; i++) {
          const int16_t mixed = static_cast<int16_t>((static_cast<int32_t>(samples[2 * i]) + samples[2 * i + 1]) / 2);
          samples[2 * i] = mixed;
          samples[2 * i + 1] = mixed;
        }
        break;
      default:
        break;
    }
  }

  const auto phase = static_cast<PhaseMode>(this->phase_mode_.load(std::memory_order_relaxed));
  if (phase != PhaseMode::NONE) {
    if (params.channels == 2) {
      const size_t frames = len / 4;
      const bool left = phase != PhaseMode::RIGHT;
      const bool right = phase != PhaseMode::LEFT;
      for (size_t i = 0; i < frames; i++) {
        if (left) {
          samples[2 * i] = invert_sample(samples[2 * i]);
        }
        if (right) {
          samples[2 * i + 1] = invert_sample(samples[2 * i + 1]);
        }
      }
    } else if (params.channels == 1) {
      // Mono has no L/R distinction; any inversion setting inverts the one channel
      const size_t n = len / 2;
      for (size_t i = 0; i < n; i++) {
        samples[i] = invert_sample(samples[i]);
      }
    }
  }
}

// THREAD CONTEXT: Player task
void SnapcastClient::push_repeat_frame_(const StreamParams &params) {
  const uint32_t frame_bytes = params.frame_bytes();
  if (this->audio_listener_ == nullptr || frame_bytes == 0 || frame_bytes > sizeof(this->last_frame_)) {
    return;
  }
  if (this->last_frame_bytes_ != frame_bytes) {
    // No cached frame in this format yet
    this->push_silence_(1, params);
    return;
  }
  size_t offset = 0;
  while (offset < frame_bytes && this->output_active_.load(std::memory_order_relaxed) &&
         !this->shutdown_.load(std::memory_order_relaxed)) {
    const size_t written =
        this->audio_listener_->on_audio_write(this->last_frame_ + offset, frame_bytes - offset, 100, params);
    if (written == 0) {
      return;
    }
    offset += written;
  }
  this->playout_mutex_.lock();
  this->pushed_frames_total_ += 1;
  this->playout_mutex_.unlock();
}

void SnapcastClient::push_chunk_(const ChunkRecord &rec, uint32_t drop_frames, bool silent) {
  const uint32_t frame_bytes = rec.params.frame_bytes();
  size_t remaining = rec.bytes;
  size_t skip = std::min<size_t>(static_cast<size_t>(drop_frames) * frame_bytes, remaining);

  while (remaining > 0 && !this->shutdown_.load(std::memory_order_relaxed)) {
    const size_t want = std::min(remaining, SLICE_BUFFER_SIZE);
    size_t got = 0;
    while (got < want && !this->shutdown_.load(std::memory_order_relaxed)) {
      got += this->pcm_ring_->read(this->slice_buffer_.get() + got, want - got, pdMS_TO_TICKS(100));
    }
    remaining -= got;

    if (silent) {
      memset(this->slice_buffer_.get(), 0, got);
    }
    this->apply_channel_mode_(this->slice_buffer_.get(), got, rec.params);

    // Cache the slice's final frame (post-transform) for click-free servo insertion
    if (got >= frame_bytes && frame_bytes <= sizeof(this->last_frame_)) {
      memcpy(this->last_frame_, this->slice_buffer_.get() + got - frame_bytes, frame_bytes);
      this->last_frame_bytes_ = frame_bytes;
    }

    size_t offset = 0;
    if (skip > 0) {
      offset = std::min(skip, got);
      skip -= offset;
    }

    uint32_t zero_writes = 0;
    while (offset < got) {
      if (this->audio_listener_ == nullptr || !this->output_active_.load(std::memory_order_relaxed) ||
          this->shutdown_.load(std::memory_order_relaxed)) {
        // Consumer went away mid-chunk: discard the rest, deadlines keep us honest
        this->discard_ring_bytes_(remaining);
        return;
      }
      size_t written = this->audio_listener_->on_audio_write(this->slice_buffer_.get() + offset, got - offset, 100,
                                                             rec.params);
      offset += written;
      if (written > 0) {
        zero_writes = 0;
        this->playout_mutex_.lock();
        this->pushed_frames_total_ += written / frame_bytes;
        this->mark_playout_(this->pushed_history_, this->pushed_history_next_, now_us(), this->pushed_frames_total_);
        this->playout_mutex_.unlock();
      } else if (++zero_writes >= 20) {
        // ~2 s of refused writes: the pipeline is wedged (observed on hardware --
        // the mixer stopped draining while still active, every write returned 0,
        // and this loop zombied the player task for minutes: no sync reports, no
        // recovery, silence). Drop the chunk and keep the player alive; the
        // deadline logic hard-resyncs through the outage and the starvation
        // re-baseline restores sync when the pipeline comes back.
        ESP_LOGW(TAG, "Pipeline refusing audio for 2 s, dropping chunk");
        this->discard_ring_bytes_(remaining);
        return;
      }
    }
  }
}

}  // namespace esphome::snapclient

#endif  // USE_ESP32
