#include "snapcast_client.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#ifdef AUDIO_TIMING_TSF_ACTIVE
#include "esphome/components/json/json_util.h"
#endif

#include <esp_timer.h>
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

// Median window for the steering servo's error signal. One sample per chunk, so its
// duration follows the codec block size -- measured 26.2 ms/chunk (FLAC's 1152-frame
// default at 44.1 kHz), giving ~0.39 s here. That duration is a term in the loop lag
// that bounds the trim gain, so it is not a free parameter: a codec or rate with a
// different block size changes it.
//
// Lengthened from 15 for stereo-image stability. The per-chunk error is WHITE NOISE
// (measured: consecutive-difference sigma / sigma = 1.32-1.43 against sqrt(2) = 1.41),
// so averaging more of it reduces the residual as ~sqrt(n) and costs nothing in
// tracking -- there is no ramp being trailed. Note this is the opposite of the earlier
// attempt to SHORTEN the window to buy loop bandwidth: with a noise-dominated error,
// bandwidth pumps noise into the output and length removes it. Added lag is ~0.2 s
// (half-window), keeping phase margin comfortable at KP = 0.5.
static constexpr size_t MEDIAN_WINDOW = 31;

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

// Median error below which our playout counts as tracking the timebase, reported
// to the TSF layer for leader eligibility. Generous: this gates "am I fit to
// publish the group timebase", not servo precision.
static constexpr int64_t PLAYOUT_HEALTHY_US = 5000;

#ifdef USE_AUDIO_TIMING_RATE_LOCK
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
static constexpr float TRIM_KP_PPM_PER_US = 0.5f;
// Computed, not written out: KI = KP^2/4 is the critical-damping relationship above,
// and a literal lets the two drift apart silently. That happened -- a KP change had
// to have its KI recomputed by hand, and getting it wrong changes the damping with
// no compile error and no obvious symptom.
static constexpr float TRIM_KI_PPM_PER_US_S = TRIM_KP_PPM_PER_US * TRIM_KP_PPM_PER_US / 4.0f;
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
// speaker's no-data timeout (500 ms) cannot tear the pipeline down. There is
// deliberately no total cap: the keepalive runs for as long as the SESSION is up.
// See the stream-idle block in loop() for why, and for what that costs.
static constexpr int64_t KEEPALIVE_SLICE_US = 50000;

// At most one hard-resync log line this often; the sync report carries full counts
static constexpr int64_t RESYNC_LOG_INTERVAL_US = 2000000;
// How long the stream may stay staler than the server's whole buffer before the
// player gives up on it and forces a reconnect. Long enough that a burst of
// congestion which TCP eventually outruns is ridden out rather than punished
// (recovery needs delivery faster than real time, which a clearing radio provides
// in well under a second), short enough that the silent spiral is bounded.
static constexpr int64_t STALE_BAILOUT_US = 3000000;

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
static constexpr uint32_t RESYNC_STORM_COUNT = 8;
static constexpr int64_t RESYNC_STORM_WINDOW_US = 2000000;

#ifdef AUDIO_TIMING_TSF_ACTIVE
// TSF unicast roster refresh cadence (only while no stream is active; blocking RPC)
static constexpr int64_t TSF_PEER_REFRESH_US = 60000000;
#endif

// Time-sync RTT gating (see handle_time_reply_)
static constexpr int64_t RTT_GATE_US = 20000;      // reject samples this far above the floor
static constexpr int64_t RTT_FLOOR_LEAK_US = 500;  // floor rises this much per sample (~0.5 ms/s)

static inline int64_t now_us() { return esp_timer_get_time(); }

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

#ifdef USE_AUDIO_TIMING_RATE_LOCK
  this->rate_lock_ = std::make_unique<RateLock>(this->config_.rate_lock_i2s_port);
#endif

#ifdef AUDIO_TIMING_TSF_ACTIVE
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

