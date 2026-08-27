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

// Unmute gate width, in multiples of sync_deadband (128 us default -> 256 us).
//
// REVERTED from 8x. The widening was justified by a measurement taken with KP = 0.1
// running EVERYWHERE (only 54-57% of medians in band, lock 31-35 s), before acquisition
// was given its own gain. Acquisition now runs at KP = 0.5 -- the historical value, which
// held the error inside 256 us for as long as this project has existed -- so the problem
// the widening solved had already been solved by the gain split, and was never
// re-measured after it.
//
// The cost was real. A wider gate lets a board unmute up to ~1 ms from its target, and
// after independent re-baselines two boards cross it at very different errors (measured
// 845 us and 85 us). That error then has to be nulled at the low run gain: from a cold
// start the pair peaked 1.198 ms apart at 21 s and was still 290 us apart at 75 s. Four
// times further out than 2x allows, then five times slower to correct.
//
// The reasoning for widening was also wrong where it mattered: it argued both boards
// unmute at correlated errors, which holds in steady state (differential sd ~30 us) but
// demonstrably not after a re-baseline -- which is the case the gate exists for.
static constexpr int64_t UNMUTE_BAND_DEADBANDS = 2;

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
// Two gains, switched on st.converged (see the PI block). ACQUIRE is the historical value
// and runs while muted, where nothing is audible so amplified noise costs nothing. RUN
// takes over once unmuted, where the error is mostly differential measurement noise and the
// gain is the multiplier turning that noise into audible inter-device skew.
static constexpr float TRIM_KP_ACQUIRE_PPM_PER_US = 0.5f;
static constexpr float TRIM_KP_RUN_PPM_PER_US = 0.25f;
// DECAY between them, keyed on TIME SINCE THE LAST DISTURBANCE EVENT rather than on a single step
// at convergence. See trim_kp_() for the schedule and mark_kp_event_() for what counts as an event.
//
// Why a schedule at all: KP trades three things, not two. High gain nulls a disturbance fast, which
// both shortens recovery AND shrinks the offset the recovery leaves behind -- the wire offset is
// the integral of the differential rate, so the planted offset is set by how long the integral runs
// before the servo nulls it. Low gain is what keeps steady-state differential noise small, and that
// noise is audible skew. A fixed value has to pick one; that is why 0.1 lost (tau 80 s, landed
// -155 us) and why 0.25 is a compromise rather than a choice.
//
// Why THIS schedule and not the two that failed: both earlier attempts scheduled the gain on the
// error, which is the variable the gain controls -- the loop trails a ramp by rate/KP, so raising
// KP shrinks the very quantity the schedule reads, and it limit-cycles structurally. A timer since
// a discrete event has no path from the gain back to the scheduling input; it is open-loop in the
// error by construction, which is the only property that makes a schedule safe here.
//
// The residual feedback path, stated so it can be checked rather than assumed: a hard resync IS an
// event, and resyncs are triggered by error. But that trigger is 50 ms against a steady-state error
// of single-digit us -- four orders of magnitude of separation -- and the measured triggers are
// supply outages, not gain. If resyncs ever start firing at a rate that tracks KP, this schedule is
// the first suspect.
//
// TAU = 20 s: recovery at 0.25 measures tau ~14 s and ~54 s to settle, so the gain must stay high
// across the fine settling that follows an unmute and be back at RUN before the next quiet window
// is graded. Three tau (60 s) covers it. Endpoints are DELIBERATELY UNCHANGED from the fixed
// switch, so the first measurement grades the schedule alone; lowering the RUN endpoint toward 0.1
// is the follow-up the schedule is supposed to make affordable, and it is a separate change with
// its own measurement.
static constexpr float TRIM_KP_DECAY_TAU_S = 20.0f;
// Past this age the decay is inside 5% of RUN, so it is snapped there -- both to keep the gain
// exactly comparable to the old fixed value in steady state and to skip the expf on every chunk.
static constexpr float TRIM_KP_DECAY_SPAN_S = 3.0f * TRIM_KP_DECAY_TAU_S;
// TRIED AT 0.1 AND REVERTED. The loop-gain argument for lowering it is still sound -- the loop is
// median -> trim (KP) -> achieved rate -> pivot bias (3.15 us/ppm) -> median, so its gain is
// KP * 3.15, and 0.79 at 0.25 against 0.32 at 0.1 removes a 1/(1-G) ~ 4.8x amplification on top of
// the linear factor. What the measurement showed is that the cost side was underpriced, in a way
// the original note did not consider at all.
//
// Measured at 0.1 on a recovery, on the analyser: trough +548 us, tau 80 s, SETTLED 295 s --
// against ~42 s at 0.25, and worse than the ~135 s this was expected to cost. But the number that
// decided it is the one nobody had priced: the offset the recovery LEAVES BEHIND. It landed at
// -155 us, outside the +-130 us band recorded at 0.25.
//
// That is not a coincidence and it is the point. The wire offset is the integral of the
// differential rate, so a recovery freezes wherever the integral has got to when the servo finally
// nulls the rate. A lower gain nulls it more slowly, so the integral runs for longer and the
// planted offset is LARGER. KP therefore trades steady-state noise against both recovery time and
// the size of the static offset every event leaves -- and the second was the whole problem this
// work is trying to solve.
//
// Baseline it had to beat, measured in steady state with both instruments agreeing (KP = 0.25,
// 180 s): wire offset sd 6.20 us, differential achieved rate sd 1.515 ppm, on-device differential
// median MAD 6.00 us. Judge any future attempt on those, NOT on sd of the differential median --
// network events put that at 209 us against a MAD of 6. And judge it on the LANDING OFFSET after
// a lone restart, which is the term that was missed.
//
// Lowering KP is only worth revisiting once the re-baseline anchor stops planting an offset in the
// first place (see the seed path): with little left to converge from, the integration window
// shrinks and the landing-offset cost goes with it.
//
// 0.1 -> 0.25 buys recovery time at the cost of steady-state skew, and the two are the SAME
// dial: for this plant the error obeys e'' + KP*e' + KI*e = 0, so the envelope decays as
// e^(-KP/2 * t) and the sqrt(KI) cancels. Settling depends on KP alone -- raising KI changes
// only damping and overshoot, which is why the integrator is no help here.
//
// Measured at 0.1, recovering from a power cycle on a logic analyser: trough -620 us, back to
// -50 us over ~86 s, i.e. tau ~34 s and ~135 s to settle. Two minutes of audible drift after
// every reboot. At 0.25 that scales to tau ~14 s, ~54 s to settle.
//
// The cost is differential noise, which scales linearly with KP: measured 45-100 us excursions
// at KP = 0.5 and ~15 us at 0.1, so ~37 us here -- under two frames, and well inside the
// 100 us this whole line of work started from.
//
// This is a COMFORT SETTING, not a fix. The reason a reboot starts 620 us out at all is the
// re-baseline anchor planting an offset (see the disproven-hypotheses note in the seed path).
// Fix that and there is little left to converge from, and KP stops mattering.
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
// Trim time-mean cadence. Independent of the 128-chunk sync report because that boundary also
// gates the accounting-split repair, and because a diagnostic must be throttled by time rather
// than iteration count. Measured budget when this was chosen: 2.5 log lines/s/device total,
// against the ~38/s the config flags as enough to stall an OTA -- so ~1 s is affordable where
// the per-chunk 38/s is not. The wire is sampled at ~58 Hz, so the firmware series is the
// coarse one in every comparison; this closes part of that gap without touching the statistic,
// since a time-mean is a time-mean at any window length.
static constexpr int64_t TRIM_WINDOW_LOG_INTERVAL_US = 1000000;
// Per-chunk resync trace: how many chunks, and how often the burst may re-arm. 80 chunks is
// ~2.1 s at the 26 ms cadence when playing normally and much less during a storm, where the
// loop discards as fast as it iterates -- which is the case worth capturing. Matches the
// existing seed trace's budget, so the burst cost is already precedented. Rate-limited to once
// per 10 s so a sustained storm produces one readable burst rather than a flood competing with
// the audio stream for the same congested link.
static constexpr uint16_t RESYNC_TRACE_CHUNKS = 80;
static constexpr int64_t RESYNC_TRACE_ARM_INTERVAL_US = 10000000;
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
// 20 ms -> 2 ms. The old value was a noise floor, not a tolerance: with the repair keyed on a
// single end-of-window sample it had to clear an artefact that reached tens of milliseconds.
// Keyed on the median that artefact is gone -- measured +0/-1 us per window on both boards in
// steady state -- so the floor is now ~1 us and 2 ms is three orders of magnitude above it.
//
// This matters for real offsets, not just tidiness: the smallest split measured on the wire
// tonight was 8.5 ms, which sat BELOW the old threshold and was therefore unrepairable however
// long it persisted. Every offset in the session (8.5, 28, 67, 73, 191, 198 ms) clears 2 ms.
static constexpr int32_t DRIFT_REPAIR_US = 2000;
// Bound on a believable r_push (pushed - src_received): what is genuinely in flight between our
// push point and the mixer's source queue is the slice buffer plus a chunk or two. 10000 frames is
// ~227 ms at 44.1 kHz, several times that, so this refuses only the counter pairs that were
// re-zeroed independently -- which is a third of all samples, measured.
static constexpr int64_t RPUSH_VALID_FRAMES = 10000;
// How often to publish the validity fraction. Not per chunk: the number is a property of a window,
// and the point is to make an input's trustworthiness visible, not to add traffic.
static constexpr int64_t RPUSH_LOG_INTERVAL_US = 10000000;
// Chunk divisor for the drift distribution sampling (see the sampler in the player loop).
static constexpr uint32_t DRIFT_SAMPLE_EVERY_CHUNKS = 4;
// A real split is STEADY: the original was observed rock-steady at +50.7 ms for 18
// minutes. Measurement artefacts are not -- with a mixer in the chain, drift sawtooths
// between ~0 and -100 ms as a source ring fills and drains, and a threshold test alone
// happily fires on the peaks. Requiring the spread across the hold window to stay inside
// this band is what distinguishes the two, and it is the property the hold was always
// meant to test.
// 10 ms -> 2 ms, for the same reason as the threshold: this bounded the artefact's spread
// through the window, and on a median the spread of a genuine split is microseconds. Kept well
// above that rather than at the floor, because a split that is still settling should restart
// the window rather than be repaired mid-move.
static constexpr int32_t DRIFT_STEADY_BAND_US = 2000;
// How close the mixer's conservation residual has to be to the split before the split is treated as
// the residual's doing rather than the accounting's. Measured agreement was 1 us (drift +25509 us
// against r_mix 25510 us); the band is wide enough for snapshot rounding and nothing else.
static constexpr int32_t MIX_RESIDUAL_MATCH_US = 2000;
// Held this long before acting: a real split is rock-steady (18 minutes at +50.7),
// while a refill transient is not, and repairing a transient would inject the error
// it is meant to remove.
// 10 s -> 3 s. The hold had two jobs: out-wait the sampling artefact, and let a post-event
// transient settle. The median does the first instantly, so only the second remains -- and it
// is short: measured across an injected starvation, the split was constant to the microsecond
// (-28527 every report) from ~3 s after unmute, so a 3 s hold would have fired ~7 s earlier
// while still observing a steady value. The band test below remains the real guard: anything
// still moving restarts the window regardless of how long it has been open.
static constexpr int64_t DRIFT_REPAIR_HOLD_US = 3000000;
// RE-ANCHOR AFTER A SESSION START, opt-in (config_.reanchor_after_reconnect).
//
// A reconnect rebuilds the pipeline and re-anchors the playout accounting, and the per-device error
// in that anchor becomes a permanent static offset which no on-device field can see -- measured
// twice on 2026-08-26 as ~1.3 and ~1.4 ms of wire offset planted by an outage's reconnect, with
// every on-device metric reading healthy either side of it.
//
// What is measured (n=12, quiet-gated, injected splits on one board): a repair removes about TWO
// THIRDS of whatever standing offset the device carries, per firing -- post = +0.33 x pre - 6.4 --
// and two of them took a board from -1377 us back to -35 us. Its own displacement, seen where the
// device is already aligned, is +-50 us. So the repair is corrective, and forcing one after a
// session start is the cheapest way to spend it on the offset a reconnect just planted.
//
// What is NOT established is WHY, and the honest version of that matters here: the standing offset
// does not show up in `drift` (a fully-accounted device reads ~7 us, and the injection that
// recovered 1.3 ms measured +2562 us, i.e. only the injection). So this forces the repair by
// biasing the accounting, exactly as the test hook does, rather than by any principled re-derivation
// -- which is why it is OFF by default and gated on a config flag until a lone reconnect has been
// graded with it. If the effect does not reproduce for naturally planted offsets, the cost is one
// hold and its +-50 us per reconnect, and this comes straight back out.
//
// The bias must exceed DRIFT_REPAIR_US to be detected at all; 2500 us is what every measured point
// used. It is ramped by the same machinery as the test hook, so nothing is stepped.
static constexpr int32_t REANCHOR_BIAS_US = 2500;
// How long after the unmute to wait before perturbing. Long enough that the servo has finished the
// fine settling the re-lock ends with -- perturbing into that would measure the settling, not the
// anchor -- and short enough to be over before a listener has settled in.
static constexpr int64_t REANCHOR_SETTLE_US = 10000000;
// Floor on the gap between forced cycles. A storm can plant several re-locks in a minute and each
// cycle costs a ramp, a hold and its +-50 us; since a repair removes a FRACTION of the standing
// error rather than all of it, skipping one loses nothing the next event's cycle cannot pick up.
static constexpr int64_t REANCHOR_MIN_INTERVAL_US = 60000000;