// THREAD CONTEXT: Speaker playback callback thread
void SnapcastClient::notify_audio_played(uint32_t frames, int64_t timestamp_us) {
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
      // runs on the speaker callback thread while on_query_buffered() is documented
      // player-task-only. Querying from here would break that contract to replace a
      // sound premise.
      this->pushed_frames_total_ = this->played_frames_total_ + frames;
      this->fb_samples_ = 0;
#ifdef USE_AUDIO_TIMING_RATE_LOCK
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
    frames = static_cast<uint32_t>(std::max<int64_t>(available_frames, 0));
  } else if (frames > 0) {
    this->starved_latched_ = false;  // real audio flowing again
  }
  this->played_frames_total_ += frames;
  this->played_last_ts_us_ = timestamp_us;
  this->playout_valid_ = true;

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
#ifdef AUDIO_TIMING_TSF_ACTIVE
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
  this->last_chunk_us_ = 0;

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
#ifdef AUDIO_TIMING_TSF_ACTIVE
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
  // While the SESSION is up we deliberately do NOT end the stream on a chunk gap.
  // Ending it tears the audio pipeline down (media source -> IDLE -> output
  // inactive), and rebuilding playout phase from nothing costs a mute plus 7-20 s of
  // re-lock. Measured on hardware: two ordinary inter-track gaps, 17 s and 18 s, each
  // forced a teardown; the first restart came back 31 ms late and corrected silently,
  // the second landed 61 ms late, tripped the 50 ms hard-resync threshold, and cost
  // an audible 7.2 s gap. Identical events either side of a coin flip. The player
  // task feeds keepalive silence across the whole gap instead, so playout phase, the
  // frame accounting and the TSF mapping all stay live and a resuming stream needs no
  // correction at all.
  //
  // Deliberate trade-off: the media player therefore reads PLAYING (silently) for as
  // long as the session is connected, instead of dropping to IDLE after
  // stream_idle_timeout. That option now only governs the disconnected case. TSF
  // beaconing and the fast time-sync cadence also stay engaged across gaps -- which
  // is the point, since a frozen mapping is what made a resuming group re-converge.
  if (this->stream_active_ && !this->connected_.load(std::memory_order_relaxed) &&
      now - this->last_chunk_us_ > static_cast<int64_t>(this->config_.stream_idle_timeout_ms) * 1000) {
    ESP_LOGD(TAG, "Stream idle for %" PRIu32 " ms, ending stream", this->config_.stream_idle_timeout_ms);
    this->set_stream_active_(false);
  }

#ifdef AUDIO_TIMING_TSF_ACTIVE
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