// FAST POSITION CORRECTION, opt-in (config_.fast_splice_threshold_us; 0 disables).
//
// The servo can only steer RATE, so every standing offset has to be integrated away: the envelope
// decays as e^(-KP/2 t), which measures tau ~14 s and ~42 s to settle at KP = 0.25. The gain cannot
// simply be raised -- the feedback pivot means the loop measures its own output at 3.15 us/ppm, so
// loop gain is KP x 3.15 = 0.79 already, and differential trim noise (audible skew) scales linearly
// with KP. That is the whole 0.5/0.25/0.1 argument and it has been had three times.
//
// But AUTHORITY is not the constraint. A single-frame splice is ~23 us and is documented inaudible;
// the trim clamp is +-1000 ppm = 1 ms/s. A 1 ms offset is 43 frames -- about a second at one frame
// per chunk -- against ~40 s of integrating it away. The reason it never happens while playing is
// that the splice path sits behind `if (!trim_holds)`, so whenever the rate lock is programming,
// splices are off entirely. That is deliberate (splices limit-cycle around the deadband, which is
// why the PI owns the end-game) and it leaves a converged device with a millisecond of standing
// offset no fast way to spend it -- while the events that plant milliseconds happen several times
// an hour.
//
// So: splice ONLY well above the band, one frame per chunk, and hand back to the PI inside it.
// Engaging at 1 ms rather than at the deadband is what keeps this away from the limit cycle: the
// PI still owns everything below, and 1 ms is 8x converge_fine.
static constexpr int64_t FAST_SPLICE_RELEASE_US = 300;
// Bound on one episode, so a mis-measurement cannot walk the audio indefinitely: 128 frames is
// ~2.9 ms at 44.1 kHz, comfortably more than any planted offset measured (1.4 ms) and far less
// than the server's buffer. An episode that hits this is a bug report, not a correction.
static constexpr uint32_t FAST_SPLICE_MAX_FRAMES = 128;
// Do not chase an ACCOUNTING STEP by position. A repair moves the prediction by the size of the
// split -- ~2.5 ms for a forced re-anchor -- and the median error jumps by that much although no
// audio has moved. Position correction answering that would splice real frames against a
// bookkeeping change, and since the forced re-anchor's bias is always above the engage threshold,
// every re-anchor would be followed by one.
//
// Observed on the first firing: a repair at 18:11:10 was followed 0.4 s later by "Fast splice
// engaged: -2315 us standing", 62 frames in 1.65 s -- and the wire moved neither its offset
// (-46.9 -> -50.0 us) nor its frame_lag (-2 -> -1), where 62 frames is 1.4 ms of content and
// should have shown as one or the other. Whether that means the splice corrected the prediction
// without moving audio, or the analyser masked a real shift (rival was 0.89, i.e. an ambiguous
// correlation lock), is NOT established -- and until it is, these two mechanisms must not act on
// each other. The PI keeps the repair's step, which is what every landing-offset measurement so
// far was taken with.
static constexpr int64_t FAST_SPLICE_REPAIR_HOLDOFF_US = 30000000;
// AND THE ERROR HAS TO PERSIST. Position correction spends real frames, so it must not answer a
// transient -- and the first thing it did in the field was answer one.
//
// Measured 2026-08-27 00:02:33: BOTH boards engaged within 100 ms of each other, at +2733 and
// +2738 us. Two devices do not independently acquire the same 2.7 ms displacement; that is a
// COMMON-MODE step, the whole group's deadline moving together, and common-mode error cancels
// between devices -- it is not skew and needs no correction. Each board spent ~75 frames of real
// audio on it, and one of them re-engaged 5 s later at -1001 us, which is what the old splice
// servo's limit cycle looked like.
//
// A device cannot tell common-mode from its own displacement (that is the whole invisibility
// problem), but it can tell TRANSIENT from STANDING, and that is enough: a group deadline step
// relaxes, while an offset planted by a re-lock does not. Same shape as the repair's
// DRIFT_REPAIR_HOLD_US, and for the same reason -- act on evidence that held still.
static constexpr int64_t FAST_SPLICE_PERSIST_US = 4000000;