#ifdef AUDIO_TIMING_TSF_ACTIVE
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
  } else {
    ESP_LOGE(TAG, "Unsupported codec '%.*s' — set the snapserver stream codec to flac or pcm",
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

  this->last_chunk_us_ = now_us();
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
  bool warned_no_sync = false;
  // Rolling sync-error diagnostics, logged once per ~128 chunks (~3 s)
  int64_t err_accum_us = 0;
  int64_t err_peak_us = 0;
  uint32_t err_count = 0;
  // Per-window stutter forensics: how often each correction mechanism fired
  uint32_t soft_dropped_frames = 0;
  uint32_t soft_inserted_frames = 0;
  uint32_t hard_resyncs = 0;
  // Median of recent sync errors (rejects residual feedback spikes better than a
  // mean); the steering servo acts on this, not the raw per-chunk error. Same design
  // as the esp32 snapclient reference (99/19-sample medians on a sample-accurate age).
  int64_t err_window[MEDIAN_WINDOW];
  size_t err_window_idx = 0;
  size_t err_window_filled = 0;
  // Bang-bang steering with hysteresis, ported from the reference: while engaged,
  // trim exactly one frame per chunk (~950 ppm) until the median crosses back inside
  // the disengage threshold. Holds the error near zero continuously instead of
  // letting it random-walk inside a deadband — a free-walking deadband is exactly
  // what wanders the stereo image between two paired devices.
  int8_t steer_dir = 0;
#ifdef USE_AUDIO_TIMING_RATE_LOCK
  // Rate lock: once converged, steady-state corrections become hardware clock trims
  // instead of frame splices. The PI integrator (positive = play faster) persists
  // across resyncs, flushes, and rate changes because it converges to the crystal
  // offset, a property of the hardware, not the stream.
  float trim_integral_ppm = 0.0f;
  bool rate_lock_ok = this->rate_lock_ != nullptr;
  uint32_t rate_lock_rate = 0;
  // Trim wander over the report window. The trim IS the loop's estimate of the
  // disturbance it is cancelling, so its spread says whether the loop is tracking a
  // slow crystal offset (narrow, as designed) or chasing something it cannot (wide,
  // or pinned at the rail). Observed on all four devices: swings of hundreds of ppm
  // and repeated +-500 ppm saturation while medians stayed inside a few hundred us
  // -- i.e. running at its authority limit in normal operation. Quantify it before
  // any further gain change.
  float trim_min_ppm = 0.0f;
  float trim_max_ppm = 0.0f;
  uint32_t trim_samples = 0;
  uint32_t trim_railed = 0;
#endif
  uint32_t raw_sample_countdown = 1;
  // Smoothed accounted-vs-observed disagreement (us); 0 when the accounting is honest
  float fill_corr_us = 0.0f;
  bool fill_corr_valid = false;
  uint32_t fill_sample_countdown = 0;
  // Mute-until-synced: real audio flows only after a full window of in-band medians
  bool converged = false;
  uint32_t in_band_chunks = 0;
  int64_t last_resync_log_us = 0;
  // When the stream first went staler than the server's buffer, 0 while it is not
  int64_t stale_since_us = 0;
  // Rolling hard-resync count, for telling a one-shot catch-up from a storm
  int64_t storm_window_us = 0;
  uint32_t storm_resyncs = 0;
  // Format of the last chunk played, for keepalive silence during a delivery gap
  StreamParams keepalive_params{};
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
      if (keepalive_params.valid() && this->output_active_.load(std::memory_order_relaxed) &&
          this->is_connected()) {
        const uint32_t frames = keepalive_params.sample_rate / (1000000 / KEEPALIVE_SLICE_US);
        this->push_silence_(frames, keepalive_params);
      }
      continue;
    }
    keepalive_params = rec.params;

    const uint32_t frame_bytes = rec.params.frame_bytes();
    if (frame_bytes == 0) {
      this->discard_ring_bytes_(rec.bytes);
      continue;
    }

#ifdef USE_AUDIO_TIMING_RATE_LOCK
    if (rate_lock_ok && rec.params.sample_rate != rate_lock_rate) {
      // The speaker reprograms the I2S clock for a new stream format; re-read the
      // divider baseline once the new clock is running. The rate is what lets the
      // lock compute the IDEAL divider rather than inherit the driver's rounding of
      // it, which can otherwise eat the servo's whole trim authority.
      this->rate_lock_->set_output_rate(rec.params.sample_rate);
      this->rate_lock_->invalidate_baseline();
      rate_lock_rate = rec.params.sample_rate;
    }
#endif

    if (!this->output_active_.load(std::memory_order_relaxed)) {
      // No consumer: discard immediately. New chunks arrive continuously, so playback
      // starts in sync as soon as the source is activated.
      this->discard_ring_bytes_(rec.bytes);
      continue;
    }

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
      // on_query_buffered() reports bytes still held downstream, so seed pushed as
      // played + that, making the accounted queue equal the measured one. Falls back
      // to the old assume-empty behaviour when the sink cannot report, which is why
      // the query distinguishes "unknown" from "zero".
      //
      // Scope: the sink reports its own queue, not the mixer's output ring or the I2S
      // DMA below it. Those are bounded by buffer_duration and identical across
      // devices running one config, so what remains is common-mode -- and it is
      // relative offset between devices that moves a stereo image, not a shared
      // constant.
      size_t buffered_bytes = 0;
      const bool have_fill = this->audio_listener_ != nullptr && frame_bytes > 0 &&
                             this->audio_listener_->on_query_buffered(buffered_bytes);
      const int64_t in_flight_frames = have_fill ? static_cast<int64_t>(buffered_bytes / frame_bytes) : 0;
      this->playout_mutex_.lock();
      this->playout_valid_ = false;
      this->played_frames_total_ = 0;
      this->pushed_frames_total_ = in_flight_frames;
      this->fb_samples_ = 0;
      this->playout_mutex_.unlock();
      if (have_fill) {
        ESP_LOGD(TAG, "Re-baseline anchored to measured fill: %" PRId64 " frames (%" PRId64 " ms)", in_flight_frames,
                 in_flight_frames * 1000 / static_cast<int64_t>(rec.params.sample_rate));
      } else {
        // Log the FALLBACK too. Without this the two cases are indistinguishable in a
        // log -- a silent fallback looks exactly like the feature working, which is
        // how the first flash of this code read as "no starvations to anchor" when in
        // fact every one of ~120 starvations had taken this branch.
        ESP_LOGW(TAG, "Re-baseline could not read the pipeline fill; assuming empty (listener=%d)",
                 this->audio_listener_ != nullptr ? 1 : 0);
      }
      err_window_filled = 0;
      steer_dir = 0;
      converged = false;
#ifdef USE_AUDIO_TIMING_RATE_LOCK
      if (this->rate_lock_ != nullptr) {
        this->rate_lock_->invalidate_baseline();
      }
#endif
      ESP_LOGI(TAG, "Pipeline drained (source starvation); re-baselining playout");
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
      if (!warned_no_sync) {
        this->filter_mutex_.lock();
        warned_no_sync = !this->time_filter_.has_estimate();
        this->filter_mutex_.unlock();
        if (warned_no_sync) {
          ESP_LOGW(TAG, "Starting playback before first time sync; expect a hard resync");
        }
      }
      this->push_chunk_(rec, 0, true);
      continue;
    }

    // Refresh the padding estimate: measured fill (rings + FULL DMA span, padding
    // included) minus what the accounting believes is outstanding (real frames only).
    // Diagnostics-only since the correction is no longer applied, so the listener-chain
    // walk and mutex are not worth paying for unless timing diagnostics are wanted.
#ifdef USE_SNAPCLIENT_TIMING_DIAG
    if (fill_sample_countdown == 0) {
      fill_sample_countdown = FILL_SAMPLE_EVERY_CHUNKS;
      size_t measured_bytes = 0;
      if (this->audio_listener_ != nullptr && frame_bytes > 0 &&
          this->audio_listener_->on_query_buffered(measured_bytes)) {
        this->playout_mutex_.lock();
        const int64_t accounted_frames = this->pushed_frames_total_ - this->played_frames_total_;
        this->playout_mutex_.unlock();
        const int64_t measured_frames = static_cast<int64_t>(measured_bytes / frame_bytes);
        const int64_t sample_us =
            (measured_frames - accounted_frames) * 1000000 / static_cast<int64_t>(rec.params.sample_rate);
        if (std::abs(sample_us) <= FILL_CORR_MAX_US) {
          fill_corr_us = fill_corr_valid
                             ? fill_corr_us + FILL_EWMA_ALPHA * (static_cast<float>(sample_us) - fill_corr_us)
                             : static_cast<float>(sample_us);
          fill_corr_valid = true;
        }
      }
    } else {
      fill_sample_countdown--;
    }
#endif  // USE_SNAPCLIENT_TIMING_DIAG

    // fill_corr_us is MEASURED AND REPORTED BUT NOT APPLIED. Applying it was wrong and
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