// UNMUTE ALSO NEEDS THE ANCHOR TO AGREE, not just the median error.
//
// The median error is a SELF-CONSISTENT test: it compares audio against this device's own
// prediction, so it reads ~0 while the prediction itself is displaced, and every other on-device
// field agrees with it. That is the whole invisibility problem, and the unmute gate was built out
// of exactly that quantity.
//
// What it costs, measured 2026-08-26 21:26: an outage's re-lock unmuted at median 230 us, and FIVE
// SECONDS LATER a repair fired reading +39977 us. The board had been placing real audio against an
// anchor 40 ms wrong for those seconds, and the pair settled 173 us apart -- permanently, with
// every metric healthy. The wire offset between two devices is the DIFFERENCE OF THEIR ANCHOR
// ERRORS, so an anchor that wrong at unmute is a planted offset by construction.
//
// The anchor error is not invisible: it is the accounting split the repair already measures, whose
// own floor is ~7 us on a fully-accounted device, and its samples are collected whether or not the
// device is converged. So gate the unmute on it -- do not start placing audio until the anchor
// agrees with the measured pipeline.
//
// Set to DRIFT_REPAIR_US, i.e. "do not unmute carrying a split the repair would immediately act
// on". Tighter is where a <10 us goal has to go, but not before a repair costs less than the
// +-50 us it currently displaces.
// TIGHTENED from DRIFT_REPAIR_US (2000 us) on the first event that tested it. 00:14 on
// 2026-08-27: a unmuted at anchor 0 us, b at anchor +158 us -- both inside the old threshold --
// and the wire stepped from -21.7 us to -236.3 us. The 158 us DIFFERENCE and the ~215 us step are
// the same quantity: the offset between two devices IS the difference of their anchor errors, and
// the gate was letting through 20x more than the goal.
//
// 100 us against a drift-median floor of ~7 us leaves room to tighten further; the reason not to go
// straight to the floor is that the median is over ~3.3 s of samples, so demanding single-digit us
// at the unmute instant risks waiting out the bound on every event and unmuting late with no
// benefit. Tighten again once an event has been graded at this value.
static constexpr int32_t UNMUTE_ANCHOR_US = 100;
// A bound on that wait. Silence is also a defect: if the anchor never settles -- a mixer reporting
// a depth that does not describe the whole pipeline, say -- unmuting late but audible beats staying
// quiet, and the log line says which happened. Long enough for the ~3.3 s drift window to fill
// several times over.
static constexpr int64_t UNMUTE_ANCHOR_MAX_WAIT_US = 15000000;
// TEST HOOK ramp rate, see inject_split(). 100 us/s is chosen to sit at or under the disturbance
// the servo already tracks unaided -- the clock-offset estimate wanders ~100 us/s on wifi jitter --
// so the injected bias arrives as ordinary drift rather than as a transient the servo fights. At
// this rate the 2 ms minimum that can arm a repair (DRIFT_REPAIR_US) takes ~20 s to build, plus
// DRIFT_REPAIR_HOLD_US to fire, which is the price of not swamping the measurement.
static constexpr double SPLIT_RAMP_US_PER_S = 100.0;

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
    this->playout_epoch_.fetch_add(1, std::memory_order_relaxed);
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
      this->playout_epoch_.fetch_add(1, std::memory_order_relaxed);
      rebaselined = true;
      this->dbg_seed_trace_arm_.store(true, std::memory_order_relaxed);
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
    // A re-baseline must not be re-triggered by its OWN aftermath. The seed sets played to 0 and
    // pushed to the measured in-flight audio, but the sink goes on crediting frames it played from
    // BEFORE the reset, and those are not in the seed. They drain the accounted queue to zero, and
    // `available <= 0` is exactly the condition that arms this latch -- so the recovery re-arms it,
    // a second re-baseline fires, and by then the pipeline has REFILLED, so it anchors to a full
    // one. The latch's own hysteresis does not help: starved_latched_ clears on the first credit
    // that fits, which the early post-seed credits do.
    //
    // Measured on hardware, and this is the whole 8.5 ms bug: seed 3705 frames (84 ms) at
    // 19:21:00.7; 3526 frames discarded across 9 clamp events; second re-baseline at 19:21:02.9
    // anchoring to 253 ms (own=253741, debt=0 -- entirely real audio, nothing drained about it).
    // The accounting was then permanently 8526 us adrift -- `drift=-8526` on every RECON line for
    // the next two minutes -- while the servo reported clean medians and steered real audio to that
    // wrong target. On a logic analyser the pair sat 8.51 ms apart, sd 22 us: a pure static offset.
    //
    // Suppressing for twice the measured latency covers the drain of the pre-reset audio, which is
    // what that latency measures. Masking a genuine second starvation inside the window is the
    // intended trade: a starvation arriving that soon is part of the same event, and re-baselining
    // again is precisely the cascade being prevented.
    const bool starve_suppressed = timestamp_us < this->starve_suppress_until_us_.load(std::memory_order_relaxed);
    if (available_frames <= 0 && frames > 0 && !this->starved_latched_ && !starve_suppressed) {
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

  // ANCHOR ERROR: has the audio that was resident at the last seed finished draining?
  //
  // INTERPOLATED within the crossing report, not taken at its timestamp. Feedback arrives in
  // DMA-sized batches (~50 ms), so using the report instant would quantise the answer to the
  // batch and swamp the tens of microseconds being looked for. The batch is contiguous audio at a
  // known rate, so where inside it the target frame sits is exact arithmetic.
  if (this->seed_drain_target_frames_ > 0 && this->played_frames_total_ >= this->seed_drain_target_frames_ &&
      frames > 0) {
    const int64_t into_batch = this->seed_drain_target_frames_ - this->seed_drain_prev_frames_;
    const int64_t batch_us = timestamp_us - this->played_prev_ts_us_;
    const int64_t crossed_us =
        (into_batch > 0 && into_batch <= static_cast<int64_t>(frames) && batch_us > 0)
            ? this->played_prev_ts_us_ + batch_us * into_batch / static_cast<int64_t>(frames)
            : timestamp_us;
    const int64_t drained_us = crossed_us - this->seed_drain_from_us_;
    ESP_LOGD(TAG,
             "SEEDDRAIN anchored=%" PRId64 " actual=%" PRId64 " err=%" PRId64 " frames=%" PRId64 " t=%" PRId64,
             this->seed_drain_latency_us_, drained_us, drained_us - this->seed_drain_latency_us_,
             this->seed_drain_target_frames_, now_us());
    this->seed_drain_target_frames_ = 0;
  }
  this->seed_drain_prev_frames_ = this->played_frames_total_;
  this->played_prev_ts_us_ = timestamp_us;

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
    // A new session, which the player task cannot infer from its own state: a reconnect and a
    // mid-session excursion both clear st.converged, and only the first rebuilt the pipeline.
    this->session_epoch_.fetch_add(1, std::memory_order_relaxed);
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
  // A boot is a disturbance like any other, and the largest one measured: the re-baseline anchor
  // starts a power cycle ~620 us out. Stamped here rather than left at 0 so the first convergence
  // hands off to a decaying gain instead of dropping straight to RUN.
  this->mark_kp_event_(st, "boot");
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
    if (st.padding_debt_frames > 0 && now_us() >= st.padding_repay_at_us) {
      audio::AudioDepth lat_now, own_now;
      if (this->audio_listener_ != nullptr && this->audio_listener_->on_query_latency(lat_now) &&
          this->audio_listener_->on_query_audio(own_now)) {
        const int64_t pad_now_us =
            lat_now.microseconds > own_now.microseconds ? lat_now.microseconds - own_now.microseconds : 0;
        // REPAY THE WHOLE DEBT, and on a deadline set at the seed rather than when the CURRENT
        // padding reads empty. The old trigger was `pad_now <= PADDING_DRAINED_US`, minus
        // pad_now's own frames, and it was wrong in both directions: the DMA is a rolling window,
        // so pad_now falls to zero as soon as one full buffer of real audio is queued -- while the
        // silence the seed was actually given may still be resident -- and equally it can stay
        // high on NEW padding long after the seeded silence has gone, which then repays a
        // fraction of the debt and leaves the rest planted forever.
        //
        // Measured with an injected starvation: repaying 2205 frames on that trigger left the
        // accounting 60 ms BELOW the chain where not repaying at all would have left it 10 ms
        // below -- the repayment contributed 50 ms of shortfall, because by the time pad_now read
        // zero the real audio behind the seeded silence had already arrived and been credited.
        //
        // The deadline needs no query to be right. The seeded silence sits behind the real audio
        // inside each resident descriptor, so all of it has played once the DAC has worked through
        // the span that was resident at the seed -- `latency` at that instant -- and the DAC plays
        // at real time. pad_now is still read, for the diagnostic only.

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
            std::min(st.padding_debt_frames, this->pushed_frames_total_ - this->played_frames_total_);
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
        st.padding_repay_at_us = 0;
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
    {
      // See dl_off_* in ServoState: isolates the timebase's contribution to the error so it can be
      // told apart from the prediction's.
      const int64_t dl_off = deadline - rec.server_ts_us;
      if (st.dl_off_valid) {
        const int64_t step = dl_off - st.dl_off_prev_us;
        if (!st.dl_step_valid) {
          st.dl_step_valid = true;
          st.dl_step_min_us = st.dl_step_max_us = step;
        } else {
          st.dl_step_min_us = std::min(st.dl_step_min_us, step);
          st.dl_step_max_us = std::max(st.dl_step_max_us, step);
        }
      }
      st.dl_off_valid = true;
      st.dl_off_prev_us = dl_off;
    }
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
      // The mute decision, subject to the room's resilience setting. See SyncResilience: the
      // storm test is the tunable half, because a storm is an established run of audible
      // corrections; the stale test is the half that survives every level except NEVER_MUTE,
      // because past the server's own buffer the DEADLINE is wrong and playing toward it is
      // meaningless rather than merely rough.
      const SyncResilience resilience = this->sync_resilience();
      const bool storm_mutes = resilience == SyncResilience::MUTE_ON_STORM;
      const bool stale_mutes = resilience != SyncResilience::NEVER_MUTE;
      mute_now = (storm_mutes && st.storm_resyncs >= RESYNC_STORM_COUNT) ||
                 (stale_mutes && std::abs(error_us) > stale_us);
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

    // PER-CHUNK RESYNC TRACE. Armed by an excursion, bounded, and rate-limited, so it costs
    // nothing in steady state.
    //
    // It exists because the question this path raises cannot be answered from the throttled
    // lines below. Dropping a chunk buys exactly one chunk of deadline, so a REAL lateness
    // should shrink ~1:1 with the audio discarded, while a bad prediction or a bad deadline
    // reads the same excess on every following chunk. Those two demand opposite responses and
    // produce the same coarse symptom -- an error that grows while discards happen -- which is
    // precisely how a discard cap got written, flashed and reverted. `drops` against `err` on
    // consecutive lines is the discriminator.
    //
    // Placed BEFORE the late-resync branch on purpose: that branch ends in `continue`, so it
    // skips dbg_early_recon_ entirely and a storm currently leaves no chunk-resolution trace at
    // all. ring= is here because the other open question is whether the ring drains because the
    // client is behind or because the correction is draining it.
#ifdef CLOCK_SYNC_TSF_ACTIVE
    // A LEADERSHIP CHANGE swaps the timebase the deadline is computed against, so whatever the
    // servo had converged to is now measured against a different clock. Checked per chunk (an
    // atomic load) rather than at report cadence, because a 3.3 s delay would spend most of the
    // decay before the schedule noticed the event.
    if (this->tsf_sync_ != nullptr) {
      const int8_t role_now = static_cast<int8_t>(this->tsf_sync_->role());
      if (st.kp_last_role >= 0 && role_now != st.kp_last_role) {
        this->mark_kp_event_(st, "role change");
      }
      st.kp_last_role = role_now;
    }
#endif

    // Ring level in ms of audio. Computed every chunk now, because the pre-trigger history
    // below needs it on chunks where nothing is being logged at all.
    const uint32_t ring_ms =
        (frame_bytes > 0 && rec.params.sample_rate > 0)
          ? static_cast<uint32_t>(static_cast<uint64_t>(this->pcm_ring_->available()) * 1000 /
                                  (frame_bytes * rec.params.sample_rate))
          : 0;

    if (st.resync_trace_left == 0 && std::abs(error_us) > hard_us &&
        now_us() - st.resync_trace_arm_us >= RESYNC_TRACE_ARM_INTERVAL_US) {
      st.resync_trace_arm_us = now_us();
      st.resync_trace_left = RESYNC_TRACE_CHUNKS;
      st.resync_trace_idx = 0;
      st.resync_drops = 0;
      // Arm the pre-trigger replay in the same breath. The window ends at the chunk BEFORE this
      // one -- this chunk is RSYNC[0] -- so the two records abut with no overlap and no gap.
      this->arm_pre_trace_dump_(st);
    }
    if (st.resync_trace_left > 0) {
      st.resync_trace_left--;
      ESP_LOGD(TAG, "RSYNC[%u] t=%" PRId64 " err=%" PRId64 " med=%" PRId64 " ring=%" PRIu32 " drops=%" PRIu32,
               st.resync_trace_idx++, now_us(), error_us, median_err_us, ring_ms, st.resync_drops);
    }
    // One packed line of history per chunk, then back to recording. Recording is frozen for the
    // ~14 chunks this takes, which is why it is ordered after the emit and before the record.
    this->emit_pre_trace_line_(st);
    this->record_pre_trace_(st, error_us, median_err_us, ring_ms);
    if (error_us <= hard_us && error_us >= -hard_us) {
      st.resync_drops = 0;  // episode closed
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
      // Re-arm the gain schedule: this is the disturbance the fast gain exists for, and the point
      // where nulling quickly is worth the noise it costs. Marked on EVERY resync of a storm, not
      // just the first -- the decay should run from the last one, not the first.
      this->mark_kp_event_(st, "hard resync (late)");
      st.err_window_filled = 0;
      st.steer_dir = 0;
      // INFO on the true->false edge: this is the moment audio goes silent, and it
      // is the only user-audible event in the loop. Logging only the re-lock (which
      // is INFO) made a dropout look like a spontaneous "Sync locked" with no cause,
      // since the resync line above is DEBUG and throttled. One line per gap.
      if (st.converged && mute_now) {
        // The re-lock that follows re-derives the anchor, so arm the forced repair for it.
        st.reanchor_armed = true;
        ESP_LOGI(TAG, "Muting: hard resync, %" PRId64 " ms late (%" PRIu32
                      " in %" PRId64 " s) -- audible gap until re-lock",
                 error_us / 1000, st.storm_resyncs, RESYNC_STORM_WINDOW_US / 1000000);
      }
      st.converged = st.converged && !mute_now;
      st.resync_drops++;
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
      this->mark_kp_event_(st, "hard resync (early)");
      st.err_window_filled = 0;
      st.steer_dir = 0;
      if (st.converged && mute_now) {
        st.reanchor_armed = true;
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
        // Acquisition and steady state want opposite gains, switched on st.converged:
        // muted, nothing is audible and the error must be nulled fast enough to satisfy the
        // unmute gate; unmuted, the differential is the only thing that matters.
        //
        // Two richer schemes were tried on hardware and both are recorded as failures below
        // and at TRIM_KP_ACQUIRE_PPM_PER_US: scheduling the gain on |median error| with
        // hysteresis (limit-cycled), and a one-way latch on sustained smallness (did not
        // cycle, but was history-dependent, did not address the starvation-recovery case it
        // was justified by, and existed only to correct the error let through by a widened
        // unmute gate that should not have been widened). The schedule that replaced the step
        // keeps that rule: it reads a TIMER SINCE A DISCRETE EVENT, never the error.
        const float kp = this->trim_kp_(st);
        st.kp_active = kp;
        // Bumpless transfer. The gains differ 5x, so switching would step the output by
        // (kp_hi - kp_lo) * error. At the schedule threshold that is 0.4 * 300 = 120 ppm --
        // a real rate step on one board, applied while unmuted, which is precisely the
        // defect this whole line of work exists to remove. Move the difference into the
        // integrator instead so the COMMANDED TRIM IS CONTINUOUS across the switch: only
        // its future responsiveness changes, not its present output.
        //
        // trim_kp_last starts at 0 meaning "never conditioned", so the first pass only
        // records the gain -- transferring against a startup error would inject a large
        // integral term for a switch that never happened.
        if (st.trim_kp_last != kp) {
          if (st.trim_kp_last != 0.0f) {
            st.trim_integral_ppm =
                std::clamp(st.trim_integral_ppm + (st.trim_kp_last - kp) * static_cast<float>(median_err_us),
                           -clamp_ppm, clamp_ppm);
          }
          // Recorded unconditionally, including on the first pass -- guarding the whole
          // block on the sentinel would leave it at 0 forever and no switch would ever
          // transfer.
          st.trim_kp_last = kp;
        }
        // SPLIT PENDING: hold the trim instead of steering on a prediction we already suspect.
        //
        // drift_excess_since_us is non-zero exactly while a sustained accounted-vs-measured split
        // is being timed toward DRIFT_REPAIR_HOLD_US. Through that window the median error is
        // measured against a prediction the code is ABOUT TO DECLARE WRONG, and steering on it
        // moves real audio that nothing will ever move back -- the servo has no position feedback,
        // so a displacement here is permanent.
        //
        // Measured before this: each repair planted a step of +329.6, +311.1, -347.4 us for a
        // +-2500 us split (two clean positives agreeing to 18 us) and +491 us for a natural
        // +20000 us one. The size tracks the TRIM applied during the hold, saturated by the clamp
        // -- 0.18 and 0.16 of trim x hold respectively -- which is what identifies the trim, rather
        // than the split, as the thing doing the damage. Roughly 23 repairs fired in one session,
        // so this is a larger contributor to inter-device skew than anything else measured.
        //
        // Holding rather than zeroing: the trim's INTEGRAL is the converged crystal-offset
        // cancellation, so the clock keeps running at the right rate. Zeroing would itself be a
        // rate step of tens of ppm. The 3 s of not steering costs little -- the loop exists to
        // track disturbances that move over minutes -- and the hold is what buys the confirmation
        // that stops a spike triggering a spurious repair, so it is kept.
        //
        // HOLD THE INTEGRAL, NOT THE WHOLE TRIM. Freezing p_term + integral preserved the SUSPECT
        // term at full size for the whole 3 s: p_term is the servo's response to an error the code
        // is about to declare wrong, which is the exact thing this branch exists not to act on,
        // while the integral is the part worth preserving. Measured on a clean +2500 us injection
        // under the previous behaviour: trim +199 ppm at the hold, wire step -101.5 us, against
        // the model's 199 x 3 x 0.17 = 101 -- so essentially ALL of that displacement was p_term,
        // a converged integral running ~40-50 ppm. Same model with the integral alone predicts
        // ~25 us.
        //
        // Note this is not rate-LIMITING the output, which is measured and reverted (see the
        // 2 ppm/s note near the clamp): the trim steps to the integral at once and steps back on
        // release. A rate step is not a displacement -- displacement is the integral of rate over
        // time, and this branch is about not accumulating any.
        const bool split_pending = st.drift_excess_since_us != 0;
        if (split_pending) {
          const float held_ppm = std::clamp(st.trim_integral_ppm, -clamp_ppm, clamp_ppm);
          if (!st.trim_split_held) {
            st.trim_split_held = true;
            ESP_LOGD(TAG, "Trim held: accounting split pending confirmation, not steering on a "
                          "suspect prediction (trim %+.2f -> integral %+.2f ppm) t=%" PRId64,
                     st.trim_applied_ppm, held_ppm, now_us());
          }
          trim_holds = this->rate_lock_->set_trim_ppm(held_ppm);
          if (trim_holds) {
            // What is PROGRAMMED, so the report's trim integral and the nominal-hold path below
            // both start from the value the hardware actually has.
            st.trim_applied_ppm = held_ppm;
          } else {
            st.rate_lock_ok = false;
            ESP_LOGW(TAG, "Rate lock unavailable, falling back to frame-splice corrections");
          }
        } else {
          if (st.trim_split_held) {
            st.trim_split_held = false;
            ESP_LOGD(TAG, "Trim released: split resolved t=%" PRId64, now_us());
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
        }  // end !split_pending
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
      } else {
        // The rate lock is steering, so the block above is off. A standing offset therefore has
        // only the rate loop to remove it, at ~40 s -- see FAST_SPLICE_RELEASE_US. Position
        // correction runs here instead, well above the band, one frame at a time.
        // Re-read rather than reusing the PI block's local: that one is scoped to the rate-lock
        // branch, and this must be correct whether or not the PI ran this chunk. Same expression.
        const int32_t fast =
            this->fast_splice_(st, median_err_us, rec.params.sample_rate, st.drift_excess_since_us != 0);
        if (fast > 0) {
          drop_frames = static_cast<uint32_t>(fast);
          st.soft_dropped_frames += static_cast<uint32_t>(fast);
        } else if (fast < 0) {
          st.soft_inserted_frames += static_cast<uint32_t>(-fast);
          this->push_repeat_frame_(rec.params);
        }
      }
    }

#ifdef USE_I2S_RATE_LOCK
    // Integrate the REALISED trim over this chunk's audio time. Placed after the whole servo
    // chain, deliberately: the hard-resync and aggressive-catch-up branches never enter the
    // PI, yet audio keeps being clocked out under whatever trim was last programmed, so
    // accumulating inside the PI block would drop that time out of the integral without
    // saying so.
    //
    // applied_ppm(), not st.trim_applied_ppm: the first is what the divider actually
    // achieved after rational quantisation, the second is what the PI asked for. An integral
    // of achieved RATE has to use the achieved value -- the clock does not run at the demand.
    // Measured on hardware they sit sd 0.5-0.65 ppm apart with a 4 ppm peak-to-peak, so this
    // is not the 0.15 ppm quantisation step alone, though it is common-mode enough that the
    // DIFFERENTIAL between two boards barely moves (sd 5.89 realised against 5.74 demanded).
    //
    // Attribution is off by at most one chunk (~26 ms in a ~3.3 s window): the trim
    // programmed now governs audio that drains from now on. Second-order against the
    // aliasing this exists to remove.
    {
      const float chunk_s = static_cast<float>(frames) / static_cast<float>(rec.params.sample_rate);
      st.trim_window_s += chunk_s;
      if (st.rate_lock_ok) {
        st.trim_integral_ppm_s +=
            static_cast<double>(this->rate_lock_->applied_ppm()) * static_cast<double>(chunk_s);
        st.trim_covered_s += chunk_s;
      }
      // Emitted on its OWN TIME THROTTLE, deliberately not on the 128-chunk report boundary.
      // Two reasons. The report interval also gates the accounting-split REPAIR, so shortening
      // it to get finer diagnostics would make a self-repair fire four times as often -- a
      // control change wearing a diagnostics hat. And the project rule is to throttle
      // diagnostics by time, never by iteration count, because a loop with no guaranteed
      // cadence floods the log.
      //
      // ~1 s against the report's 3.35 s. The window MEAN is what defeats the aliasing that
      // made the end-of-window snapshot useless, and it stays a true time-mean at any window
      // length, so this is 3.3x the resolution at no cost to the statistic. Going to per-chunk
      // (38 lines/s/device) is the documented flood that stalls an OTA; the armed burst trace
      // covers events instead.
      //
      // t= is esp_timer microseconds since boot, the SAME clock clock_sync stamps its lines
      // with, so the two components' series share one axis.
      //
      // What it is worth, measured once the stamp made the measurement possible: the host
      // "[HH:MM:SS]" prefix is receive time, and against the device clock it reads p50 0 ms,
      // p90 7.7 ms, p99 28 ms -- under 3% of a 1 s interval. An earlier claim of 200 ms typical
      // and up to 1 s was WRONG, drawn from one truncated and interleaved line. Keep the field
      // anyway: it is free, it cannot degrade under the log congestion a receive timestamp is
      // exposed to, and it is what turns "the host clock is probably fine" into a number.
      if (now_us() - st.trim_log_us >= TRIM_WINDOW_LOG_INTERVAL_US && st.trim_window_s > 0.0f) {
        st.trim_log_us = now_us();
        const float covered_pct = 100.0f * st.trim_covered_s / st.trim_window_s;
        if (st.trim_covered_s > 0.0f) {
          // kp is on this line because the gain is now a CONTINUOUS quantity, and an invisible
          // one reads as "the schedule did nothing" exactly the way the trim snapshot once read
          // "(idle)" for a loop steering by tens of ppm. Appended at the END so the existing
          // parser keeps matching. Not the mean over the window -- the value last APPLIED, which
          // is what the trim beside it was produced with.
          ESP_LOGD(TAG, "Trim window: mean %+.3f ppm over %.2f s audio (covered %.0f%%) t=%" PRId64 " kp=%.3f",
                   static_cast<float>(st.trim_integral_ppm_s / static_cast<double>(st.trim_covered_s)),
                   st.trim_window_s, covered_pct, now_us(),
                   // 0 means the PI has not run since boot -- the split-hold path programs a trim
                   // without going through the gain. Printing that 0 reads as "the loop is running
                   // at zero gain", which is the same class of lie as the "(idle)" trim snapshot.
                   // Fall back to what the schedule WOULD hand it on this chunk.
                   st.kp_active > 0.0f ? st.kp_active : this->trim_kp_(st));
        } else {
          ESP_LOGD(TAG, "Trim window: no trim programmed over %.2f s audio t=%" PRId64, st.trim_window_s,
                   now_us());
        }
        st.trim_integral_ppm_s = 0.0;
        st.trim_covered_s = 0.0f;
        st.trim_window_s = 0.0f;
      }
    }
#endif

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
        // The anchor has to agree too, or the audio about to be unmuted is placed against a
        // prediction that is already wrong -- see UNMUTE_ANCHOR_US. INT32_MIN means the drift
        // window has no samples yet, which is not evidence of agreement.
        const bool anchor_known = st.drift_med_last_us != INT32_MIN;
        const bool anchor_ok = anchor_known && std::abs(st.drift_med_last_us) <= UNMUTE_ANCHOR_US;
        if (!anchor_ok && st.unmute_anchor_wait_us == 0) {
          st.unmute_anchor_wait_us = now_us();
          ESP_LOGD(TAG, "Unmute held: median %" PRId64 " us is in band but the anchor reads %" PRId32
                        " us; waiting for the accounting to agree t=%" PRId64,
                   median_err_us, st.drift_med_last_us, now_us());
        }
        const bool waited_out =
            !anchor_ok && st.unmute_anchor_wait_us != 0 &&
            now_us() - st.unmute_anchor_wait_us >= UNMUTE_ANCHOR_MAX_WAIT_US;
        if (anchor_ok || waited_out) {
          st.converged = true;
          // Recovery is over, so the leader-side hold is released with it.
          st.deadline_implausible = false;
          st.unmute_anchor_wait_us = 0;
          if (waited_out) {
            ESP_LOGW(TAG, "Sync locked (median %" PRId64 " us) but the anchor still reads %" PRId32
                          " us after %" PRId64 " s -- unmuting anyway; expect a planted offset of "
                          "about the difference against the other devices",
                     median_err_us, st.drift_med_last_us, UNMUTE_ANCHOR_MAX_WAIT_US / 1000000);
          } else {
            ESP_LOGI(TAG, "Sync locked (median %" PRId64 " us, anchor %" PRId32 " us), unmuting", median_err_us,
                     st.drift_med_last_us);
          }
        }
      }
    } else {
      st.in_band_chunks = 0;
      st.unmute_anchor_wait_us = 0;
    }

    if (st.rpush_samples > 0 && now_us() - st.rpush_log_us >= RPUSH_LOG_INTERVAL_US) {
      st.rpush_log_us = now_us();
      ESP_LOGD(TAG, "RPUSH n=%" PRIu32 " bad=%" PRIu32 " (%.1f%%) t=%" PRId64, st.rpush_samples, st.rpush_bad,
               100.0f * static_cast<float>(st.rpush_bad) / static_cast<float>(st.rpush_samples), now_us());
      st.rpush_samples = 0;
      st.rpush_bad = 0;
    }

    this->accumulate_achieved_rate_(st, rec);
    this->reanchor_after_relock_(st);

    this->push_chunk_(rec, drop_frames, !st.converged);

    // TEMPORARY DIAGNOSTIC: see dbg_early_recon_ -- this is the post-startup half.
    // Every term comes from ONE snapshot and the accounting is evaluated at that same instant, so
    // this is the RECON line at chunk resolution rather than once per 128 chunks. Bounded to the
    // first few seconds and sampled every 4th chunk, which is ~10 lines/s for ~4 s.
    this->dbg_early_recon_(rec, "run");

    // Accumulate the accounted queue per chunk so the group cross-check can be handed a MEAN rather
    // than a single sample of a sawtooth. One extra lock per chunk (~38/s) buys an order of
    // magnitude on that comparison's noise floor.
    // TEST HOOK, see inject_split(): bias the accounting by a known amount, audio untouched, RAMPED
    // IN so the servo tracks it as ordinary drift instead of reacting to a step.
    //
    // Stepping it was tried and measured its own disturbance rather than the repair's: an injected
    // +10 ms produced a -3.9 ms excursion and left the wire's fit floor at 822-1204 us, against the
    // few hundred us being looked for. Ramping at a rate the servo already absorbs (the clock-offset
    // estimate wanders ~100 us/s on wifi jitter unaided) reproduces what a natural accounting drift
    // does: the audio arrives at the biased position smoothly, and the REPAIR is the only step on
    // the wire -- which is the thing being measured.
    const int32_t split_req = this->inject_split_us_.exchange(0, std::memory_order_relaxed);
    if (split_req != 0) {
      // Accumulate rather than replace: a second request while one is in flight should add.
      this->split_ramp_remaining_us_ += split_req;
      ESP_LOGW(TAG, "SPLITINJECT request %+" PRId32 " us, ramping at %d us/s (remaining %+" PRId64 ") t=%" PRId64,
               split_req, static_cast<int>(SPLIT_RAMP_US_PER_S), this->split_ramp_remaining_us_, now_us());
    }
    this->playout_mutex_.lock();
    if (this->split_ramp_remaining_us_ != 0) {
      const double chunk_s = static_cast<double>(frames) / rec.params.sample_rate;
      const int64_t budget = static_cast<int64_t>(SPLIT_RAMP_US_PER_S * chunk_s + 0.5);
      const int64_t inc = std::clamp<int64_t>(this->split_ramp_remaining_us_, -budget, budget);
      // ACCUMULATE in us and spend whole frames, because the accounting has no finer unit than a
      // frame. Converting each chunk's budget straight to frames truncated to zero and the ramp
      // silently never moved: at 100 us/s a 26 ms chunk earns ~3 us, while one frame is 22.7 us, so
      // `inc * rate / 1e6` was 0 every time and `remaining` just accumulated. The bug was visible
      // only because the request line prints the running remainder -- it reached +5000 with drift
      // still reading -1.
      //
      // With a carry, any rate below one frame per chunk (868 us/s at 44.1 kHz) becomes reachable:
      // frames are applied every few chunks instead of every chunk, which is what a rate gentler
      // than the frame grid has to look like.
      this->split_ramp_carry_us_ += inc;
      const int64_t whole = this->split_ramp_carry_us_ * rec.params.sample_rate / 1000000;
      if (whole != 0) {
        const int64_t shift = whole;
        // Keep the unspent remainder, so the rate stays right on average rather than losing the
        // fraction every chunk.
        this->split_ramp_carry_us_ -= shift * 1000000 / rec.params.sample_rate;
        this->pushed_frames_total_ += shift;
        this->split_ramp_remaining_us_ -= inc;
        // Histories describe levels against the old counter, so they are re-marked rather than
        // cleared: cleared every chunk would leave the split-vs-measured comparison no history at
        // all, which is the very quantity this experiment needs intact.
        this->mark_playout_(this->pushed_history_, this->pushed_history_next_, now_us(),
                            this->pushed_frames_total_);
        if (this->split_ramp_remaining_us_ == 0) {
          ESP_LOGW(TAG, "SPLITINJECT ramp complete t=%" PRId64, now_us());
        }
      } else {
        // No whole frame yet: the budget is consumed into the carry, not dropped, so a sub-frame
        // rate still advances -- just over several chunks.
        this->split_ramp_remaining_us_ -= inc;
      }
    }
    st.depth_accum_frames += this->pushed_frames_total_ - this->played_frames_total_;
    this->playout_mutex_.unlock();
    st.depth_samples++;

    // Sample the ACCOUNTING SPLIT across the window, not once at the end of it.
    //
    // The report's fill_drift_us is a single snapshot pair per ~3.3 s, and the quantity
    // sawtooths as the mixer's source ring fills and drains, so that one sample lands at an
    // arbitrary phase of the wave. Publishing an instant of a sawtooth is the same mistake the
    // group depth made -- two devices sampled out of phase differed by up to +-50 ms of pure
    // artefact -- and it is why the repair needs a 10 s hold and a 20 ms threshold to see past
    // it. Characterise the wave first: min, max and mean per window say how big it really is,
    // whether the mean is steady while the instant swings, and therefore whether the mean is a
    // fit input for a faster, tighter repair.
    //
    // Every 4th chunk, ~10/s, so ~32 samples per report: enough to resolve a wave whose period
    // is seconds, cheap enough not to matter (a seqlock read and a mutex, against the ~38/s the
    // chunk loop already runs at).
    //
    // Differenced AT THE READING'S OWN INSTANT via accounted_at_(), for the same reason the
    // report's own value is: the reading is a snapshot on the sink's cadence while the accounted
    // queue moves on ours, and differencing them as-read measures the phase between those
    // cadences and almost nothing else.
    if (--st.drift_sample_countdown == 0) {
      st.drift_sample_countdown = DRIFT_SAMPLE_EVERY_CHUNKS;
      audio::AudioDepth d_meas;
      int64_t d_acct_frames = 0;
      if (this->audio_listener_ != nullptr && frame_bytes > 0 &&
          this->audio_listener_->on_query_audio(d_meas)) {
        this->playout_mutex_.lock();
        const bool ok = this->accounted_at_(d_meas.as_of_us, d_acct_frames);
        this->playout_mutex_.unlock();
        if (ok) {
          const int64_t acct_us = d_acct_frames * 1000000 / static_cast<int64_t>(rec.params.sample_rate);
          const int32_t d = static_cast<int32_t>(acct_us - static_cast<int64_t>(d_meas.microseconds));
          if (st.drift_samples == 0) {
            st.drift_min_us = st.drift_max_us = d;
          } else {
            st.drift_min_us = std::min(st.drift_min_us, d);
            st.drift_max_us = std::max(st.drift_max_us, d);
          }
          // r_push validity, on the same samples. In range means the two counters share an
          // origin: pushed leads src_received by what is genuinely in flight, which is bounded by
          // the slice buffer and a chunk or two. Anything outside is a counter pair that was
          // re-zeroed independently, and no arithmetic on it means anything.
          const int64_t r_push = static_cast<int64_t>(this->pushed_frames_total_) -
                                 static_cast<int64_t>(d_meas.dbg_src_received);
          st.rpush_samples++;
          if (r_push < -RPUSH_VALID_FRAMES || r_push > RPUSH_VALID_FRAMES) {
            st.rpush_bad++;
          }
          st.drift_window_us[st.drift_window_idx] = d;
          st.drift_window_idx = (st.drift_window_idx + 1) % ServoState::DRIFT_WINDOW;
          st.drift_accum_us += d;
          st.drift_samples++;
        }
      }
    }
  }
  vTaskDelete(nullptr);
}