#ifdef AUDIO_TIMING_TSF_ACTIVE
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
    if (this->tsf_sync_ != nullptr && --raw_sample_countdown == 0) {
      raw_sample_countdown = RAW_SAMPLE_EVERY_CHUNKS;
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
#endif  // AUDIO_TIMING_TSF_ACTIVE


    err_accum_us += error_us;
    err_peak_us = std::max(err_peak_us, std::abs(error_us));

    err_window[err_window_idx] = error_us;
    err_window_idx = (err_window_idx + 1) % MEDIAN_WINDOW;
    if (err_window_filled < MEDIAN_WINDOW) {
      err_window_filled++;
    }
    int64_t median_err_us = error_us;
    if (err_window_filled == MEDIAN_WINDOW) {
      int64_t sorted[MEDIAN_WINDOW];
      memcpy(sorted, err_window, sizeof(sorted));
      std::nth_element(sorted, sorted + MEDIAN_WINDOW / 2, sorted + MEDIAN_WINDOW);
      median_err_us = sorted[MEDIAN_WINDOW / 2];
    }

    if (++err_count >= 128) {
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
      int32_t fill_ms = -1;
      int32_t fill_drift_ms = 0;
      {
        size_t measured_bytes = 0;
        if (this->audio_listener_ != nullptr && frame_bytes > 0 &&
            this->audio_listener_->on_query_buffered(measured_bytes)) {
          const int64_t measured_frames = static_cast<int64_t>(measured_bytes / frame_bytes);
          fill_ms = static_cast<int32_t>(measured_frames * 1000 / static_cast<int64_t>(rec.params.sample_rate));
          fill_drift_ms = pipeline_ms - fill_ms;
        }
      }
      char fill_str[80] = "";
      if (fill_ms >= 0) {
        snprintf(fill_str, sizeof(fill_str), ", fill %" PRId32 " ms (drift %+" PRId32 ", corr %+d ms)", fill_ms,
                 fill_drift_ms, static_cast<int>(fill_corr_us / 1000.0f));
      }
      // Ring occupancy shows how much dropout cushion is actually held client-side
      const uint32_t buffered_ms = static_cast<uint32_t>(
          static_cast<uint64_t>(this->pcm_ring_->available()) * 1000 / (frame_bytes * rec.params.sample_rate));
      char trim_str[112] = "";
#ifdef USE_AUDIO_TIMING_RATE_LOCK
      if (rate_lock_ok) {
        if (trim_samples > 0) {
          snprintf(trim_str, sizeof(trim_str),
                   ", trim %+.2f ppm (span %+.0f..%+.0f, railed %" PRIu32 "/%" PRIu32 ")",
                   this->rate_lock_->applied_ppm(), trim_min_ppm, trim_max_ppm, trim_railed, trim_samples);
        } else {
          snprintf(trim_str, sizeof(trim_str), ", trim %+.2f ppm (idle)", this->rate_lock_->applied_ppm());
        }
      }
#endif
      char tsf_str[64] = "";
#ifdef AUDIO_TIMING_TSF_ACTIVE
      if (this->tsf_sync_ != nullptr) {
        // Publish our depth so the group can cross-check it (see TsfSync)
        this->tsf_sync_->set_pipeline_ms(pipeline_ms);
        const TsfSync::Role role = this->tsf_sync_->role();
        if (role == TsfSync::Role::LEADER) {
          snprintf(tsf_str, sizeof(tsf_str), ", tsf=leader(peers %u)", this->tsf_sync_->peer_count());
        } else if (role == TsfSync::Role::FOLLOWER) {
          // depth delta vs the leader: the only visibility we have into an absolute
          // playout offset, which the median above cannot show by construction
          const int32_t depth_delta = this->tsf_sync_->pipeline_delta_ms();
          if (depth_delta == INT32_MIN) {
            snprintf(tsf_str, sizeof(tsf_str), ", tsf=follower(%.1fs)", this->tsf_sync_->mapping_age_s(now_us()));
          } else {
            snprintf(tsf_str, sizeof(tsf_str), ", tsf=follower(%.1fs, depth %+" PRId32 " ms)",
                     this->tsf_sync_->mapping_age_s(now_us()), depth_delta);
          }
        }
      }
#endif
      ESP_LOGD(TAG,
               "Sync: avg %" PRId64 " us, peak %" PRId64 " us, median %" PRId64
               " us | corrected -%" PRIu32 "/+%" PRIu32 " frames, %" PRIu32 " hard resyncs, feedback %" PRId64
               " us mean / %" PRId64 " ms max, buffered %" PRIu32 " ms, pipeline %" PRId32 " ms%s%s%s over %" PRIu32
               " chunks",
               err_accum_us / err_count, err_peak_us, median_err_us, soft_dropped_frames, soft_inserted_frames,
               hard_resyncs, fb_mean_gap_us, max_gap_us / 1000, buffered_ms, pipeline_ms, fill_str, trim_str,
               tsf_str, err_count);
      err_accum_us = 0;
      err_peak_us = 0;
      err_count = 0;
      soft_dropped_frames = 0;
      soft_inserted_frames = 0;
      hard_resyncs = 0;
#ifdef USE_AUDIO_TIMING_RATE_LOCK
      trim_samples = 0;
      trim_railed = 0;
#endif
    }

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
    const int64_t stale_us =
        std::max<int64_t>(static_cast<int64_t>(this->buffer_ms_.load(std::memory_order_relaxed)),
                          static_cast<int64_t>(this->config_.hard_resync_threshold_ms)) *
        1000;
    if (error_us > stale_us) {
      if (stale_since_us == 0) {
        stale_since_us = now_us();
      } else if (now_us() - stale_since_us >= STALE_BAILOUT_US) {
        ESP_LOGW(TAG, "Stream %" PRId64 " ms late for %" PRId64 " s and not catching up: reconnecting",
                 error_us / 1000, STALE_BAILOUT_US / 1000000);
        stale_since_us = 0;
        // Breaks recv_exact_ out of the session; the network task reconnects with no
        // backoff, and connection_session_() clears the flag and resets the time filter
        this->reconnect_requested_.store(true, std::memory_order_relaxed);
      }
    } else {
      stale_since_us = 0;
    }

    // Decide ONCE, for both directions, whether this excursion should mute. Muting is
    // for storms; a lone splice is cheaper than a re-lock. Magnitude still overrides
    // the count: both devices once logged a simultaneous 24888016 ms error (a ~6.9 h
    // timebase step) with only 5 resyncs, and playing audibly toward something that
    // far outside the server's buffer is meaningless -- so anything past bufferMs
    // mutes on the spot, and the bailout above reconnects if it persists.
    bool mute_now = false;
    if (std::abs(error_us) > hard_us) {
      if (now_us() - storm_window_us > RESYNC_STORM_WINDOW_US) {
        storm_window_us = now_us();
        storm_resyncs = 0;
      }
      storm_resyncs++;
      mute_now = storm_resyncs >= RESYNC_STORM_COUNT || std::abs(error_us) > stale_us;
      if (converged && !mute_now && storm_resyncs == 1) {
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
      if (now_us() - last_resync_log_us >= RESYNC_LOG_INTERVAL_US) {
        last_resync_log_us = now_us();
        ESP_LOGD(TAG, "Hard resync: %" PRId64 " ms late, dropping chunks (throttled log)", error_us / 1000);
      }
      hard_resyncs++;
      err_window_filled = 0;
      steer_dir = 0;
      // INFO on the true->false edge: this is the moment audio goes silent, and it
      // is the only user-audible event in the loop. Logging only the re-lock (which
      // is INFO) made a dropout look like a spontaneous "Sync locked" with no cause,
      // since the resync line above is DEBUG and throttled. One line per gap.
      if (converged && mute_now) {
        ESP_LOGI(TAG, "Muting: hard resync, %" PRId64 " ms late (%" PRIu32
                      " in %" PRId64 " s) -- audible gap until re-lock",
                 error_us / 1000, storm_resyncs, RESYNC_STORM_WINDOW_US / 1000000);
      }
      converged = converged && !mute_now;
      this->discard_ring_bytes_(rec.bytes);
      continue;
    }

    uint32_t drop_frames = 0;
    if (error_us < -hard_us) {
      // Hard resync, early: fill the gap with silence (bounded per chunk so the
      // loop stays responsive), keeping the DAC fed and continuous
      const int64_t gap_frames = (-error_us) * rec.params.sample_rate / 1000000;
      const uint32_t fill = std::min<int64_t>(gap_frames, rec.params.sample_rate / 2);
      if (now_us() - last_resync_log_us >= RESYNC_LOG_INTERVAL_US) {
        last_resync_log_us = now_us();
        ESP_LOGD(TAG, "Hard resync: %" PRId64 " ms early, inserting silence (throttled log)", -error_us / 1000);
      }
      hard_resyncs++;
      err_window_filled = 0;
      steer_dir = 0;
      if (converged && mute_now) {
        ESP_LOGI(TAG, "Muting: hard resync, %" PRId64 " ms early (%" PRIu32
                      " in %" PRId64 " s) -- audible gap until re-lock",
                 -error_us / 1000, storm_resyncs, RESYNC_STORM_WINDOW_US / 1000000);
      }
      converged = converged && !mute_now;
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
        soft_dropped_frames += adjust;
      } else if (adjust < 0) {
        soft_inserted_frames += -adjust;
        this->push_silence_(-adjust, rec.params);
      }
      steer_dir = 0;
    } else if (err_window_filled == MEDIAN_WINDOW) {
      // Steering servo (reference design): engage when the median error exceeds
      // sync_deadband, then trim exactly one frame (~23 us splice, inaudible) per
      // chunk until it crosses back inside half the threshold. Continuous hold near
      // zero is what keeps a stereo pair's image pinned.
      const int64_t engage_us = this->config_.sync_deadband_us;
      if (steer_dir == 0) {
        if (median_err_us > engage_us) {
          steer_dir = 1;
        } else if (median_err_us < -engage_us) {
          steer_dir = -1;
        }
      } else if ((steer_dir > 0 && median_err_us < engage_us / 2) ||
                 (steer_dir < 0 && median_err_us > -engage_us / 2)) {
        steer_dir = 0;
      }
      bool trim_holds = false;
#ifdef USE_AUDIO_TIMING_RATE_LOCK
      // Steady-state rate lock: steer the I2S clock instead of splicing frames.
      // Continuous PI on the median error, no deadband -- trims are inaudible, and
      // gating them through the hysteresis band re-creates the limit cycle. The
      // hysteresis/steer_dir path above still drives the splice fallback. Muted
      // convergence uses hard splices while far out (much faster than the trim
      // slew), handing off to the PI for the end-game so the error actually
      // settles inside the band instead of splice-limit-cycling around it.
      if (rate_lock_ok && (converged || std::abs(median_err_us) <= this->config_.converge_fine_us)) {
        const float dt_s = static_cast<float>(frames) / rec.params.sample_rate;
        const float clamp_ppm = trim_clamp_ppm(this->config_.converge_fine_us);
        const float p_term = TRIM_KP_PPM_PER_US * static_cast<float>(median_err_us);
        // Conditional integration (anti-windup): winding the integral while the
        // output rails just schedules a rail-to-rail relaxation oscillation
        // (observed post-boot: trim flipping +-500 ppm with +-5 ms medians for
        // ~90 s, with audible correction bursts). Freeze the integral whenever the
        // output is saturated in the error's own direction.
        const float unclamped = p_term + trim_integral_ppm;
        if (std::abs(unclamped) < clamp_ppm || (unclamped > 0.0f) != (median_err_us > 0)) {
          trim_integral_ppm = std::clamp(
              trim_integral_ppm + TRIM_KI_PPM_PER_US_S * static_cast<float>(median_err_us) * dt_s, -clamp_ppm,
              clamp_ppm);
        }
        const float trim_ppm = std::clamp(p_term + trim_integral_ppm, -clamp_ppm, clamp_ppm);
#ifdef USE_SNAPCLIENT_TIMING_DIAG
        // Report-only: span shows whether the loop tracks a slow offset or chases
        // something it cannot, and railed counts saturation.
        if (trim_samples == 0) {
          trim_min_ppm = trim_max_ppm = trim_ppm;
        } else {
          trim_min_ppm = std::min(trim_min_ppm, trim_ppm);
          trim_max_ppm = std::max(trim_max_ppm, trim_ppm);
        }
        trim_samples++;
        if (std::abs(trim_ppm) >= clamp_ppm - 0.5f) {
          trim_railed++;
        }
#endif
        trim_holds = this->rate_lock_->set_trim_ppm(trim_ppm);
        if (!trim_holds) {
          rate_lock_ok = false;
          ESP_LOGW(TAG, "Rate lock unavailable, falling back to frame-splice corrections");
        }
      } else if (rate_lock_ok) {
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
        // `converged || in-band`, so this branch means !converged && out-of-band),
        // so the rate change cannot be audible. trim_holds stays false on purpose,
        // to keep the coarse splice path engaged.
        this->rate_lock_->set_trim_ppm(0.0f);
      }
#endif
      // While muted (pre-convergence) audibility doesn't constrain splice size, so
      // steer hard to reach the band quickly, then single frames for the end-game
      if (!trim_holds) {
        const uint32_t steer_frames = (converged || std::abs(median_err_us) <= this->config_.converge_fine_us)
                                          ? 1
                                          : startup_steer_frames(frames);
        if (steer_dir > 0) {
          drop_frames = steer_frames;
          soft_dropped_frames += steer_frames;
        } else if (steer_dir < 0) {
          soft_inserted_frames += steer_frames;
          if (converged) {
            this->push_repeat_frame_(rec.params);
          } else {
            this->push_silence_(steer_frames, rec.params);
          }
        }
      }
    }

#ifdef AUDIO_TIMING_TSF_ACTIVE
    // Report our own tracking quality to the TSF layer: a leader publishes the
    // timebase the whole group follows, so it must hand off while its own playout
    // is diverged (observed: a device stuck in a degraded buffer state kept
    // leading, with every peer following its mapping)
    if (this->tsf_sync_ != nullptr) {
      this->tsf_sync_->set_playout_healthy(converged && err_window_filled == MEDIAN_WINDOW &&
                                           std::abs(median_err_us) < PLAYOUT_HEALTHY_US);
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
    // post-boot mutes to ~20 s of counter resets. Corrections inside 2x deadband
    // are trim-only and inaudible.
    if (std::abs(median_err_us) <= 2 * this->config_.sync_deadband_us) {
#ifdef AUDIO_TIMING_TSF_ACTIVE
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
      if (!converged && err_window_filled == MEDIAN_WINDOW && timebase_settled && ++in_band_chunks >= MEDIAN_WINDOW) {
        converged = true;
        ESP_LOGI(TAG, "Sync locked (median %" PRId64 " us), unmuting", median_err_us);
      }
    } else {
      in_band_chunks = 0;
    }

    this->push_chunk_(rec, drop_frames, !converged);
  }
  vTaskDelete(nullptr);
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

#ifdef AUDIO_TIMING_TSF_ACTIVE
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