// TEMPORARY DIAGNOSTIC: one-instant reconciliation of the accounting against the chain, from the
// very first chunk. Every term is from ONE snapshot and the accounting is evaluated at that
// snapshot's instant, so nothing here is aligned by guesswork. Bounded and sampled so it cannot
// flood. Remove once the startup offset is explained.
void SnapcastClient::dbg_early_recon_(const ChunkRecord &rec, const char *phase) {
  // A re-baseline arms a full-cadence burst, which takes precedence over the startup sampling: the
  // seed's whole story happens inside ~150 ms, so every other chunk is not enough resolution and the
  // 240-chunk startup budget may long since have been spent.
  if (this->dbg_seed_trace_arm_.exchange(false, std::memory_order_relaxed)) {
    this->dbg_seed_trace_left_ = 80;  // ~2.1 s at the 26 ms chunk cadence
    this->dbg_seed_trace_idx_ = 0;
  }
  uint32_t n;
  if (this->dbg_seed_trace_left_ > 0) {
    this->dbg_seed_trace_left_--;
    n = this->dbg_seed_trace_idx_++;
    phase = "seed";
  } else {
    if (this->dbg_early_chunks_ >= 240) {
      return;
    }
    n = this->dbg_early_chunks_++;
    if ((n % 2) != 0) {
      return;
    }
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
  //   r_mix  : the mixer -- taken from the source ring, minus handed to the sink, minus its transfer
  //            buffer and what is in flight to the sink. Omitting that last stage is what made this
  //            residual equal the stage itself, and downstream that read as an accounting split.
  const int64_t own_frames = static_cast<int64_t>(m.dbg_own_us) * rate / 1000000;
  const int64_t xfer_frames = static_cast<int64_t>(m.dbg_xfer_us) * rate / 1000000;
  const int64_t r_push = p_now - static_cast<int64_t>(m.dbg_src_received);
  const int64_t r_src = static_cast<int64_t>(m.dbg_src_received) - static_cast<int64_t>(m.dbg_src_consumed) -
                        own_frames;
  const int64_t inflight_frames = static_cast<int64_t>(m.dbg_inflight_us) * rate / 1000000;
  const int64_t r_mix = static_cast<int64_t>(m.dbg_src_consumed) - static_cast<int64_t>(m.dbg_sink_received) -
                        xfer_frames - inflight_frames;
  ESP_LOGD(TAG,
           "EARLY[%" PRIu32 "] %s ok=%d acct=%" PRId64 " live=%" PRId64 " meas=%" PRIu32 " own=%" PRIu32
           " xfer=%" PRIu32 " inflight=%" PRIu32 " queued=%" PRIu32 " dma=%" PRIu32 " pushed=%" PRId64
           " played=%" PRId64 " clamp=%" PRId64 " pad=%" PRIu32 " | srcrx=%" PRIu32 " srctx=%" PRIu32
           " sinkrx=%" PRIu32 " r_push=%" PRId64 " r_src=%" PRId64 " r_mix=%" PRId64 " age=%" PRId64,
           n, phase, ok ? 1 : 0, ok ? acct_frames * 1000000 / rate : -1, (p_now - pl_now) * 1000000 / rate,
           m.microseconds, m.dbg_own_us, m.dbg_xfer_us, m.dbg_inflight_us, m.dbg_queued_us, m.dbg_dma_us, p_now,
           pl_now, this->dbg_clamped_frames_, m.dbg_padded_frames, m.dbg_src_received, m.dbg_src_consumed,
           m.dbg_sink_received, r_push, r_src, r_mix, now_us() - m.as_of_us);
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
      // DISPROVEN: ageing the snapshot before anchoring. Kept as a warning, because the reasoning
      // looks compelling and is wrong.
      //
      // The snapshot is 15-39 ms old here and the offsets this seed plants are 3.7-13 ms, so ageing
      // it looks like the obvious correction. Two attempts, both measured with an injected starvation:
      //
      //   ageing the whole latency        -> hard resyncs at 350, 2297, 3581 ms, then a reconnect
      //   ageing all but dbg_dma_us       -> that field is real-audio-in-DMA, not the span, so the
      //                                      split was inverted: pair 67 ms apart (was 10)
      //   ageing all but the PUBLISHED span (AudioDepth::render_nondraining_us, added for this)
      //                                   -> pair 73 ms apart, drift -68549 then -88549 and growing
      //
      // The third attempt used exactly the right term -- the seed line showed dma=25034 against
      // held=50000, so the two are genuinely distinct and the value propagated correctly -- and it
      // was still worse. That rules out the implementation and leaves the premise:
      //
      //   SNAPSHOT AGE DOES NOT IMPLY DRAINAGE.
      //
      // Ageing assumes the queue drained by the elapsed time. It did not: upstream keeps pushing into
      // it, so over 37 ms of staleness the net change is near zero. Measured on the seed that did the
      // damage: own=234399, i.e. 234 ms of real audio present and being replenished, from which 36.8
      // ms was subtracted for nothing. The premise only holds during a true drain with no refill --
      // and there the queue term is already zero, so ageing correctly does nothing (observed:
      // aged_off=0, anchor unchanged, harmless).
      //
      // So the anchor error is NOT staleness. Whatever plants the 3.7-13 ms is still unexplained, and
      // the next attempt needs a different hypothesis rather than a better subtraction. held= is still
      // logged below: it is the instrument that settled this, and it is worth keeping visible.
      // DISPROVEN: refusing a seed onto a pipeline that is not drained.
      //
      // A starvation produces TWO seeds ~1.4 s apart: the first while the pipeline is genuinely empty
      // (own=0), the second once it has refilled (own=234399..244400). The second looks indefensible
      // -- a pipeline holding 244 ms of our own audio plainly did not drain -- and refusing it looked
      // like the clean semantic fix for the multi-millisecond offsets this path plants.
      //
      // Measured with an injected starvation, the guard fired exactly as designed and made things far
      // worse: pair 198 ms apart, drift -194060, against 10 ms before.
      //
      // The second seed is COMPENSATING, not damaging. The first anchors to a truth that expires: at
      // that instant the pipeline really is empty, but 1.4 s later it holds ~244 ms, and the accounting
      // pinned to the drained-state anchor is then short by exactly that. The second seed re-anchors to
      // the refilled pipeline and brings it back. The 8.5-13 ms offset is the RESIDUAL of that
      // correction, not the correction itself -- so removing the correction leaves the whole error.
      //
      // Any future attempt has to explain that residual, or anchor at an instant that stays true,
      // rather than suppressing the second seed. Three other hypotheses are also dead: divider
      // quantisation (want tracks applied to 0.24 ppm), the cascade alone (fixed, offsets persisted),
      // and snapshot staleness (see the note above).
      const int64_t in_flight_frames =
          have_fill ? static_cast<int64_t>(latency.microseconds) * rec.params.sample_rate / 1000000 : 0;
      // The padding is the difference between the two queries, by definition: latency counts the
      // DMA's silence, own-audio does not. Record it so it can be repaid, not left for the repair.
      st.padding_debt_frames =
          (have_fill && have_own && latency.microseconds > own_audio.microseconds)
              ? static_cast<int64_t>(latency.microseconds - own_audio.microseconds) * rec.params.sample_rate / 1000000
              : 0;
      // When the debt comes off, decided here rather than by a later query. See the repayment site.
      st.padding_repay_at_us =
          st.padding_debt_frames > 0 ? now_us() + static_cast<int64_t>(latency.microseconds) : 0;
      // Bar this seed's own aftermath from re-arming the latch (see notify_audio_played). The
      // window is twice the measured latency, which is how long the pre-reset audio downstream can
      // still be generating credits we did not seed; floored so a near-empty measurement -- the
      // usual case here, since the pipeline just drained -- still leaves room for the sink's own
      // in-flight buffer to clear.
      const int64_t suppress_us =
          std::max<int64_t>(2 * static_cast<int64_t>(have_fill ? latency.microseconds : 0), 1000000);
      this->starve_suppress_until_us_.store(now_us() + suppress_us, std::memory_order_relaxed);
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
      // Arm the anchor-error measurement HERE, under the mutex. notify_audio_played reads these
      // from the speaker callback thread while holding it, so setting them after the unlock below
      // would be a plain data race on the very fields the measurement depends on.
      if (have_fill) {
        this->seed_drain_target_frames_ = in_flight_frames;
        this->seed_drain_from_us_ = now_us();
        this->seed_drain_latency_us_ = latency.microseconds;
        this->seed_drain_prev_frames_ = this->played_frames_total_;
        // Reset the interpolation base too. Without this it still holds a PRE-SEED feedback
        // timestamp, so if the target is crossed on the first batch after the seed the batch
        // interval spans the whole starvation and the interpolation is nonsense. That is not a
        // corner case: a dry pipeline anchors at one DMA buffer, and one buffer IS one batch --
        // exactly the case this measurement was armed for.
        this->played_prev_ts_us_ = now_us();
      }
      this->playout_mutex_.unlock();
      if (have_fill) {
        // The snapshot's age is logged but NOT applied -- see above. It is here because it is the
        // number that would have to be wrong for this anchor to be wrong.
        ESP_LOGD(TAG, "SEEDDBG latency=%" PRIu32 " own=%" PRIu32 " dma=%" PRIu32 " held=%" PRIu32
                      " debt=%" PRId64 " seed=%" PRId64 " played_was=%" PRId64,
                 latency.microseconds, have_own ? own_audio.microseconds : 0, latency.dbg_dma_us,
                 latency.render_nondraining_us, st.padding_debt_frames, in_flight_frames,
                 this->played_frames_total_);
        ESP_LOGD(TAG, "Re-baseline anchored to measured latency: %" PRIu32 " ms (%" PRId64
                      " frames), snapshot %" PRId64 " ms old",
                 latency.microseconds / 1000, in_flight_frames, (now_us() - latency.as_of_us) / 1000);
        // SEED ANCHOR, on its own line and stamped, so the wire can be asked what this anchor
        // actually did. The hypothesis under test: a planted static offset is the per-device ERROR
        // in this `latency`, which is unobservable here by construction -- the servo then measures
        // against the prediction this anchors, so it reads ~0 while the audio sits that far off,
        // and only an external instrument can see it.
        //
        // What makes it testable is that the error should appear on the wire as a STEP at this
        // instant, whose size varies between events and correlates with the anchored value. So the
        // fields are the ones needed to pair a seed against a wire step: the anchored latency, the
        // snapshot age (the term that would have to be wrong for the anchor to be wrong), and t=.
        // age is included because a stale snapshot is the most likely source of the error and it
        // is deliberately NOT compensated for -- see the note above.
        //
        // Compare across boards only for a SIMULTANEOUS event. Recorded landing values say
        // simultaneous restarts land within +-10 us while a lone restart lands anywhere in
        // +-130 us, which is exactly what correlated versus uncorrelated pipeline state predicts,
        // and is the reason this anchor is the suspect.
        ESP_LOGD(TAG, "SEEDANCHOR latency=%" PRIu32 " age=%" PRId64 " frames=%" PRId64 " t=%" PRId64,
                 latency.microseconds, (now_us() - latency.as_of_us), in_flight_frames, now_us());
        // The measurement itself is armed above, under the mutex. Armed even when
        // in_flight_frames is small: a fully dry pipeline anchors at one DMA buffer, and that case
        // matters most, because the wire cannot measure it at all -- with the audio stopped the
        // analyser loses PCM lock and reads NaN either side of the seed.
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
      // A seed re-derives the anchor from this device's own measured latency, which is the error
      // the forced cycle exists to spend a repair on.
      st.reanchor_armed = true;
  #ifdef USE_I2S_RATE_LOCK
      if (this->rate_lock_ != nullptr) {
        this->rate_lock_->invalidate_baseline();
      }
  #endif
      // The seed moves the accounting under the servo, which is exactly the "measured wrong, then
      // corrected" shape the fast gain is for.
      this->mark_kp_event_(st, "re-baseline");
      ESP_LOGI(TAG, "Pipeline drained (source starvation); re-baselining playout");
      this->dbg_seed_trace_arm_.store(true, std::memory_order_relaxed);
    }
}

// ACHIEVED RATE AGAINST SERVER TIME. THREAD CONTEXT: player task.
//
// The one measurement this whole line of work is missing: a rate reference taken from the PLANT
// rather than from the controller's own output. Every failed attempt at a rate reference derived it
// from the servo -- and the trim carries each board's own crystal error, an unknown constant that
// integrates forever (~540 us per 100 s against a ~7 us floor).
//
// The source here is the playout FEEDBACK (frames the speaker says it has rendered, and when),
// fitted against SERVER time via the shared mapping. Server time is what makes two devices'
// values comparable: they share it, so differencing their rates gives the term that integrates
// into wire offset, with no servo state anywhere in the path.
//
// Least squares over the whole window, incrementally. Never a two-endpoint baseline: the
// credit-adjacent timestamps carry ~300 us of jitter, so a 30 s baseline resolves ~+-10 ppm where
// the spec is 0.04 ppm -- 250x too coarse. The spec comes from the organising fact rather than
// taste: the offset is the integral of the rate, so a constant error of e ppm costs e us per
// second, and 0.04 ppm is what keeps a 300 s run inside a ~13 us floor.
//
// Diagnostics only, and deliberately so for now: it is logged to be SCORED against the analyser's
// own rate columns before anything is allowed to steer on it or before it goes in the beacon.
// WINDOW LENGTH, MEASURED RATHER THAN CHOSEN. 300 s of raw feedback pairs off board b, refitted
// offline at several window lengths:
//
//     whole capture (312 s)   +49.27 ppm    sd 24.7 frames
//      30 s windows           sd 41.98 ppm  range  -47.3 .. +109.7
//      60 s windows           sd  4.35 ppm  range  +51.0 ..  +62.6
//     120 s windows           sd  2.43 ppm  range  +43.5 ..  +48.4
//
// The whole-capture value IS that board's programmed trim (+49..51 ppm on its Trim window lines),
// so the fit recovers the right answer; the +-100 ppm swings the first version logged were the
// 30 s window beating against a disturbance of period tens of seconds -- the depth wave this file
// already documents -- together with the feedback's quantisation (dframes are multiples of 441,
// one DMA buffer; dt of ~10 ms).
//
// 120 s is where it is stable enough to mean something. Note what that costs and what it does not
// buy: 2.43 ppm is still ~60x the 0.04 ppm the offset integral needs, so this is a usable rate
// signal and NOT yet an offset reference. Do not publish it as one on the strength of this.
static constexpr int64_t RATE_WINDOW_US = 120000000;
static constexpr double RATE_MIN_SAMPLES = 200.0;
// Reject a window whose residual says it fitted a discontinuity rather than a rate. A clean window
// sits at ~25 frames; the window that straddled a stream resume read 424. Publishing that as a rate
// is the "reads as data and is not" failure this file keeps warning about.
// TIGHTENED from 60 on the first night's windows, which showed the residual is an almost perfect
// predictor of whether a window can be believed:
//
//     sd 3.57 .. 4.68 frames  ->  srv_ppm  -0.27 .. +0.37   (physics: a locked device renders
//     sd 24.2 .. 28.8 frames  ->  srv_ppm  -8.24 .. -4.70    exactly nominal in SERVER time)
//
// A window at sd ~25 is not a noisier estimate of the same quantity, it is 5-8 ppm wrong -- two
// orders past the 0.04 ppm this reference has to hit. 8 frames sits well above the clean cluster
// and an order below the dirty one.
static constexpr double RATE_MAX_SD_FRAMES = 8.0;


void SnapcastClient::accumulate_achieved_rate_(ServoState &st, const ChunkRecord &rec) {
  if (rec.params.sample_rate == 0) {
    return;
  }
  // A seed or a session restart steps the counters, and a fit across that discontinuity is
  // milliseconds of nonsense the moment anything scales it. Start again rather than straddle it.
  const uint32_t epoch = this->playout_epoch_.load(std::memory_order_relaxed);
  if (epoch != st.rate_epoch) {
    st.rate_epoch = epoch;
    st.rate_server.reset();
    st.rate_local.reset();
    st.rate_last_fb_ts = 0;
    st.rate_window_start_us = 0;
  }

  this->playout_mutex_.lock();
  const int64_t played = this->played_frames_total_;
  const int64_t played_ts = this->played_last_ts_us_;
  const bool valid = this->playout_valid_;
  this->playout_mutex_.unlock();
  if (!valid || played_ts == 0 || played_ts == st.rate_last_fb_ts) {
    return;  // no feedback yet, or no NEW feedback since the last chunk
  }
  st.rate_last_fb_ts = played_ts;

  // Local -> server, through the same shared mapping the deadline uses, so this measures against
  // the timebase the group actually shares rather than against our own clock.
  int64_t server_ts = 0;
#ifdef CLOCK_SYNC_TSF_ACTIVE
  int64_t shared_offset_us = 0;
  if (this->tsf_sync_ == nullptr || !this->tsf_sync_->shared_server_offset_us(now_us(), shared_offset_us)) {
    return;  // no shared mapping: a fit against a provisional timebase measures the timebase
  }
  server_ts = played_ts + shared_offset_us;
#else
  return;
#endif

  if (st.rate_server.n == 0.0) {
    st.rate_x0 = server_ts;
    st.rate_x0_local = played_ts;
    st.rate_y0 = played;
    st.rate_window_start_us = now_us();
  }
  const double y = static_cast<double>(played - st.rate_y0);
  st.rate_server.add(static_cast<double>(server_ts - st.rate_x0), y);
  // The control fit, on the same samples: local time needs no mapping, so it cannot carry the
  // mapping's errors. Its expected value is the programmed trim.
  st.rate_local.add(static_cast<double>(played_ts - st.rate_x0_local), y);

  if (now_us() - st.rate_window_start_us < RATE_WINDOW_US) {
    return;
  }
  if (st.rate_server.n >= RATE_MIN_SAMPLES) {
    const double nominal = static_cast<double>(rec.params.sample_rate);
    double sd_s = 0.0, sd_l = 0.0;
    const double hz_s = st.rate_server.slope(sd_s) * 1000000.0;
    const double hz_l = st.rate_local.slope(sd_l) * 1000000.0;
    ESP_LOGD(TAG,
             "ARATE%s srv_ppm=%+.4f loc_ppm=%+.4f srv_hz=%.4f loc_hz=%.4f n=%.0f span=%.1f s "
             "sd_srv=%.2f sd_loc=%.2f frames t=%" PRId64,
             (sd_s > RATE_MAX_SD_FRAMES || sd_l > RATE_MAX_SD_FRAMES) ? " REJECTED" : "",
             (hz_s / nominal - 1.0) * 1000000.0, (hz_l / nominal - 1.0) * 1000000.0, hz_s, hz_l,
             st.rate_server.n, static_cast<double>(now_us() - st.rate_window_start_us) / 1e6, sd_s, sd_l,
             now_us());
  }
  st.rate_server.reset();
  st.rate_local.reset();
  st.rate_window_start_us = now_us();
}

// THREAD CONTEXT: player task. See FAST_SPLICE_RELEASE_US for the argument.
int32_t SnapcastClient::fast_splice_(ServoState &st, int64_t median_err_us, uint32_t sample_rate,
                                     bool split_pending) {
  const int64_t threshold = static_cast<int64_t>(this->config_.fast_splice_threshold_us);
  const int64_t frame_us = sample_rate > 0 ? 1000000 / static_cast<int64_t>(sample_rate) : 0;
  // Retire the oldest in-flight splice whether or not one is applied this chunk: the ring is a
  // window on recent HISTORY, and freezing it while disabled would leave a stale debt behind.
  const int8_t retired = st.splice_hist[st.splice_hist_idx];
  int32_t applied = 0;

  const bool repair_settling =
      st.last_repair_us != 0 && now_us() - st.last_repair_us < FAST_SPLICE_REPAIR_HOLDOFF_US;
  if (threshold > 0 && frame_us > 0 && st.converged && !split_pending && !repair_settling) {
    // What the median has NOT yet seen. A splice moves the error immediately, but the median is
    // an average over 31 chunks, so about half a window of corrections are still invisible to it.
    const int64_t in_flight_us = static_cast<int64_t>(st.splice_sum) * frame_us;
    const int64_t effective_us = median_err_us - in_flight_us;
    if (st.fast_splice_active) {
      if (std::abs(effective_us) <= FAST_SPLICE_RELEASE_US || st.fast_splice_frames >= FAST_SPLICE_MAX_FRAMES) {
        if (st.fast_splice_frames >= FAST_SPLICE_MAX_FRAMES) {
          ESP_LOGW(TAG, "Fast splice hit its %" PRIu32 "-frame bound with %" PRId64
                        " us still standing -- treating as a measurement fault, handing back to the PI",
                   FAST_SPLICE_MAX_FRAMES, effective_us);
        } else {
          ESP_LOGD(TAG, "Fast splice done: %" PRIu32 " frames, %" PRId64 " us left for the PI t=%" PRId64,
                   st.fast_splice_frames, effective_us, now_us());
        }
        st.fast_splice_active = false;
      } else {
        applied = effective_us > 0 ? 1 : -1;
        st.fast_splice_seen_us = 0;  // an episode owns the timer; the next arm starts fresh
      }
    } else if (std::abs(effective_us) >= threshold) {
      // Above the threshold, but only ARM once it has stayed there. The timer is cleared the
      // moment the error drops back, so a transient never accumulates credit toward engaging.
      if (st.fast_splice_seen_us == 0) {
        st.fast_splice_seen_us = now_us();
      } else if (now_us() - st.fast_splice_seen_us >= FAST_SPLICE_PERSIST_US) {
        st.fast_splice_active = true;
        st.fast_splice_frames = 0;
        applied = effective_us > 0 ? 1 : -1;
        ESP_LOGI(TAG, "Fast splice engaged: %" PRId64 " us standing for %" PRId64
                      " s, correcting by position at one frame (%" PRId64 " us) per chunk t=%" PRId64,
                 effective_us, FAST_SPLICE_PERSIST_US / 1000000, frame_us, now_us());
      }
    } else {
      st.fast_splice_seen_us = 0;
    }
  } else {
    st.fast_splice_active = false;
    st.fast_splice_seen_us = 0;
  }

  st.splice_hist[st.splice_hist_idx] = static_cast<int8_t>(applied);
  st.splice_hist_idx = (st.splice_hist_idx + 1) % ServoState::SPLICE_HIST;
  st.splice_sum += applied - retired;
  if (applied != 0) {
    st.fast_splice_frames++;
  }
  return applied;
}

// THREAD CONTEXT: player task. See REANCHOR_BIAS_US for what this is for and what is unproven
// about it.
void SnapcastClient::reanchor_after_relock_(ServoState &st) {
  if (!this->config_.reanchor_after_reconnect) {
    return;
  }
  // ARMED BY THE RE-LOCK, NOT BY THE RECONNECT. The first version keyed on the session epoch, and
  // the very first natural event after it shipped walked past it: board a took a supply outage,
  // stormed, muted and re-locked at median 163 us WITHOUT a disconnect or a stream restart, so no
  // epoch changed and no cycle ran. That is the offset-planting event -- the anchor is re-derived
  // at every re-lock, not only when the session is rebuilt. The epoch is still one of the arming
  // reasons, because a boot or a reconnect is also a re-lock.
  const uint32_t epoch = this->session_epoch_.load(std::memory_order_relaxed);
  if (st.reanchor_epoch != epoch) {
    st.reanchor_epoch = epoch;
    st.reanchor_armed = true;
    st.reanchor_due_us = 0;
  }
  if (!st.reanchor_armed) {
    return;
  }
  if (!st.converged) {
    // Not locked yet, or knocked out of lock again. Restart the settle from the next unmute: a
    // perturbation applied mid-recovery measures the recovery.
    st.reanchor_due_us = 0;
    return;
  }
  if (st.reanchor_due_us == 0) {
    st.reanchor_due_us = now_us() + REANCHOR_SETTLE_US;
    return;
  }
  if (now_us() < st.reanchor_due_us) {
    return;
  }
  st.reanchor_armed = false;
  st.reanchor_due_us = 0;
  // A storm can plant several re-locks inside a minute, and each cycle costs a ramp, a hold and
  // its +-50 us. One per interval is enough: the repair removes a FRACTION of the standing error,
  // so the next event's cycle picks up whatever this one left.
  if (st.reanchor_last_us != 0 && now_us() - st.reanchor_last_us < REANCHOR_MIN_INTERVAL_US) {
    ESP_LOGD(TAG, "Re-anchor skipped: one fired %" PRId64 " s ago t=%" PRId64,
             (now_us() - st.reanchor_last_us) / 1000000, now_us());
    return;
  }
  st.reanchor_last_us = now_us();
  // Biases the ACCOUNTING only, leaving the audio alone, and is ramped -- the same path the test
  // hook uses, so a forced cycle and a manual one are the same experiment. Logged at INFO with its
  // own wording so the two are never confused in a log.
  this->inject_split_us_.store(REANCHOR_BIAS_US, std::memory_order_relaxed);
  ESP_LOGI(TAG, "Re-anchoring after re-lock: forcing one repair cycle (%+" PRId32 " us bias) t=%" PRId64,
           REANCHOR_BIAS_US, now_us());
}

// THREAD CONTEXT: player task. The gain schedule; see TRIM_KP_DECAY_TAU_S for the argument.
float SnapcastClient::trim_kp_(const ServoState &st) const {
  if (!st.converged) {
    // Muted acquisition is unchanged: nothing is audible, and the error must be nulled fast enough
    // to satisfy the unmute gate. The schedule only governs what happens AFTER the handoff.
    return TRIM_KP_ACQUIRE_PPM_PER_US;
  }
  const float age_s = static_cast<float>(now_us() - st.kp_event_us) / 1000000.0f;
  if (!(age_s < TRIM_KP_DECAY_SPAN_S)) {
    return TRIM_KP_RUN_PPM_PER_US;  // also the path for a never-set (0) event stamp
  }
  return TRIM_KP_RUN_PPM_PER_US +
         (TRIM_KP_ACQUIRE_PPM_PER_US - TRIM_KP_RUN_PPM_PER_US) * std::exp(-age_s / TRIM_KP_DECAY_TAU_S);
}

void SnapcastClient::mark_kp_event_(ServoState &st, const char *why) {
  const int64_t now = now_us();
  // Logged only when the schedule was actually near RUN, i.e. when this re-arm is a real change
  // of regime rather than the tenth chunk of one storm. Without the throttle a storm emits one
  // line per chunk on a link that is usually the thing that caused the storm.
  if (now - st.kp_event_log_us >= static_cast<int64_t>(TRIM_KP_DECAY_TAU_S * 1000000.0f)) {
    st.kp_event_log_us = now;
    ESP_LOGD(TAG, "KP re-armed to %.2f (%s), decaying to %.2f over %.0f s t=%" PRId64,
             TRIM_KP_ACQUIRE_PPM_PER_US, why, TRIM_KP_RUN_PPM_PER_US, TRIM_KP_DECAY_SPAN_S, now);
  }
  st.kp_event_us = now;
}

// PRE-TRIGGER HISTORY for the resync trace. THREAD CONTEXT: player task, all three.
//
// The armed burst answers "did discarding close the error?" and settled it. It cannot answer the
// question that replaced it -- WHY DOES THE RING EMPTY? -- because it is armed BY the threshold
// crossing, so RSYNC[0] already reads ring=26 and the drain is over before the first line exists.
// Every offset-planting event measured so far runs the same chain: ring empties -> late playout
// -> resync -> repair -> permanent displacement, and only the first link has never been watched.
//
// So: record every chunk unconditionally into a ring, and replay it when the burst arms. Same
// instrument, one buffer. The cost in steady state is five stores per chunk and no log traffic.
//
// Three constraints shaped the rest. The samples cannot live in ServoState, which is the player
// task's 6 KB stack -- hence PreSample and pre_trace_ on the client. The replay cannot be dumped
// in one go, because 80 lines at once would flood the log queue and the first casualty would be
// the live burst it is meant to be read against -- hence packing and one line per chunk. And the
// replay must not race the recorder -- hence the freeze, which is safe precisely because the
// frozen span is the span the live burst covers line for line.
static void append_num_(char *buf, size_t cap, size_t &pos, bool first, long long v) {
  if (pos + 1 >= cap) {
    return;
  }
  const int r = snprintf(buf + pos, cap - pos, first ? "%lld" : ",%lld", v);
  if (r < 0) {
    return;
  }
  pos = (static_cast<size_t>(r) < cap - pos) ? pos + static_cast<size_t>(r) : cap - 1;
}

// Saturating narrowing. Real errors run to a few seconds, but a timebase step once read
// 24888016 ms, and a wrapped int32 in a diagnostic is worse than a pegged one.
static int32_t clamp_i32_(int64_t v) {
  return static_cast<int32_t>(std::min<int64_t>(std::max<int64_t>(v, INT32_MIN), INT32_MAX));
}

void SnapcastClient::record_pre_trace_(ServoState &st, int64_t error_us, int64_t median_err_us, uint32_t ring_ms) {
  if (st.pre_dump_left > 0) {
    return;  // frozen while replaying; the live burst covers this span
  }
  const int64_t t = now_us();
  // dt is the gap from the predecessor, so the first sample after a boot or a freeze has no
  // predecessor to measure against and stores 0.
  const int64_t dt = st.pre_filled == 0 ? 0 : t - st.pre_last_t_us;
  PreSample &s = this->pre_trace_[st.pre_idx];
  s.dt_us = static_cast<uint32_t>(std::min<int64_t>(std::max<int64_t>(dt, 0), UINT32_MAX));
  s.err_us = clamp_i32_(error_us);
  s.med_us = clamp_i32_(median_err_us);
  s.ring_ms = static_cast<uint16_t>(std::min<uint32_t>(ring_ms, UINT16_MAX));
  s.drops = static_cast<uint16_t>(std::min<uint32_t>(st.resync_drops, UINT16_MAX));
  st.pre_last_t_us = t;
  st.pre_idx = (st.pre_idx + 1) % RESYNC_PRE_CHUNKS;
  if (st.pre_filled < RESYNC_PRE_CHUNKS) {
    st.pre_filled++;
  }
}

void SnapcastClient::arm_pre_trace_dump_(ServoState &st) {
  if (st.pre_filled == 0 || st.pre_dump_left > 0) {
    return;
  }
  st.pre_dump_left = st.pre_filled;
  st.pre_dump_pos = static_cast<uint16_t>((st.pre_idx + RESYNC_PRE_CHUNKS - st.pre_filled) % RESYNC_PRE_CHUNKS);
  st.pre_dump_label = st.pre_filled;  // oldest prints as -pre_filled, newest as -1
  // Samples store deltas only, so absolute time is reconstructed by walking back from the newest
  // sample's timestamp: dt of a sample is the gap from its PREDECESSOR, so reaching the oldest of
  // n samples subtracts n-1 of them.
  int64_t t = st.pre_last_t_us;
  uint16_t idx = st.pre_idx == 0 ? static_cast<uint16_t>(RESYNC_PRE_CHUNKS - 1) : static_cast<uint16_t>(st.pre_idx - 1);
  for (uint16_t k = 1; k < st.pre_filled; k++) {
    t -= this->pre_trace_[idx].dt_us;
    idx = idx == 0 ? static_cast<uint16_t>(RESYNC_PRE_CHUNKS - 1) : static_cast<uint16_t>(idx - 1);
  }
  st.pre_dump_t_us = t;
}

void SnapcastClient::emit_pre_trace_line_(ServoState &st) {
  if (st.pre_dump_left == 0) {
    return;
  }
  const uint16_t n = std::min<uint16_t>(RESYNC_PRE_PER_LINE, st.pre_dump_left);
  // Field-major packing: one CSV list per quantity, so a reader can eyeball ring= across six
  // chunks on one line, and a parser can zip the lists. Sized for six saturated values each.
  char dt[96], err[96], med[96], ring[64], drops[64];
  size_t p_dt = 0, p_err = 0, p_med = 0, p_ring = 0, p_drops = 0;
  dt[0] = err[0] = med[0] = ring[0] = drops[0] = '\0';
  const int64_t t_first = st.pre_dump_t_us;
  const uint16_t label_first = st.pre_dump_label;
  for (uint16_t k = 0; k < n; k++) {
    const PreSample &s = this->pre_trace_[st.pre_dump_pos];
    append_num_(dt, sizeof(dt), p_dt, k == 0, s.dt_us);
    append_num_(err, sizeof(err), p_err, k == 0, s.err_us);
    append_num_(med, sizeof(med), p_med, k == 0, s.med_us);
    append_num_(ring, sizeof(ring), p_ring, k == 0, s.ring_ms);
    append_num_(drops, sizeof(drops), p_drops, k == 0, s.drops);
    st.pre_dump_pos = static_cast<uint16_t>((st.pre_dump_pos + 1) % RESYNC_PRE_CHUNKS);
    st.pre_dump_label--;
    st.pre_dump_left--;
    // pre_dump_t_us tracks the sample now under the cursor, so it advances by THAT sample's dt.
    if (st.pre_dump_left > 0) {
      st.pre_dump_t_us += this->pre_trace_[st.pre_dump_pos].dt_us;
    }
  }
  ESP_LOGD(TAG, "RPRE[-%u..-%u] t=%" PRId64 " dt=%s err=%s med=%s ring=%s drops=%s", label_first,
           static_cast<unsigned>(label_first - n + 1), t_first, dt, err, med, ring, drops);
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
      // Mixer conservation residual, in us: taken in, minus passed on, minus what it says it holds.
      // Must be zero. Non-zero says the depth reading is not describing the whole pipeline, and the
      // repair below refuses to act on the difference when this explains it. See the gate.
      int32_t mix_residual_us = 0;
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
            // Conservation across the mixer, at the same instant and from the same snapshot as the
            // drift itself. src_consumed and sink_received are CUMULATIVE while xfer is a level, which
            // is what makes this worth computing: frames merely in flight inside the mixer show up
            // here and then go back to zero when they are passed on, while frames genuinely lost
            // there leave a residual that never returns.
            {
              const int64_t rate_r = static_cast<int64_t>(rec.params.sample_rate);
              const int64_t xfer_f = static_cast<int64_t>(measured.dbg_xfer_us) * rate_r / 1000000;
              const int64_t inflight_f = static_cast<int64_t>(measured.dbg_inflight_us) * rate_r / 1000000;
              // Subtracting the in-flight stage is what makes this a conservation check again rather
              // than a restatement of it. src_consumed = written_to_sink + xfer, so before the mixer
              // reported the in-flight term this residual was ALGEBRAICALLY EQUAL to it --
              // written_to_sink - sink_received -- which is why it matched the observed split to 1 us
              // and why the split existed at all. With the term reported and included here the
              // residual is zero by construction, so the gate below is now a regression guard rather
              // than a live defence: it fires only if some stage goes missing from the chain again.
              const int64_t resid_f = static_cast<int64_t>(measured.dbg_src_consumed) -
                                      static_cast<int64_t>(measured.dbg_sink_received) - xfer_f - inflight_f;
              mix_residual_us = static_cast<int32_t>(
                  std::clamp<int64_t>(resid_f * 1000000 / rate_r, INT32_MIN / 2, INT32_MAX / 2));
            }

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
            ESP_LOGD(TAG,
                     "RECON drift=%" PRId32 " acct=%" PRId64 " meas=%" PRIu32 " sum=%" PRIu32 " own=%" PRIu32
                     " xfer=%" PRIu32 " inflight=%" PRIu32 " queued=%" PRIu32 " dma=%" PRIu32 " age=%" PRId64
                     " pushed=%" PRId64
                     " played=%" PRId64 " clamped=%" PRId64 " | srcrx=%" PRIu32 " srctx=%" PRIu32 " sinkrx=%" PRIu32
                     " r_push=%" PRId64 " r_src=%" PRId64 " r_mix=%" PRId64 " pad=%" PRIu32,
                     fill_drift_us, accounted_us, measured.microseconds,
                     measured.dbg_own_us + measured.dbg_xfer_us + measured.dbg_inflight_us + measured.dbg_queued_us +
                         measured.dbg_dma_us,
                     measured.dbg_own_us, measured.dbg_xfer_us, measured.dbg_inflight_us, measured.dbg_queued_us,
                     measured.dbg_dma_us,
                     now_us() - measured.as_of_us, dbg_pushed, dbg_played, this->dbg_clamped_frames_,
                     measured.dbg_src_received, measured.dbg_src_consumed, measured.dbg_sink_received,
                     dbg_pushed - static_cast<int64_t>(measured.dbg_src_received),
                     static_cast<int64_t>(measured.dbg_src_received) -
                         static_cast<int64_t>(measured.dbg_src_consumed) - own_f,
                     static_cast<int64_t>(measured.dbg_src_consumed) -
                         static_cast<int64_t>(measured.dbg_sink_received) - xfer_f -
                         static_cast<int64_t>(measured.dbg_inflight_us) * rate_i / 1000000,
                     // Cumulative silence the SINK has padded in front of our audio. Deliberately not
                     // folded into any residual: it is not a conservation failure, every frame of it
                     // was really played. It is here because it DISPLACES our audio and no other field
                     // can show that -- the accounting counts real frames, so a padded frame moves the
                     // audio one frame later while every metric still agrees with itself. Two devices
                     // differing by N frames of padding should sit N * (1e6 / rate) us apart on a
                     // logic analyser, which is the prediction to test.
                     measured.dbg_padded_frames);
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
      // Windowed MEDIAN of the accounting split, used by the repair below and reported after it.
      // Declared here so both see it: the spikes that pollute a mean are rare and fixed-size, so a
      // median rejects them outright. INT32_MIN means no samples yet -- nothing to act on.
      int32_t drift_med_us = INT32_MIN;
      if (st.drift_samples > 0) {
        const size_t mn = std::min<size_t>(st.drift_samples, ServoState::DRIFT_WINDOW);
        int32_t msorted[ServoState::DRIFT_WINDOW];
        memcpy(msorted, st.drift_window_us, mn * sizeof(int32_t));
        std::nth_element(msorted, msorted + mn / 2, msorted + mn);
        drift_med_us = msorted[mn / 2];
      }
      // Carried for the unmute gate at the top of the loop; see UNMUTE_ANCHOR_US.
      st.drift_med_last_us = drift_med_us;
      if (fill_comparable && st.converged) {
        // SIGN-SYMMETRIC. Drift in either direction is the same defect -- the accounted queue
        // disagrees with the measured chain, so the prediction is wrong by that much and the servo
        // steers real audio to the wrong time while every on-device metric agrees with itself.
        //
        // Negative drift was excluded because, with a mixer in the chain, drift sawtooths between
        // ~0 and -100 ms as a source ring fills and drains, and a magnitude test alone fires on the
        // peaks. But the sign was the wrong filter for that: the property that separates artefact
        // from split is STEADINESS, which the band-and-hold test below already applies. The
        // sawtooth spans ~100 ms and fails a 10 ms band on its own; a real split does not.
        //
        // Measured tonight, against a logic analyser, the offset a board renders at is -drift:
        //
        //   drift  -8526 us -> wire   8.51 ms      drift -68549 us -> wire  73 ms
        //   drift -64672 us -> wire     67 ms      drift -194060 us -> wire 198 ms
        //   drift -198435 us -> wire   191 ms
        //
        // Five for five within a few percent, and every one of them NEGATIVE -- so the excluded
        // sign is precisely the one the re-baseline anchor plants, and it was unrepairable by
        // construction. That is why these offsets persisted for as long as the device stayed up.
        //
        // A repair of this size is audible (field-measured corrections of -66 to -220 ms), but a
        // permanent 191 ms offset is worse, and it is the sound of the defect being removed rather
        // than created.
        // The MEDIAN is the repair's input, so it is computed before the report that displays it.
        // INT32_MIN means no samples yet, which the repair treats as "nothing to act on".
        //
        // KEYED ON THE WINDOWED MEDIAN, not on the single end-of-window sample. Measured on both
        // boards in steady state: median +0/-1 us every window, while the mean of the same window
        // ran -1320 to -5190 and the minimum hit -42246. The split is genuinely ZERO when nothing
        // is wrong; what polluted it was rare fixed-size spikes, one or two per 32 samples, which a
        // mean carries in at spike/n and a median rejects outright.
        //
        // That is what lets the thresholds below come down. They were sized for a "0 to -100 ms
        // sawtooth" that no longer exists -- most likely removed when the mixer began reading the
        // sink and its transfer buffer on the same side of the hand-off -- and a 20 ms floor could
        // not see the 8.5 ms offset measured on the wire earlier tonight at all.
        //
        // REFUSED when the mixer's conservation residual already accounts for the split. The repair's
        // premise is "trust the measurement", and this is the one cheap test of whether the
        // measurement is self-consistent: r_mix must be zero, and when it is not, the depth reading
        // is not describing the whole pipeline. Subtracting the difference from `pushed` would then
        // corrupt an accounting that was correct.
        //
        // Measured on hardware, which is why the tolerance is this tight: a device came up with
        // drift +25509 us held steady for 14 s -- far longer than DRIFT_REPAIR_HOLD_US, so the
        // steadiness test passed -- against r_mix of 1125 frames, i.e. 25510 us. One microsecond
        // apart. Those frames were being HELD by the mixer, not lost: r_mix returned to 0, the drift
        // went with it to +22 us, and no repair was needed or wanted. The sign of that phantom was
        // the one the servo then answered with a 1260-frame insertion, and the pair ended up ~85 us
        // apart on a logic analyser.
        //
        // A genuine loss inside the mixer does NOT hide behind this gate: src_consumed and
        // sink_received are cumulative, so lost frames leave r_mix permanently offset while the
        // drift they cause matches it -- and that case is the mixer's bug to fix, not something to
        // paper over by moving our own counters. The other splits seen tonight are unaffected: the
        // -25488 us one had r_mix exactly 0 and repaired as before, and the -42246 us spike had
        // r_mix 441 frames against a 42 ms drift, nowhere near explaining it.
        const int32_t drift_for_repair = drift_med_us;
        const bool residual_explains =
            drift_for_repair != INT32_MIN && std::abs(mix_residual_us) >= DRIFT_REPAIR_US / 2 &&
            std::abs(drift_for_repair - mix_residual_us) <= MIX_RESIDUAL_MATCH_US;
        if (residual_explains) {
          // Logged whenever the split is big enough that the repair WOULD have looked at it, not
          // only when a window was already open. The gate disarms before the arming branch below,
          // so without this a phantom that shows up while unarmed leaves no trace and the gate
          // cannot be told from a gate that never fires.
          if (std::abs(drift_for_repair) >= DRIFT_REPAIR_US) {
            ESP_LOGW(TAG,
                     "Accounting split %+" PRId32 " us left alone: the mixer's conservation residual is "
                     "%+" PRId32 " us, so the depth reading is not describing the whole pipeline%s",
                     drift_for_repair, mix_residual_us, st.drift_excess_since_us != 0 ? " (window dropped)" : "");
          }
          st.drift_excess_since_us = 0;
        } else if (drift_for_repair != INT32_MIN && std::abs(drift_for_repair) >= DRIFT_REPAIR_US &&
            st.drift_excess_since_us == 0) {
          st.drift_excess_since_us = now_us();
          st.drift_excess_min_us = drift_for_repair;
          st.drift_excess_max_us = drift_for_repair;
        }
        if (st.drift_excess_since_us != 0 && drift_for_repair != INT32_MIN && !residual_explains) {
          st.drift_excess_min_us = std::min(st.drift_excess_min_us, drift_for_repair);
          st.drift_excess_max_us = std::max(st.drift_excess_max_us, drift_for_repair);
          if (st.drift_excess_max_us - st.drift_excess_min_us > DRIFT_STEADY_BAND_US) {
            // Moving, so it is a measurement artefact rather than a split. Restart the window from
            // here rather than abandoning it: a genuine split that begins during a noisy patch should
            // still be caught once the noise passes. Restarting DISARMED unless this sample would
            // have armed it on its own -- otherwise a drift that collapsed to zero would keep a
            // window open on the strength of a magnitude it no longer has.
            st.drift_excess_since_us = (std::abs(drift_for_repair) >= DRIFT_REPAIR_US) ? now_us() : 0;
            st.drift_excess_min_us = drift_for_repair;
            st.drift_excess_max_us = drift_for_repair;
          } else if (now_us() - st.drift_excess_since_us >= DRIFT_REPAIR_HOLD_US) {
            // Trust the measurement: drop the accounted queue by the whole drift. Playback was
            // running that far early, so the prediction moves later and the servo walks the phase
            // back through the proportional band.
            const int64_t excess_frames =
                static_cast<int64_t>(drift_for_repair) * static_cast<int64_t>(rec.params.sample_rate) / 1000000;
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
                     "Accounting split repaired: accounted queue ran %+" PRId32 " us against measured latency "
                     "for %" PRId64 " s; playback was that far %s",
                     drift_for_repair, DRIFT_REPAIR_HOLD_US / 1000000, drift_for_repair > 0 ? "early" : "late");
            // The repair steps the accounting, so the median error the PI sees is about to move by
            // the size of the split. Nulling that fast is what limits how long the displacement
            // integrates -- and the repair's displacement is the largest single term measured.
            this->mark_kp_event_(st, "split repair");
            // Position correction stands down for a while: the jump the servo is about to see is
            // this step, not a displacement. See FAST_SPLICE_REPAIR_HOLDOFF_US.
            st.last_repair_us = now_us();
            st.drift_excess_since_us = 0;
          }
        }
      } else {
        // Nothing to compare against, or not yet converged: no window may be open.
        st.drift_excess_since_us = 0;
      }

      char dl_str[48] = "";
      if (st.dl_step_valid) {
        // Spread of the per-chunk STEP, i.e. the timebase's movement with its drift removed. A
        // smooth ramp reads ~0; a glitch reads its own size. The absolute offset is deliberately
        // not reported: it carries this device's local epoch, so it is neither comparable across
        // devices nor interesting on one, and including it pushed the report past the log line
        // limit and silently truncated trim and tsf off the end.
        snprintf(dl_str, sizeof(dl_str), ", tbjit %" PRId64, st.dl_step_max_us - st.dl_step_min_us);
        st.dl_step_valid = false;
      }
      char drift_str[96] = "";
      if (st.drift_samples > 0) {
        // Median over the window: the spikes are rare and fixed-size, so a median rejects them
        // where a mean carries them in at spike/n.
        // Median plus range. The mean was dropped for line length: the range already exposes the
        // spikes the median is rejecting, which is what it was there to show.
        snprintf(drift_str, sizeof(drift_str), ", split %+" PRId32 " (%+" PRId32 "..%+" PRId32 ")", drift_med_us,
                 st.drift_min_us, st.drift_max_us);
      }
      st.drift_accum_us = 0;
      st.drift_samples = 0;
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
      char tsf_str[128] = "";
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

        // RENDER PHASE: the TSF instant at which this device renders server audio time zero.
        //
        // The depth published above compares buffer OCCUPANCY, and two devices rendering in
        // perfect sync legitimately hold different amounts -- which is why that delta reads
        // -13.4 ms for an offset a logic analyser measures at 3.7 ms. This compares WHEN A
        // KNOWN FRAME RENDERS instead, which must be identical across devices playing the same
        // stream, so a difference is real skew rather than a difference of buffering.
        //
        // Built from direct observations only, the same terms the offline RAW line uses:
        //   (played, played_ts) is ground truth -- that many frames HAD rendered at that local
        //   time; (s_ts, pushed) anchors the frame count to server audio time, the same number
        //   on every device for the same audio; (tsf, tsf_local) converts to the one clock the
        //   devices provably share. No servo state and no predicted playout: every wrong
        //   diagnosis in this area came from trusting a model of when audio renders.
        //
        // Once per report, not per chunk: a TSF read costs 45-81 us and this needs one, which
        // is nothing at 3.3 s intervals and would not be at 38/s.
        //
        // NOTE it consumes (pushed - played), so it inherits any accounting error. That is a
        // real limit: it cannot say WHY two devices disagree, only that they do and by how
        // much. Which is the measurement four failed hypotheses lacked.
        int64_t phase_tsf = 0, phase_local = 0, phase_width = 0;
        if (TsfSync::raw_tsf_sample(phase_tsf, phase_local, phase_width)) {
          this->playout_mutex_.lock();
          const int64_t p_played = this->played_frames_total_;
          const int64_t p_pushed = this->pushed_frames_total_;
          const int64_t p_played_ts = this->played_last_ts_us_;
          const bool p_valid = this->playout_valid_;
          this->playout_mutex_.unlock();
          if (p_valid && p_played_ts > 0) {
            const int64_t render_tsf = p_played_ts + (phase_tsf - phase_local);
            const int64_t render_server = rec.server_ts_us - (p_pushed - p_played) * 1000000 /
                                                                 static_cast<int64_t>(rec.params.sample_rate);
            this->tsf_sync_->set_render_phase_us(render_tsf - render_server);
          } else {
            this->tsf_sync_->set_render_phase_us(TsfSync::RENDER_PHASE_UNKNOWN);
          }
        }
        const TsfSync::Role role = this->tsf_sync_->role();
        const int64_t own_phase = this->tsf_sync_->render_phase_us();
        if (role == TsfSync::Role::LEADER) {
          snprintf(tsf_str, sizeof(tsf_str), ", tsf=leader(peers %u, phase %s)", this->tsf_sync_->peer_count(),
                   own_phase == TsfSync::RENDER_PHASE_UNKNOWN ? "unknown" : "set");
        } else if (role == TsfSync::Role::FOLLOWER) {
          // depth delta vs the leader: the only visibility we have into an absolute
          // playout offset, which the median above cannot show by construction
          const int32_t depth_delta = this->tsf_sync_->pipeline_delta_us();
          if (depth_delta == INT32_MIN) {
            snprintf(tsf_str, sizeof(tsf_str), ", tsf=follower(%.1fs)", this->tsf_sync_->mapping_age_s(now_us()));
          } else {
            const int32_t render_delta = this->tsf_sync_->render_delta_us();
            if (render_delta == INT32_MIN) {
              // Say WHICH side is missing: "mine" means this device has not computed a phase,
              // "leader" means the beacon carried none. Without that the absence is mute.
              snprintf(tsf_str, sizeof(tsf_str), ", tsf=follower(%.1fs, depth %+" PRId32 " us, render none/%s)",
                       this->tsf_sync_->mapping_age_s(now_us()), depth_delta,
                       own_phase == TsfSync::RENDER_PHASE_UNKNOWN ? "mine" : "leader");
            } else {
              // render= is the one to trust of the two; depth= is kept alongside it precisely so
              // the two can be compared against the analyser before anything acts on either.
              snprintf(tsf_str, sizeof(tsf_str), ", tsf=follower(%.1fs, depth %+" PRId32 " render %+" PRId32 " us)",
                       this->tsf_sync_->mapping_age_s(now_us()), depth_delta, render_delta);
            }
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
               " us mean / %" PRId64 " ms max, buffered %" PRIu32 " ms, pipeline %" PRId32 " ms%s%s%s%s%s over %" PRIu32
               " chunks",
               st.err_accum_us / st.err_count, st.err_peak_us, median_err_us, st.soft_dropped_frames, st.soft_inserted_frames,
               st.hard_resyncs, fb_mean_gap_us, max_gap_us / 1000, buffered_ms, pipeline_ms, fill_str, drift_str,
               dl_str, trim_str, tsf_str, st.err_count);
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
