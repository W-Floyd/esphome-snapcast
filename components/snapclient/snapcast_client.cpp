#include "snapcast_client.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#ifdef CLOCK_SYNC_TSF_ACTIVE
#include "esphome/components/json/json_util.h"
#endif

#include <esp_timer.h>
#ifdef USE_SNAPCLIENT_OPUS
#include <esp_heap_caps.h>
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
// Longest silence (no message of any kind) tolerated on a connected session; see recv_exact_.
static constexpr int64_t SESSION_SILENCE_US = 15LL * 1000000;

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
// 0.25 -> 0.125 (2026-08-28), on three independent measurements that agree on the mechanism, and
// only after the objection that reverted the last attempt was shown not to apply any more.
//
// THE JITTER IS THIS LOOP'S LIMIT CYCLE. Measured against the analyser over 1.5 h of event-free
// data (n=60506, SNR 235x over the instrument's 0.027 us floor):
//
//   structure function   sd of skew differences 0.30 us at tau 0.1 s, rising to a PLATEAU of
//                        9.0 us for tau >= 30 s -- a bounded wander, not white noise (which would
//                        be flat at sqrt(2)*sigma from the shortest lag) and not a random walk
//                        (which would keep growing). Corner at 10-30 s.
//   trim oscillation     ~24 s period (5 zero crossings per 60 s), 73 ppm p2p, present CONTINUOUSLY
//                        in windows with no disturbance within 180 s.
//   loop gain            KP x 3.15 us/ppm = 0.79, so amplification 1/(1-G) = 4.8x.
//
// Same timescale from three directions, so the 9 us plateau IS the lightly-damped loop cycle.
// At 0.125 the gain is 0.39 and the amplification 1.6x, predicting a plateau near 3 us -- a 3x
// reduction, which is more than the linear factor because the loop-gain term pays as well.
//
// WHY THE 0.1 REVERT DOES NOT APPLY. That test predates the gain schedule by 27 commits
// (4393039 vs 5b751f9), so 0.1 was applied during RECOVERY too, and both objections it raised are
// recovery-phase effects: 295 s to settle, and a -155 us landing offset (the wire offset is the
// integral of the differential rate, so a slower null lets the integral run longer). Under the
// schedule recovery runs at ACQUIRE 0.5 and decays to RUN over 60 s, so RUN no longer governs it.
//
// AND RECOVERY HAS ENORMOUS MARGIN, measured under the schedule across 32 real hard resyncs --
// nobody had re-measured it, and the 54 s figure quoted above predates the schedule and every
// other fix since: |median| back within 60 us in 2.3 s median, within 30 us in 6.0 s (tail to
// 51-71 s). Even a 4x slowdown leaves it under 10 s. CAVEAT: that is the SERVO's median returning
// to band, which this file's central lesson says can read clean while the wire is displaced -- so
// the landing offset is a separate quantity and is still the bar this change has to clear.
//
// ACQUIRE IS DELIBERATELY UNCHANGED. At 0.5 its loop gain is 1.58, above the ultimate 0.317, and
// that is safe only because the schedule decays it: gain(t) = 0.25 + 0.25*e^(-t/20) crosses 0.317
// at t = 26 s, about ONE oscillation period, so the ring grows once and then damps. Measured
// confirmation: post-event and quiet 60 s windows show the SAME 5.0 zero crossings, i.e. ACQUIRE
// adds amplitude (92 vs 73 ppm p2p) but no extra ringing. Lowering it would cost recovery speed to
// fix something the decay already handles; raising it buys ringing rather than settling.
// REVERTED to 0.25 (2026-08-28). The loop-gain reasoning below is unchanged and still looks right,
// but after five 5-minute windows aborted on bench events it was never graded, while its COST was
// measured immediately: per-board median error ran 1-217 us against +-30-60 at 0.25, because a lower
// gain nulls each board's own error against server time more slowly. Inter-device skew -- what
// imaging needs -- looked fine, but the unmute gate and st.converged both read those medians.
//
// A measured cost against an unmeasured benefit decides it. Re-attempt when the bench is quiet
// enough to hold a 5-minute window, and grade it on the STRUCTURE FUNCTION plateau (9.0 us at
// tau >= 30 s pre-change, predicted ~3 us at 0.125) rather than on sd, plus the landing offset
// against the +-130 us band.
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

// --- THE DELAY LOOP (PLAN-delay-controlled-servo.md) ---
//
// PI rate steering on the MEASURED tag error (err_tag), setpoint zero. The plant is the pipeline
// as a black box: dD/dt = -(trim_ppm + crystal_ppm), so the integral is the learned crystal
// offset and is not optional -- proportional-only parks at crystal/Kp (~40/0.1 = 400 us here).
//
// Block of DL_BLOCK_N arrivals at the ~100 Hz descriptor cadence: ~320 ms, resolution ~5 us
// (block-means sweep, 2026-08-28), update ~3 Hz. Dead time is one pipeline depth (~250-300 ms),
// so the update rate stays out of the way of the ~0.5 Hz bandwidth bound instead of being it.
static constexpr uint32_t DL_BLOCK_N = 32;
// tau = 1/Kp = 10 s. The plan grades 8-10 s against 30 s; 10 s is the starting point because
// three independent arguments favour the shorter figure: mid-band errors converge ~3x faster
// than today's servo rather than ~7x slower, the cold-boot integral wind-up peak is ~200 us
// against the 1 ms splice threshold (5x margin, vs 1.6x at 30 s), and at 3 Hz sampling 10 s is
// ~35x the dead time and ~30x the sample interval -- comfortably ordinary. GRADE IT ON THE WIRE
// before trusting it; this project's history is that gains right on paper oscillate on hardware.
static constexpr float DL_KP_RUN_PPM_PER_US = 0.1f;
// Ti = tau, i.e. Ki = Kp/Ti = Kp^2 and damping zeta = Kp/(2*sqrt(Ki)) = 0.5 exactly. NOT the old
// loop's Kp^2/4 (zeta = 1): the plan's transient arithmetic (cold-boot peak, setpoint response)
// was all derived at zeta = 0.5, so changing the ratio silently invalidates those numbers.
//
// No arrival for this long means tagged audio STOPPED (resampler, mixer blend, client-inserted
// silence, startup) -- the tags arrive per DMA descriptor (~10 ms) whenever they arrive at all.
// The loop then HOLDS its last trim: the integral is the learned crystal offset and remains the
// best available rate estimate; zeroing it would itself be a rate step of tens of ppm.
static constexpr int64_t DL_TAG_STALE_US = 1000000;
// How long a completed block mean stays a valid input for fast_splice_ before position
// correction hands back to the demoted prediction (where the split-pending guard applies again).
// Blocks complete every ~320 ms, so three missed blocks = the signal is gone, not late.
static constexpr int64_t DL_ERR_STALE_US = 1000000;
// Error-proportional gain: Kp = (1/tau) * max(1, |err| / knee_us), Ti divided by the same
// factor, both capped so the effective tau never drops below tau_min_s. Inside the knee the loop
// runs the slow tunables exactly (tau 120 / Ti 600 for the sub-us steady state); a few hundred us
// after a boot or re-anchor is closed at tau 20 instead of 2-3 x 120 s (measured 13:27-13:33,
// build 18: +330 us decaying with no overshoot for minutes). Continuous in the error, so there is
// no state, no hysteresis and no gain step to excite; a large error simply gets a stiffer loop.
// Both runtime tunables now (servo_param knee_us / tau_min_s); defaults in snapcast_client.h.
// Tag fault (see ServoState::tag_miss): consecutive unmoved corrections that fault the tag path,
// and how long the ledger takes over before tags are trusted again.
static constexpr uint8_t TAG_FAULT_MISSES = 3;
static constexpr int64_t TAG_SPLIT_US = 3000;  // tag vs ledger disagreement that makes a miss a fault
static constexpr int64_t TAG_JUDGE_US = 2000000;   // pipeline 0.28 s + a 0.65 s block average + margin; 1 s judged blocks that still held pre-correction samples (22:42 false fault)
static constexpr int64_t TAG_SETTLE_US = 20000000; // no fault judgement in the first 20 s after engage
static constexpr int64_t TAG_FAULT_US = 180LL * 1000000;  // 60 expired before the repair got its window (14:29-14:33)
// Tag-stream blanking after a setpoint change / hard resync / timebase re-anchor: one pipeline
// depth (~250-300 ms measured) plus margin, so every arrival folded into a block was scheduled
// against the deadline the loop is currently steering toward.
static constexpr int64_t DL_SETPOINT_BLANK_US = 500000;
// Nominal tag arrival interval (one DMA descriptor), for converting half an averaging block into
// chunks when deriving the splice in-flight horizon.
static constexpr int64_t DL_ARRIVAL_US = 10000;
// A feedback gap beyond this is a SPEAKER-CALLBACK STALL: the DAC kept draining DMA, and on
// resume the queued completions are stamped with a late "now", so every local render instant in
// the catch-up burst is wrong by up to the stall length. Those stamps feed err_tag AND the
// published render phase, and the tag-age gate cannot catch them because it compares the same
// late stamps against each other. Measured on B 2026-08-28: gaps of 92/439 ms stamped phase
// spikes of +141/+263 ms into the group while TSF, tag age, and playout were all clean. Blank
// the tag stream and the phase publish instead. Normal per-window max is ~10 ms.
static constexpr int64_t FEEDBACK_GAP_BLANK_US = 50000;
// Crystal-offset persistence cadence: only a real change, and rarely -- an NVS commit blocks the
// player task for up to ~100 ms (tolerable against a ~1.7 s buffer, but not per block) and wears
// flash. Crystal moves with temperature over tens of minutes; 2 ppm / 10 min tracks that with
// at most ~6 writes/hour worst case.
static constexpr float DL_PERSIST_DELTA_PPM = 2.0f;
static constexpr int64_t DL_PERSIST_MIN_INTERVAL_US = 600000000;
// Continuous engagement required before the integral is trusted enough to persist: ~6 tau, so
// the wind-up (and any boot overshoot) is over. Without it each boot re-saved its first guess.
static constexpr int64_t DL_PERSIST_SETTLE_US = 60000000;
// Time constant of the integral average that is persisted: long against the ~45 s common-mode
// timebase excursions that swing the instantaneous integral by tens of ppm, short against the
// tens-of-minutes crystal temperature drift it exists to capture.
static constexpr float DL_PERSIST_EMA_S = 300.0f;
// For the first minutes after BOOT (esp_timer time, so mapping flaps later do not re-trigger it)
// the integral time is short: a restored integral can be ~20 ppm stale (timebase drift moved
// while the board was off), which parks a standing error of mismatch/Kp that Ti = 120 s takes
// ~5 min to absorb. 20 s absorbs it in about a minute; the common-mode wander swing that a fast
// Ti allows is tolerated for these minutes only.
// Fast integral after boot: DISABLED (window 0). Build 16 used Ti 20 s for 180 s to absorb a
// stale restored integral, but with the integral persisted at shutdown the restore is exact
// (+56.80 saved, +56.80 restored), and the fast Ti instead integrated the first seconds' settling
// transient (err +500..+950 us behind a 1000->2000 ms setpoint change) into a +13 ppm error
// (56.8 -> 69.6 in 8 s, build 17 boot). Kept as a switch for a board with no NVS value.
static constexpr int64_t DL_TI_BOOT_WINDOW_US = 180000000;  // COLD START ONLY (dl_cold_start): no NVS value to restore
static constexpr float DL_TI_BOOT_S = 20.0f;
// Out of range with the integral this far from its own slow average, the integral is wrong (it
// was caught mid-swing by a hold: measured +114 against a +57 crystal, board then ran 50 ppm fast
// and the hold kept it there). Snap it to the average; the fast path owns the position anyway.
static constexpr float DL_INTEGRAL_SNAP_PPM = 20.0f;
// Splice in-flight horizon for the DEMOTED PREDICTION signal: half the median window, the
// historical value -- a splice reaches a 31-chunk median after ~15 chunks.
static constexpr uint32_t SPLICE_HORIZON_PREDICTION_CHUNKS = 15;
static constexpr int64_t DL_LOG_INTERVAL_US = 1000000;

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
// How long a player-task stall runs before it says so, and how often it repeats. The stalls are
// legitimate waits -- the record's PCM is guaranteed to arrive, so giving up would be wrong -- but
// a silent unbounded wait is indistinguishable from a dead task, which is exactly how three wedges
// today read in the logs. Long enough that ordinary inter-chunk gaps never trip it.
static constexpr int64_t PUSH_STALL_LOG_US = 3000000;
// How long emit_pcm_ (the network task) may wait for ring room before dropping the chunk instead.
// Two seconds is longer than any legitimate drain wait (one chunk frees in 26 ms) and far shorter
// than the dead-session and bailout detectors, which must keep running on this task.
static constexpr int64_t EMIT_ROOM_WAIT_MAX_US = 2000000;
// How long the player may go without completing a chunk before the main loop says so. Well past
// any legitimate gap: the keepalive path completes chunks, and a stream that has genuinely ended
// clears stream_active_, which gates the warning.
static constexpr int64_t PLAYER_STALL_US = 5000000;
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
// Longest AudioDepth snapshot age whose composite total is still coherent enough to difference
// against our own accounting. The mixer stamps the total with the sink's instant but reads its own
// terms now, so once the sink has rendered a descriptor's worth since that stamp the total
// over-states the present pipeline by the age. One DMA span (5 x 10 ms on the i2s std speaker) is
// where that becomes certain. See the gate in the fill cross-check for the measurement.
static constexpr int64_t DEPTH_SNAPSHOT_COHERENT_US = 50000;
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
// Follower-side inter-device alignment.
//
// GAIN AND RATE ARE SET FROM THE MEASURED LOOP DELAY, not from how fast convergence would be
// nice. At gain 0.25 stepping every report, the pair held a sustained LIMIT CYCLE: period 26.2 s
// (7.8 report intervals), amplitude 207 us p2p, measured on the analyser. For a proportional
// loop against a delay, sustained oscillation sits at period ~= 4x the delay, so the loop sees
// its own correction about 6.5 s late -- roughly two reports. That is physical: the render phase
// is computed once per report from playout that already reflects earlier audio, and the servo
// then needs time to act on the shifted deadline.
//
// So the previous gain was at or above the ultimate gain. Ziegler-Nichols puts a stable
// proportional gain near 0.5x ultimate; 0.05 is a 5x reduction on a value already known to
// oscillate. Stepping every third report puts the correction interval beyond the loop delay as
// well, which does not depend on the gain estimate being right.
// RENDER_ALIGN_GAIN: now servo_param align_gain (default 0.05)
// Reports between corrections. One report was inside the loop delay, which is what sustained the
// cycle above.
static constexpr uint32_t RENDER_ALIGN_EVERY_N_REPORTS = 3;
// RENDER_ALIGN_MAX_STEP_US: now servo_param align_step_us (default 5)
// Below this the delta is measurement noise: the render delta's own MAD is ~15 us against a
// truth MAD of 1.7 us, so correcting inside that band would inject noise, not remove it.
// RENDER_ALIGN_DEADBAND_US: now servo_param align_deadband_us (default 20)
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
// Own error against the server timeline that is still acceptable to unmute into when the GROUP
// agrees (see the unmute latch): a common offset of this size is inaudible as such; mutual offsets
// are what the mute is for. Operator's bound 2026-08-30: 3-5 ms.
static constexpr int64_t UNMUTE_COMMON_US = 4000;
// Rate bound for delivering a render_align bias change as position (see ALIGN KICK). 10 ppm moves
// 10 us per second: inaudible as a rate, and an order of magnitude below the trim clamp.
static constexpr float ALIGN_KICK_MAX_PPM = 10.0f;
// After a position step or hard resync my measured render phase lags where the audio will be by the
// ring's travel time (~3.5 s measured, builds 51-55); for that long the beacon carries no phase.
static constexpr int64_t PHASE_TRANSIENT_US = 4000000;
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

#if defined(CLOCK_SYNC_TSF_ACTIVE) && defined(USE_SNAPCLIENT_TIMING_DIAG)
/// @brief Renders a render-phase value for a log line, printing the UNKNOWN sentinel as a word.
///
/// RENDER_PHASE_UNKNOWN is INT64_MIN. Printed as a number it reads as a measurement, and anything
/// that then differences it against a real phase gets 2^63 of overflow that looks like a finding --
/// which is exactly what happened while grading this signal on 2026-08-28.
static void format_render_phase_(char *buf, size_t len, int64_t phase_us) {
  if (phase_us == TsfSync::RENDER_PHASE_UNKNOWN) {
    snprintf(buf, len, "unknown");
  } else {
    snprintf(buf, len, "%" PRId64, phase_us);
  }
}
#endif  // CLOCK_SYNC_TSF_ACTIVE && USE_SNAPCLIENT_TIMING_DIAG

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
  if (this->config_.tsf_observer) {
    ESP_LOGI(TAG, "TSF observer mode: phase inputs logged");
  }
  if (this->config_.render_align_max_us > 0) {
    this->tune_align_max_us_.store(static_cast<int32_t>(this->config_.render_align_max_us), std::memory_order_relaxed);
  }
#endif

  this->control_session_ = std::make_unique<ControlSession>(this->config_.client_id);

  this->event_queue_ = xQueueCreate(8, sizeof(Event));
  this->record_queue_ = xQueueCreate(160, sizeof(ChunkRecord));
  if (this->event_queue_ == nullptr || this->record_queue_ == nullptr) {
    return false;
  }

  // PINNED TO CPU1, AWAY FROM WIFI AND THE MAIN LOOP. Both of those are on CPU0 in this build
  // (CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0, CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0), and the wifi
  // driver runs at priority 23 -- above every task here, including the speaker task at 19. Left
  // unpinned (xTaskCreate is tskNO_AFFINITY on ESP-IDF) these tasks can be scheduled onto CPU0
  // and preempted by a bursty radio, and can migrate between cores, which costs cache locality
  // on a path whose errors are measured in microseconds.
  //
  // The player outranks the network task so decode bursts cannot starve playout.
  if (xTaskCreatePinnedToCore(SnapcastClient::player_task_trampoline, "snap_player", 6144, this, 8,
                              &this->player_task_handle_, 1) != pdPASS) {
    return false;
  }
  if (xTaskCreatePinnedToCore(SnapcastClient::network_task_trampoline, "snap_net", 8192, this, 5,
                              &this->network_task_handle_, 1) != pdPASS) {
    return false;
  }
  return true;
}

// THREAD CONTEXT: Main loop (called from the hub's loop())
void SnapcastClient::loop() {
  // PLAYER-TASK WATCHDOG. THREAD CONTEXT: main loop -- deliberately, because it is the thread that
  // keeps running through every wedge (wifi_diag logs throughout), while the player task's own
  // stall reporting has failed three times over: every counter it keeps resets on partial
  // progress, so a loop that occasionally succeeds stays silent forever.
  //
  // Says only what it can know: how long since the player finished a chunk, and which phase it
  // stamped last. That is enough to separate the candidates that a silent task cannot -- waiting on
  // an empty record queue, waiting for a popped record's PCM, or writing into something that is not
  // draining.
  {
    const uint32_t progress = this->player_progress_.load(std::memory_order_relaxed);
    const int64_t now = now_us();
    if (progress != this->player_progress_seen_ || this->player_progress_at_us_ == 0) {
      this->player_progress_seen_ = progress;
      this->player_progress_at_us_ = now;
    } else if (this->stream_active_ && this->player_progress_seen_ > 0 &&
               // AND CHUNKS MUST ACTUALLY BE ARRIVING. Without this the watchdog fires forever
               // whenever the server's stream goes idle -- a paused Spotify stream had it logging
               // "no chunk completed for 751 s" at ERROR every 5 s, which is not a wedge, it is
               // nothing to play. The wedge always has a producer still delivering (the ring was
               // filling when it was caught), so requiring recent arrivals keeps the real case and
               // drops the idle one. Same trap PLAN records for diagnosing a silent board.
               now - this->last_chunk_us_.load(std::memory_order_relaxed) < PLAYER_STALL_US &&
               now - this->player_progress_at_us_ >= PLAYER_STALL_US &&
               now - this->player_stall_log_us_ >= PLAYER_STALL_US) {
      // progress_seen_ > 0 keeps startup quiet: the stream goes active before the pipeline is
      // feeding, so a fresh boot trips this once for ~7 s and a line that cries wolf at every boot
      // is one nobody reads. A wedge always has chunks behind it, so the real case still fires.
      this->player_stall_log_us_ = now;
      static const char *const PHASE_NAMES[] = {"idle(record queue)", "keepalive", "ring read",
                                                "servo", "write", "discard"};
      const uint8_t phase = this->player_phase_.load(std::memory_order_relaxed);
      // HEAP, because the wedge is not confined to audio. A wedged board answers no ping, no API
      // (6053), no OTA (3232) and no mDNS, while its main loop keeps logging and its radio still
      // reports RSSI -- and ARP still resolves it, so it is associated but its IP stack is not
      // serving. One cause explains all of that at once: an exhausted heap. The mixer cannot
      // allocate its ring buffer on start, sockets cannot be accepted, mDNS cannot answer, and the
      // main loop carries on because it allocates nothing.
      // TWO LINES, because the one line was 300+ bytes and the 256-byte formatting ceiling cut it
      // at "output_active=0, he" on every 2026-08-30 07:53 occurrence -- records=, the field the
      // comment below calls decisive, was never printed (see CLAUDE.md: no load-bearing line may
      // have a variable-length tail).
      ESP_LOGE(TAG, "PLAYER STALLED: no chunk completed for %" PRId64 " s, phase=%s, ring=%zu bytes, records=%u, "
                    "output_active=%d, stream_active=%d, iters=+%" PRIu32,
               (now - this->player_progress_at_us_) / 1000000,
               phase < (sizeof(PHASE_NAMES) / sizeof(PHASE_NAMES[0])) ? PHASE_NAMES[phase] : "?",
               this->pcm_ring_ != nullptr ? this->pcm_ring_->available() : 0,
               // THE decisive field. A full ring with an EMPTY queue means PCM nobody has a record
               // for; a full ring with records waiting means the player is not consuming them, and
               // those are opposite bugs. Everything so far has been inferred from which of the two
               // is true, and it was never actually measured.
               this->record_queue_ != nullptr ? static_cast<unsigned>(uxQueueMessagesWaiting(this->record_queue_)) : 0u,
               this->output_active_.load(std::memory_order_relaxed) ? 1 : 0, this->stream_active_ ? 1 : 0,
               // Iterations since the last report. Zero means the task is genuinely blocked;
               // non-zero means it is running and taking a path that never completes a chunk,
               // which the phase stamp cannot show because every such path re-stamps IDLE.
               this->player_iters_.load(std::memory_order_relaxed) - this->player_iters_seen_);
      ESP_LOGE(TAG,
               "PLAYER STALLED heap: free=%" PRIu32 " largest=%" PRIu32 " min_ever=%" PRIu32
               " -- audio is not being written",
               static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
               static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
      this->player_iters_seen_ = this->player_iters_.load(std::memory_order_relaxed);
    }
  }

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

  // Stream IDENTITY, taken separately from metadata because it is known whenever the server
  // resolves us into a group, while metadata is optional -- see take_stream_identity(). This is
  // what scopes the TSF group, so leaving it to a callback a process stream never fires meant the
  // scope silently stayed "unknown" and the group accepted phases from other streams.
  if (this->control_session_ != nullptr) {
    std::string stream_identity;
    if (this->control_session_->take_stream_identity(stream_identity)) {
      this->set_stream_identity(stream_identity);
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
//
// The captured half of the playout picture. notify_audio_played() says HOW MUCH rendered and when;
// this says WHICH audio did. Only the second can see a bias in our own frame ledger, because it does
// not consult that ledger: `pushed` and `played` appear nowhere in it.
//
// Cheap on purpose -- it runs at the DMA cadence (~100/s) on the speaker's callback thread, so it
// only records the newest observation and lets the 3.3 s report do the arithmetic.
void SnapcastClient::notify_audio_played_tagged(uint32_t frames, int64_t adjusted_ts, const audio::RenderTag &tag) {
  if (frames == 0 || !tag.valid()) {
    // Not an error: audio we inserted ourselves (silence, splices, repeated frames) and audio blended
    // with an announcement both arrive untagged by design. There is simply nothing to measure.
    return;
  }
  const uint32_t rate = this->tag_sample_rate_.load(std::memory_order_relaxed);
  if (rate == 0) {
    return;
  }
  // The transport delay for THIS observation: when the tagged frame rendered, minus the server time
  // it belongs to. Computed here rather than at the report so every arrival contributes, instead of
  // the report seeing only whichever one happened to be last.
  const int64_t rate_i = static_cast<int64_t>(rate);
  const int64_t first_frame_local = adjusted_ts - static_cast<int64_t>(frames) * 1000000 / rate_i;
  const int64_t frame_server_us =
      static_cast<int64_t>(tag.server_ts) + static_cast<int64_t>(tag.offset_frames) * 1000000 / rate_i;
  // RAW delay: local render instant minus server time. Carries the local-vs-server CLOCK OFFSET,
  // which is unbounded and drifts at the crystal difference -- measured walking ~90 ppm, i.e. ~300 us
  // per 3.35 s report. That term dominated the first block sweep and made it unreadable: a linear
  // ramp inside every window gives block means that spread the same at every width, which was
  // misread as "no measurement noise". Kept only for the mean, where the drift is the point.
  const double delay_us = static_cast<double>(first_frame_local - frame_server_us);
  // DEADLINE-CORRECTED: the same subtraction the servo's target uses, so the clock offset cancels
  // and what is left is the render error itself. This is the quantity whose precision decides
  // whether a tag-derived signal can close a loop -- accuracy is already proven (inject_split moved
  // it 1.02-1.05 of the truth while the ledger-based error, servo-nulled, could not see it at all).
  //
  // deadline() is linear in server time for fixed buffer and offset, so the tagged frame's target is
  // the last chunk's target plus their server-time difference. Zero until the first chunk of a
  // session sets the anchor, and skipped rather than fed a bogus zero.
  this->playout_mutex_.lock();
  const bool err_tag_valid = this->tag_anchor_server_ts_ != 0;
  const double err_tag_us =
      err_tag_valid ? static_cast<double>(first_frame_local - (this->tag_anchor_deadline_us_ +
                                                              (frame_server_us - this->tag_anchor_server_ts_)))
                    : 0.0;
  this->tagged_render_ = TaggedRender{adjusted_ts, frames, static_cast<int64_t>(tag.server_ts), tag.offset_frames, rate};
  this->tagged_render_count_++;
  // DELAY LOOP control block. Gated on the blanking instant by the RENDER time of the observation,
  // not by wall clock at arrival -- the point of the blank is "this audio was scheduled against the
  // old deadline", which is a property of when it rendered.
  if (err_tag_valid && first_frame_local >= this->dl_blank_until_us_) {
    if (this->dl_acc_n_ == 0) {
      this->dl_acc_first_us_ = adjusted_ts;
    }
    this->dl_acc_sum_us_ += err_tag_us;
    this->dl_acc_n_++;
    this->dl_acc_last_us_ = adjusted_ts;
  }
  // Welford: mean and M2 without retaining samples.
  this->delay_n_++;
  const double d1 = delay_us - this->delay_mean_us_;
  this->delay_mean_us_ += d1 / static_cast<double>(this->delay_n_);
  this->delay_m2_us_ += d1 * (delay_us - this->delay_mean_us_);
  // Block-means sweep; see delay_blocks_. Each level accumulates B consecutive arrivals, and on
  // completion folds that block's MEAN into a Welford over block means.
  for (size_t lvl = 0; lvl < DELAY_BLOCK_LEVELS && err_tag_valid; lvl++) {
    DelayBlock &blk = this->delay_blocks_[lvl];
    blk.sum += err_tag_us;
    blk.fill++;
    const uint32_t width = 1u << lvl;
    if (blk.fill >= width) {
      const double bmean = blk.sum / static_cast<double>(width);
      blk.n++;
      const double db = bmean - blk.mean;
      blk.mean += db / static_cast<double>(blk.n);
      blk.m2 += db * (bmean - blk.mean);
      blk.sum = 0.0;
      blk.fill = 0;
    }
  }
  this->playout_mutex_.unlock();
}

// THREAD CONTEXT: Speaker playback callback thread
void SnapcastClient::notify_audio_played(uint32_t frames, int64_t timestamp_us) {
#ifdef USE_I2S_RATE_LOCK
  // Rate-lock dither step at the DMA cadence; see RateLock::tick(). A no-op unless the
  // requested trim falls between two achievable divider ratios.
  if (this->rate_lock_ != nullptr) {
    this->rate_lock_->tick();
  }
#endif
  bool rebaselined = false;
  this->playout_mutex_.lock();
  if (this->playout_valid_) {
    // A gap well beyond the speaker's DMA cadence means the DAC was starved
    // (pipeline underrun); surfaced in the periodic sync report for diagnostics
    const int64_t gap = timestamp_us - this->played_last_ts_us_;
    if (gap > static_cast<int64_t>(this->tune_gap_blank_ms_.load(std::memory_order_relaxed)) * 1000) {
      // Late-stamped catch-up burst incoming (see FEEDBACK_GAP_BLANK_US): every tagged arrival
      // for the next stretch carries a render instant off by up to the stall. Already under
      // playout_mutex_ here, so write the blank directly rather than through mark_kp_event_.
      this->dl_blank_until_us_ =
          timestamp_us + static_cast<int64_t>(this->tune_blank_ms_.load(std::memory_order_relaxed)) * 1000;
      this->dl_acc_n_ = 0;
      this->dl_acc_sum_us_ = 0.0;
    }
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
  this->last_rx_us_ = now_us();  // silence is measured from the connect, not from the last session
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
          const int32_t prev_buffer = this->buffer_ms_.exchange(settings.buffer_ms, std::memory_order_relaxed);
          const int32_t prev_latency = this->server_latency_ms_.exchange(settings.latency, std::memory_order_relaxed);
          if (prev_buffer != settings.buffer_ms || prev_latency != settings.latency) {
            // SETPOINT CHANGE: both step deadline(), so err_tag steps with them -- but the audio
            // already in flight was scheduled against the OLD deadline, and its tags would carry
            // the step a second time in the opposite direction. Blank the tag stream for one
            // pipeline depth and discard the partial block; the fast path splices the step away
            // (it is the same fast_splice_threshold rule as any other error, no special case) and
            // the loop resumes on post-change audio.
            this->playout_mutex_.lock();
            this->dl_blank_until_us_ =
                now_us() + static_cast<int64_t>(this->tune_blank_ms_.load(std::memory_order_relaxed)) * 1000;
            this->dl_acc_n_ = 0;
            this->dl_acc_sum_us_ = 0.0;
            this->playout_mutex_.unlock();
            ESP_LOGD(TAG, "Delay loop: setpoint changed (buffer %" PRId32 "->%" PRId32 " ms, latency %" PRId32
                          "->%" PRId32 " ms), tag stream blanked %" PRId64 " ms t=%" PRId64,
                     prev_buffer, settings.buffer_ms, prev_latency, settings.latency,
                     static_cast<int64_t>(this->tune_blank_ms_.load(std::memory_order_relaxed)), now_us());
          }
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
      // DEAD SESSION. A server that drops the session without a FIN leaves this socket open and
      // silent: no chunks, no Time replies, and nothing here ever returned false -- both boards sat
      // with empty rings for 4+ minutes on 2026-08-29 15:07-15:12 ("no chunk completed for 245 s")
      // while the server listed them disconnected and MLS44 kept playing. The late-stream bailout
      // cannot fire without chunks to be late. Time replies arrive every second while streaming
      // and every few seconds idle, so SESSION_SILENCE_US of nothing at all is a dead peer.
      if (this->last_rx_us_ != 0 && now_us() - this->last_rx_us_ > SESSION_SILENCE_US) {
        ESP_LOGW(TAG, "Server silent for %" PRId64 " s (no message of any kind): reconnecting",
                 (now_us() - this->last_rx_us_) / 1000000);
        this->last_rx_us_ = 0;
        return false;
      }
      continue;
    }
    int n = recv(this->sock_, buf + got, len - got, 0);
    if (n <= 0) {
      return false;
    }
    this->last_rx_us_ = now_us();
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
    // Beacons only while a stream is active: that is when deadlines are computed AND
    // when the hub holds high-performance wifi. While idle, modem power save makes
    // TSF reads fail intermittently (observed: sporadic beacons and "TSF unreadable"
    // on an idle pair). Everyone resumes beaconing on the first active tick, and
    // stale mappings expire into the Kalman fallback on their own.
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
      this->tsf_sync_->service(now, est, this->server_id_hash_,
                               this->stream_id_hash_.load(std::memory_order_relaxed));
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
  // Write the PCM first, then post the record: the player may then rely on a popped record's
  // bytes being fully present in the ring. A full ring blocks here, which backpressures the TCP
  // connection exactly like a desktop snapclient.
  //
  // BUT WAIT FOR ROOM FOR THE WHOLE CHUNK BEFORE WRITING ANY OF IT. Writing into a nearly-full
  // ring strands PARTIAL PCM that no record describes, and that is a deadlock, not a delay: the
  // player drains the ring only by consuming records, so it waits forever for a record that this
  // function will never post, while this function waits forever for space only the player can
  // free. Neither side is doing anything wrong, which is why nothing could see it -- the player
  // is legitimately idle on an empty queue and the network task is legitimately writing.
  //
  // Diagnosed 2026-08-27 01:11 after being reproduced on demand: "PLAYER STALLED: no chunk
  // completed for 10 s, phase=idle(record queue), ring=524288 bytes, output_active=1" -- a ring at
  // exactly buffer_size with an empty record queue, on a healthy heap. It is the wedge that has
  // needed a replug all day, and it takes the network with it (no ping, no API, no OTA, no mDNS)
  // because this is the network task, blocked here, that serves everything else too.
  //
  // Waiting for the space first is enough: either the whole chunk fits and its record follows, or
  // nothing is written and the player still has records for every byte in the ring, so it drains
  // and space appears. There is no state in between for a deadlock to live in.
  // ... AND NEVER WAIT FOREVER. 2026-08-30 07:53: after a 40 s server outage and a dead-session
  // reconnect, both speakers sat with ring=520704 (one chunk short of full), the player producing no
  // line for minutes, the mixer never restarted, and this loop holding the network task -- no ping,
  // no API, no OTA, replug only. Whatever wedged the player, the network task must outlive it: after
  // EMIT_ROOM_WAIT_MAX_US the chunk is dropped (the audio is late by then anyway) and the session
  // keeps serving time sync and control, so the stall detector and the dead-session/bailout paths
  // can still act. Logged once per episode.
  const int64_t room_wait_start = now_us();
  while (this->pcm_ring_->free() < len && !this->shutdown_.load(std::memory_order_relaxed)) {
    if (!this->output_active_.load(std::memory_order_relaxed) &&
        !this->stream_active_) {
      return;  // nothing is going to drain this; drop rather than hold the network task
    }
    if (now_us() - room_wait_start > EMIT_ROOM_WAIT_MAX_US) {
      if (!this->emit_room_wait_logged_) {
        this->emit_room_wait_logged_ = true;
        ESP_LOGE(TAG,
                 "emit_pcm_: no ring room for %zu bytes after %" PRId64 " ms (ring %zu/%zu, records queued %u, "
                 "output_active=%d) -- dropping chunks, not the network task",
                 len, EMIT_ROOM_WAIT_MAX_US / 1000, this->pcm_ring_->available(), static_cast<size_t>(this->config_.buffer_size),
                 static_cast<unsigned>(uxQueueMessagesWaiting(this->record_queue_)),
                 this->output_active_.load(std::memory_order_relaxed) ? 1 : 0);
      }
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  this->emit_room_wait_logged_ = false;
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
  // CRYSTAL OFFSET PERSISTENCE. The integral is a property of the hardware, not the session, so a
  // cold boot may start from the last learned value instead of re-learning over ~90 s -- which
  // also removes the cold-boot wind-up transient entirely (the plan's deferred work item).
  // Guarded on load: a corrupt or foreign value inside the clamp is indistinguishable from a
  // learned one, but outside it is refused.
  this->dl_integral_pref_ = global_preferences->make_preference<float>(fnv1_hash("snapclient_dl_integral"));
  {
    float saved = 0.0f;
    if (this->dl_integral_pref_.load(&saved) && std::isfinite(saved) && std::abs(saved) <= TRIM_CLAMP_MAX_PPM) {
      st.trim_integral_ppm = saved;
      st.dl_saved_integral_ppm = saved;
      st.dl_integral_ema_ppm = saved;
      st.dl_integral_ema_valid = true;
      st.dl_cold_start = false;
      ESP_LOGI(TAG, "Delay loop: integral restored %+.2f ppm from NVS", saved);
    }
  }
#endif
  // A boot is a disturbance like any other, and the largest one measured: the re-baseline anchor
  // starts a power cycle ~620 us out. Stamped here rather than left at 0 so the first convergence
  // hands off to a decaying gain instead of dropping straight to RUN.
  this->mark_kp_event_(st, "boot");
  while (!this->shutdown_.load(std::memory_order_relaxed)) {
    ChunkRecord rec;
    this->player_iters_.fetch_add(1, std::memory_order_relaxed);
    this->player_phase_.store(static_cast<uint8_t>(PlayerPhase::IDLE), std::memory_order_relaxed);
    // MUTEX PROBE. The player blocks somewhere between this stamp and the next one, with records
    // waiting and iters going 2565 -> 0: it spins through paths that need nothing, then stops dead
    // on the first that does. The seqlock depth reader is bounded by construction (4 tries, then
    // failure), so the remaining shared resource on that stretch is playout_mutex_ -- which the
    // SPEAKER CALLBACK also takes, and which stays locked forever if that task is deleted inside
    // notify_audio_played. Probing it here, before the servo has any reason to want it, turns
    // "blocked somewhere" into a named answer.
    if (!this->playout_mutex_.try_lock()) {
      const int64_t probe_start = now_us();
      bool got = false;
      while (now_us() - probe_start < 2000000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (this->playout_mutex_.try_lock()) {
          got = true;
          break;
        }
      }
      if (!got) {
        ESP_LOGE(TAG, "playout_mutex_ HELD BY SOMEONE ELSE for >2 s -- the player is about to block on it "
                      "(this is the wedge; the holder is a task that died mid-section) t=%" PRId64,
                 now_us());
      } else {
        this->playout_mutex_.unlock();
      }
    } else {
      this->playout_mutex_.unlock();
    }
    if (xQueueReceive(this->record_queue_, &rec, pdMS_TO_TICKS(100)) != pdTRUE) {
      // No record. Normal between chunks; a WEDGE if it lasts, and this branch logged nothing at
      // all, which is why a wedged player task looked identical to a dead one. Throttled, and it
      // reports the ring so a "no records but a full ring" deadlock -- emit_pcm_ blocked writing
      // PCM it has not posted a record for, while the player can only drain via records -- is
      // visible as itself rather than as silence.
      if (st.no_record_since_us == 0) {
        st.no_record_since_us = now_us();
      } else if (now_us() - st.no_record_since_us >= PUSH_STALL_LOG_US) {
        st.no_record_since_us = now_us();
        ESP_LOGW(TAG, "player: no chunk records for %" PRId64 " s, ring holds %zu bytes, output_active=%d",
                 PUSH_STALL_LOG_US / 1000000, this->pcm_ring_->available(),
                 this->output_active_.load(std::memory_order_relaxed) ? 1 : 0);
      }
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
        // Stamped separately from the queue wait above it: both are "idle" to a reader, but one is
        // waiting for work and the other is pushing silence into the pipeline, and a wedge in the
        // second looks exactly like patience in the first.
        this->player_phase_.store(static_cast<uint8_t>(PlayerPhase::KEEPALIVE), std::memory_order_relaxed);
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
    // COARSE DECISIONS ON THE MEASURED ERROR WHILE TAGS ARE LIVE. The prediction is built from the
    // ledger; repaired, the repair moves real audio (-29/+29 ms pairs, 21:37), unrepaired, its bias
    // rides the common wander into the 50 ms hard-resync threshold (three 50-53 ms resyncs on A in
    // ten minutes, 21:47-21:51, each dropping real audio that err_tag then spliced back). Neither
    // is acceptable; the measured error is the only safe basis for a coarse decision when it
    // exists. It lags one block plus one pipeline depth, so a tag-driven correction may not be
    // REPEATED until the tags have had time to see it -- the same blank interval the setpoint
    // path uses. The prediction remains the fallback when tags are stale.
    //
    // TAG FAULT. A correction made on err_tag must move err_tag once the tags post-date it; if it
    // does not, the measurement is disconnected from the audio and acting on it again is harm
    // (B: a hard resync every ~20 s for 40 minutes on a constant +97 ms, 2026-08-29 12:38-13:17).
    // The first block after the guard interval judges the last tag-driven correction: |err_tag|
    // still >= 75% of what it was = a miss. Three in a row fault the tag path for TAG_FAULT_US,
    // during which every consumer of "tags live" falls back to the ledger.
    const bool tags_fresh = st.dl_have_err && now_us() - st.dl_err_at_us < DL_ERR_STALE_US;
    // In the resync window (see ServoState::post_event_until_us) the coarse path may act again as
    // soon as tags post-date the previous action by resync_blank_ms, and with a 4x step: a 300 ms
    // injected starvation left 4.8 ms that took 4 s to walk down at one bounded action per 500 ms.
    const bool resync_window = now_us() < st.post_event_until_us;
    int64_t blank_us =
        // In the window the blank IS resync_blank_ms (pipeline ~250 ms + one block ~650 ms), not the
        // smaller of the two: min(500, 1200) = 500 ms let the second and third steps of every build-48
        // sequence act on blocks that predated the first (+2277 applied, next block read +4068).
        static_cast<int64_t>(resync_window ? this->tune_resync_blank_ms_.load(std::memory_order_relaxed)
                                           : this->tune_blank_ms_.load(std::memory_order_relaxed)) *
        1000;
    // IN THE WINDOW THE BLANK IS AT LEAST THE STEP'S OWN VISIBILITY HORIZON: ring (the step is taken
    // from the chunk entering it, ~1.7 s ahead) + pipeline (~250 ms) + one block (~650 ms), so the block
    // that judges a step wholly post-dates it. Measured on the wire and on RSTEP (01:22-01:57, builds
    // 51-53): a step reaches the tags 2.6-3 s after it is applied; every shorter blank judged the next
    // block on stale audio -- doubled steps (48-50), half-realised steps (51), and an in-flight
    // subtraction (52-53) whose binary aging could not follow a step that lands over a whole block.
    // Computed from the live ring depth, not tuned: a deeper server buffer moves it by itself.
    if (resync_window && frame_bytes > 0 && rec.params.sample_rate > 0) {
      const int64_t ring_us_now = static_cast<int64_t>(
          static_cast<uint64_t>(this->pcm_ring_->available()) * 1000000ULL / (frame_bytes * rec.params.sample_rate));
      // TWO blocks, not one: build 54 (ring + pipeline + one block = 2.6 s) still judged on a block whose
      // mean straddled the landing -- tag error flat at +5.5 ms for the whole 2.6 s after a +3.0 ms
      // ledger step, dropping 3.4 s after it (02:03:35-39). A block mean is wholly post-step only once
      // a full block has elapsed after the landing.
      const int64_t horizon_now =
          ring_us_now + st.pipe_depth_frames * 1000000 / static_cast<int64_t>(rec.params.sample_rate) +
          2 * static_cast<int64_t>(this->tune_block_n_.load(std::memory_order_relaxed)) * DL_ARRIVAL_US;
      blank_us = std::max(blank_us, horizon_now);
    }
    // JUDGE ONLY AFTER THE MEASUREMENT CAN SHOW THE EFFECT. The action cadence (blank_us, 200 ms in
    // the resync window) is not the measurement lag (pipeline ~280 ms + one block ~650 ms): build 32
    // judged three actions inside one lag and faulted BOTH boards 20 s after boot on the normal
    // post-boot tag/ledger settling (+3.1 vs -0.9 ms), reconnecting them for nothing. Judge at
    // max(blank, TAG_JUDGE_US), and not at all in the first TAG_SETTLE_US after engage.
    const bool settled = st.dl_engaged_since_us != 0 && now_us() - st.dl_engaged_since_us > TAG_SETTLE_US;
    if (tags_fresh && st.coarse_act_err_us != 0 &&
        st.dl_err_at_us > st.coarse_act_us + std::max<int64_t>(blank_us, TAG_JUDGE_US)) {
      if (!settled) {
        st.coarse_act_err_us = 0;
        st.tag_miss = 0;
      }
    }
    if (tags_fresh && st.coarse_act_err_us != 0 && settled &&
        st.dl_err_at_us > st.coarse_act_us + std::max<int64_t>(blank_us, TAG_JUDGE_US)) {
      const int64_t before = std::abs(st.coarse_act_err_us);
      st.coarse_act_err_us = 0;
      // A miss counts only while the tags and the ledger DISAGREE: on B at 14:37:54 both read
      // +47.5 ms after a starvation (just under the 50 ms hard-resync threshold) and the bounded
      // catch-up simply needed ~15 s -- three "misses" faulted a healthy tag path and, on build
      // 24, forced a needless reconnect. Agreement means the measurement is fine and the coarse
      // path is merely slow; disagreement is the fault this exists for.
      const bool disagree = std::abs(st.dl_err_us - error_us) > TAG_SPLIT_US;
      if (std::abs(st.dl_err_us) >= before - before / 4 && disagree) {
        if (++st.tag_miss >= TAG_FAULT_MISSES) {
          st.tag_miss = 0;
          st.tag_fault_until_us = now_us() + TAG_FAULT_US;
          ESP_LOGW(TAG,
                   "TAGFAULT: %u tag-driven corrections left err_tag at %+" PRId64
                   " us (ledger says %+" PRId64 ") -- distrusting tags for %" PRId64 " s and reconnecting",
                   static_cast<unsigned>(TAG_FAULT_MISSES), st.dl_err_us, error_us, TAG_FAULT_US / 1000000);
          // REPAIR, NOT JUST DIAGNOSIS. What closed the 15 ms split on A at 14:33:41 was the
          // accounting-split repair, which the fault had re-armed: the LEDGER had slipped (RECON drift
          // +14988 the whole time), the tags were right, and the tag-driven catch-up's frame drops
          // were being absorbed by the split rather than moving the audio. Pre-arm the repair so it
          // runs on the next sample instead of after a fresh 3-s window; the fault window itself is
          // 180 s so it cannot expire underneath it (60 s did, four times).
          st.drift_excess_since_us = now_us() - DRIFT_REPAIR_HOLD_US;
          // RECONNECT NOW. Waiting for the repair or a second fault cost three minutes of audible
          // desync on A and the observer (16:38:26 -> 16:41:30): the repair's drift median spans
          // DRIFT_WINDOW samples at the 20-s RECON cadence, right for a slow accounting drift and
          // structurally too slow for an acute fault. A genuine fault (tag/ledger disagreement,
          // corrections not moving the measurement) has one proven remedy -- the session teardown
          // rebuilds the pipeline and its tag tracks, and every observed reconnect came back with
          // tags and ledger agreeing within tens of us (B 14:11:53: SHADOW diff -32). ~3 s gap.
          st.tag_fault_streak = 0;
          this->reconnect_requested_.store(true, std::memory_order_relaxed);
        }
      } else {
        st.tag_miss = 0;
      }
    }
    const bool coarse_on_tags = tags_fresh && now_us() >= st.tag_fault_until_us;
    const int64_t coarse_err_us = coarse_on_tags ? st.dl_err_us : error_us;
    const bool coarse_ok =
        !coarse_on_tags || st.coarse_act_us == 0 || st.dl_err_at_us > st.coarse_act_us + blank_us;
    // In the window, an error above the arm that takes NO step is the case that needs explaining
    // (build 44, B at +600 us for 45 s). One line per block, never per chunk: ~38 lines/s crashes
    // the ESPHome logger's ring buffer.
    if (resync_window && coarse_on_tags && st.dl_err_at_us != st.rskip_log_at_us &&
        std::abs(coarse_err_us) > this->tune_resync_splice_us_.load(std::memory_order_relaxed) &&
        (!coarse_ok || st.dl_err_at_us == st.resync_last_block_us)) {
      st.rskip_log_at_us = st.dl_err_at_us;
      ESP_LOGD(TAG, "RSKIP err=%+" PRId64 " ok=%d sameblk=%d since_act=%" PRId64 " ms blank=%" PRId64 " ms",
               coarse_err_us, coarse_ok ? 1 : 0, st.dl_err_at_us == st.resync_last_block_us ? 1 : 0,
               st.coarse_act_us ? (st.dl_err_at_us - st.coarse_act_us) / 1000 : -1, blank_us / 1000);
    }
    // Anchor for the SHADOW error (see the SHADOW log line). Stored, not recomputed.
    st.last_deadline_us = deadline;
    st.last_deadline_server_ts = rec.server_ts_us;
    // Published for the speaker callback, which cannot see this player-task local.
    this->playout_mutex_.lock();
    this->tag_anchor_deadline_us_ = deadline;
    this->tag_anchor_server_ts_ = rec.server_ts_us;
    this->playout_mutex_.unlock();

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
        // VERBOSE, not DEBUG: 38 lines/s from the player task. Two boards crashed inside 80 min on
    // 2026-08-29 (observer 19:11:49, A 20:34:51) in ESPHome's thread-safe logger buffer
    // (Logger::log_vprintf_ -> TaskLogBuffer::send_message_thread_safe -> xRingbufferSendComplete
    // assert, ringbuf.c:374) under this volume. Nothing on the bench parses RAW (RPRE is separate).
    ESP_LOGV(TAG,
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
    if (std::abs(coarse_err_us) > hard_us && coarse_ok) {
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
    // A TIMEBASE RE-ANCHOR steps the clock the deadline is computed against, so whatever the servo
    // had converged to is now measured against a different one. Checked per chunk (an atomic load)
    // rather than at report cadence, because a 3.3 s delay would spend most of the decay before
    // the schedule noticed the event.
    if (this->tsf_sync_ != nullptr) {
      const uint32_t epoch_now = this->tsf_sync_->timebase_epoch();
      if (st.kp_last_epoch != UINT32_MAX && epoch_now != st.kp_last_epoch) {
        this->mark_kp_event_(st, "timebase re-anchor");
      }
      st.kp_last_epoch = epoch_now;
    }
    // A DEADLINE SOURCE SWITCH (shared mapping <-> local Kalman) is the same event in different
    // clothes: the deadline steps by however far the two mappings disagree (29 ms measured), and
    // no consensus machinery announces it because no consensus moved. Same response.
    if (this->deadline_source_switched_) {
      this->deadline_source_switched_ = false;
      // Only once converged: during acquisition the coarse machinery owns placement and re-places
      // on the new deadline anyway, while the blank only delays the first engage -- measured six
      // source flaps in two seconds on a boot, each restarting the tag stream.
      if (st.converged) {
        // Blank only -- no gain re-arm. When the two mappings agree to sub-ms (the normal case,
        // consensus spread 400-900 us measured) the step is tiny, and re-arming the schedule on
        // every flap is what wound the integral 4x too fast (see the flat-gain note in
        // delay_loop_update_). A genuinely large disagreement is announced by the out-of-range
        // hold and corrected by the fast path regardless.
        this->playout_mutex_.lock();
        this->dl_blank_until_us_ =
            now_us() + static_cast<int64_t>(this->tune_blank_ms_.load(std::memory_order_relaxed)) * 1000;
        this->dl_acc_n_ = 0;
        this->dl_acc_sum_us_ = 0.0;
        this->playout_mutex_.unlock();
      }
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
    if (coarse_err_us > hard_us && coarse_ok) {
      if (coarse_on_tags) {
        st.coarse_act_us = now_us();
        st.coarse_act_err_us = coarse_err_us;
      }
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
    if (coarse_err_us < -hard_us && coarse_ok) {
      if (coarse_on_tags) {
        st.coarse_act_us = now_us();
        st.coarse_act_err_us = coarse_err_us;
      }
      // Hard resync, early: fill the gap with silence (bounded per chunk so the
      // loop stays responsive), keeping the DAC fed and continuous
      const int64_t gap_frames = (-coarse_err_us) * rec.params.sample_rate / 1000000;
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
    } else if (std::abs(coarse_on_tags ? coarse_err_us : median_err_us) >
                   (resync_window
                        // In the window BOTH sources arm at resync_splice_us. Right after a hard resync
                        // the tags are blanked, but the LEDGER knows exactly how many chunks were dropped,
                        // so the sub-chunk residual is computable at t+0 instead of a block later; the
                        // tag-based steps that follow verify and correct it (step-and-verify).
                        ? static_cast<int64_t>(this->tune_resync_splice_us_.load(std::memory_order_relaxed))
                        : (coarse_on_tags ? static_cast<int64_t>(FAST_SPLICE_MAX_FRAMES) * 1000000 /
                                                static_cast<int64_t>(rec.params.sample_rate)
                                          : SOFT_CORRECTION_AGGRESSIVE_US)) &&
               coarse_ok && (!resync_window || !coarse_on_tags || st.dl_err_at_us != st.resync_last_block_us) &&
               // A LEDGER step in the window waits out the blank too. Build 45's RSTEP showed the
               // ledger path re-stepping the same error EVERY CHUNK -- err -1475, -3470, -5465,
               // -7461, -9454 within one second (00:14:58) -- because the prediction cannot show a
               // step until the pipeline has played through it; five steps landed on one error and
               // it came back +26272. The tag path had one-block-one-step; the ledger had nothing.
               (!resync_window || coarse_on_tags || st.resync_step_at_us == 0 ||
                now_us() - st.resync_step_at_us >= blank_us) &&
               // THE LEDGER TAKES ONLY THE FIRST STEP OF A WINDOW. Build 46 boot (00:27:50): tag step
               // -2680, ledger +3592, tag -2732, ledger +3656 ... a +-4.5 ms limit cycle at 1 Hz. Build 47
               // gated the ledger on "tags not fresh" and cycled identically (00:39:34): every tag step
               // blanks the tags for a moment, the ledger steps in that moment, the tags come back and
               // step on the ledger's step. Freshness flickers; "has anything stepped in this window"
               // does not. resync_step_at_us is zeroed wherever the window opens.
               (!resync_window || coarse_on_tags || st.resync_step_at_us == 0)) {
      // In the window a tag-based step needs the error to have PERSISTED across a block boundary:
      // the block used for the previous decision may not be used again (one block, one step), and
      // with the arm at 100 us this is what keeps the +-60 us block noise from being stepped on.
      if (resync_window && coarse_on_tags) st.resync_last_block_us = st.dl_err_at_us;
      // On the MEASURED error the catch-up threshold is the fast splice's own 128-frame bound
      // (~2.9 ms), not 10 ms. Between the two nothing corrected: the splice episode hit its bound
      // "with -8 ms still standing", declared a measurement fault (a rule written for the
      // prediction), stood down 4 s, re-armed, and repeated -- B sat 8-10 ms early for 5+ minutes
      // (2026-08-29 11:20-11:25) while the analyser found no shared audio inside its window.
      // A LEDGER step in the window blanks the tag path too. Build 50: hard resync, ledger step
      // +1220 at t+1.8 s, tag step +1643 at t+2.7 s on a block that predated the ledger step (blank
      // 1200 ms was only applied between TAG steps), then two corrective steps of the other sign.
      if (coarse_on_tags || resync_window) {
        st.coarse_act_us = now_us();
      }
      if (coarse_on_tags) {
        st.coarse_act_err_us = coarse_err_us;
      }
      // Post-stall catch-up: frames/32 bursts (~33 ms/s convergence) so a backlog
      // doesn't leave playback audibly behind for long
      // DAMPED in the resync window: the block error lags the audio by a block, so correcting all
      // of it each step rang (build 34, 300 ms injection on A: -4717 -> +4125 -> -1251 -> +740 us).
      // Correcting resync_gain (60 %) of it converges in three steps without the overshoot.
      int64_t coarse_target_us = coarse_on_tags ? coarse_err_us : median_err_us;
      // SUBTRACT THE STEPS STILL IN FLIGHT. Measured 01:23-01:29 (build 51, wire vs RSTEP): every step
      // arrives on the wire 1:1 -- but ~2 s after it is applied, because a drop is taken at PUSH time
      // and the ring holds ~1.7 s ahead of the DAC. A 1200 ms blank therefore judged the next block
      // BEFORE the step existed there (builds 48-50: +2277 applied, next block +4068, stepped again);
      // 2000 ms saw about half of it. The fast splice has carried this accounting since build 3x
      // (splice_hist / horizon_chunks); the coarse target now does too: pending = steps applied within
      // ring depth + one block, and the target is what the tags will read once they have landed.
      int64_t pending_us = 0;
      if (resync_window && coarse_on_tags) {
        // The step is taken from the chunk entering the RING, which holds ~1.7 s ahead of the push
        // into the pipeline (~250 ms), and the block that reads it is ~650 ms long: ring + pipeline
        // + block. Build 52 used pipeline + block alone and pend= read 0 at every decision.
        const int64_t horizon_us =
            static_cast<int64_t>(ring_ms) * 1000 +
            st.pipe_depth_frames * 1000000 / static_cast<int64_t>(rec.params.sample_rate) +
            static_cast<int64_t>(this->tune_block_n_.load(std::memory_order_relaxed)) * DL_ARRIVAL_US;
        const int64_t now_p = now_us();
        for (size_t i = 0; i < ServoState::WIN_STEPS; i++) {
          if (st.win_step_at_us[i] != 0 && now_p - st.win_step_at_us[i] < horizon_us) {
            pending_us += st.win_step_us[i];
          }
        }
        coarse_target_us -= pending_us;
      }
      // A ONE-BOARD POSITION STEP ON A COMMON ERROR IS A DIFFERENTIAL ERROR. After a boot into a
      // running group err_tag is mostly the +-150 us common deadline wander, and A's window steps on
      // it produced a +-260..500 us sawtooth on the wire (build 41, 23:15-23:19) while B, just under
      // the arm, did nothing. Errors above resync_local_us are local by construction (the wander
      // never reaches them) and step on err_tag; below it the step needs the GROUP render delta --
      // the on-device differential measurement -- to agree in sign, and moves by the smaller of the
      // two. SIGNS, from the definitions: phase = TSF(render) - server_time, larger = rendered LATER;
      // group delta = mine - mean(peers), so delta > 0 = LATE. err_tag = render - deadline, > 0 = LATE.
      // Same sign = agreement. Build 43 had this test inverted (its comment said "delta > 0 = early",
      // a label inherited from the wire's B-A header while the analyser's probe b sat on board A):
      // it refused every step on which the two agreed -- B sat at -380 us for 60 s after a 300 ms
      // injection (23:32:14) with err_tag -380 / group delta -532, both saying EARLY, and the PI
      // alone closed it in ~2 min. Verified 23:37:43: B's +52-frame insert moved the wire 1.56 ms
      // in the direction err_tag and the phases had both named.
      bool coarse_step_ok = true;
      if (resync_window && coarse_on_tags &&
          std::abs(coarse_target_us) < this->tune_resync_local_us_.load(std::memory_order_relaxed)) {
        const int32_t gd = this->tsf_sync_ != nullptr ? this->tsf_sync_->render_group_delta_us() : INT32_MIN;
        if (gd == INT32_MIN || std::abs(gd) > 500000 || ((gd > 0) != (coarse_target_us > 0)) ||
            std::abs(gd) < this->tune_resync_splice_us_.load(std::memory_order_relaxed)) {
          coarse_step_ok = false;  // no differential evidence: leave it to the (symmetric) PI
        } else {
          // THE EVIDENCE IS THE GAP TO THE OTHERS, NOT THE DELTA TO THE MEAN. The group delta is
          // mine - mean(all, me included): with two devices it is HALF the pairwise gap by design (each
          // corrects half and they meet). Bounding a one-board step by it halved every step -- 02:25,
          // gain 1.0: +864 -> +430 -> +222 -> +108, four rounds of 3.5 s where +1643 was standing and
          // one step would have done (runs 3 and 4, above the local threshold, converged in one). The
          // gap to the others' mean is delta * n / (n - 1).
          const int32_t n = this->tsf_sync_->consensus_n();
          const int64_t gap = n > 1 ? static_cast<int64_t>(std::abs(gd)) * n / (n - 1) : std::abs(gd);
          coarse_target_us = coarse_target_us < 0 ? -std::min<int64_t>(-coarse_target_us, gap)
                                                  : std::min<int64_t>(coarse_target_us, gap);
        }
      }
      // The damping (resync_gain) is for the TAG steps, which act on a lagged, block-averaged
      // measurement. The ledger's first step after a hard resync is arithmetic -- the dropped chunks
      // are counted -- and with a ~3.4 s wait before the next decision, leaving 20 % of it on the
      // table costs a whole round (build 54: 47-70 s). It takes the full target.
      const int64_t coarse_step_us =
          !coarse_step_ok ? 0
          : (resync_window && coarse_on_tags)
              ? static_cast<int64_t>(static_cast<float>(coarse_target_us) *
                                     this->tune_resync_gain_.load(std::memory_order_relaxed))
              : coarse_target_us;
      const int32_t adjust_frames =
          static_cast<int32_t>(coarse_step_us * static_cast<int64_t>(rec.params.sample_rate) / 1000000);
      // RESYNC WINDOW = STEP AND VERIFY. One correction of the whole measured error per measurement
      // lag (resync_blank_ms ~ pipeline + one block), bounded at half a chunk, and the continuous
      // fast splice stays OUT of the window: build 33 ran both at once and the splice bang-banged
      // (28 frames one way, 16 back, 3, 8, 9, 4 ... within two seconds) because the block error it
      // acts on is held for ~0.65 s while its own splices move the audio underneath the average.
      const int32_t max_adjust = std::max<int32_t>(1, resync_window ? frames / 2 : frames / (SOFT_CORRECTION_DIVISOR / 4));
      const int32_t adjust = std::clamp(adjust_frames, -max_adjust, max_adjust);
      // One short dedicated line per in-window decision (<= one per block). Build 44's B episode
      // (23:51:29-23:52:15) sat at +600 us for 45 s with no step and nothing in the log said why.
      if (resync_window) {
        const int32_t gd_log = this->tsf_sync_ != nullptr ? this->tsf_sync_->render_group_delta_us() : INT32_MIN;
        if (gd_log == INT32_MIN) {
          ESP_LOGD(TAG, "RSTEP err=%+" PRId64 " src=%s gd=unknown ok=%d step=%+" PRId64 " adj=%+" PRId32 " pend=%+" PRId64,
                   coarse_target_us, coarse_on_tags ? "tag" : "ledger", coarse_step_ok ? 1 : 0, coarse_step_us, adjust,
                   pending_us);
        } else {
          ESP_LOGD(TAG, "RSTEP err=%+" PRId64 " src=%s gd=%+" PRId32 " ok=%d step=%+" PRId64 " adj=%+" PRId32 " pend=%+" PRId64,
                   coarse_target_us, coarse_on_tags ? "tag" : "ledger", gd_log, coarse_step_ok ? 1 : 0, coarse_step_us,
                   adjust, pending_us);
        }
      }
      if (adjust > 0) {
        drop_frames = adjust;
        st.soft_dropped_frames += adjust;
      } else if (adjust < 0) {
        st.soft_inserted_frames += -adjust;
        this->push_silence_(-adjust, rec.params);
      }
      if (resync_window && adjust != 0) {
        st.resync_step_at_us = now_us();
        st.win_step_us[st.win_step_idx] =
            static_cast<int64_t>(adjust) * 1000000 / static_cast<int64_t>(rec.params.sample_rate);
        st.win_step_at_us[st.win_step_idx] = st.resync_step_at_us;
        st.win_step_idx = (st.win_step_idx + 1) % ServoState::WIN_STEPS;
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
      // Steady-state rate lock: steer the I2S clock instead of splicing frames -- and steer it on
      // the MEASURED tag error, not the prediction. This is THE DELAY LOOP
      // (PLAN-delay-controlled-servo.md): the prediction from here down is only the per-chunk
      // scheduling comparison (hard resync, stale bailout, storm mute, splice fallback), and the
      // rate loop's error signal is err_tag, which the ledger appears nowhere in. The gate below
      // still reads the median because it is a COARSE audibility gate, not the loop's error:
      // converged latches, so it binds only during acquisition, where the coarse machinery is
      // already in charge. Muted convergence uses hard splices while far out (much faster than
      // the trim slew), handing off to the loop for the end-game.
      // GATE ON THE MEASURED ERROR WHILE TAGS ARE LIVE: with a biased ledger the median can sit
      // outside converge_fine forever on an unconverged board, which parked the rate loop OFF and
      // handed position to the prediction-driven steer fallback -- measured on B 2026-08-29: median
      // +2019 us (ledger) vs err_tag -8.6 ms, -136 frames dropped by the steer against +120 inserted
      // by the tag-driven fast path per report, a tug of war that held B 9 ms early indefinitely.
      const int64_t gate_err_us = coarse_on_tags ? coarse_err_us : median_err_us;
      if (!(st.rate_lock_ok && (st.converged || std::abs(gate_err_us) <= this->config_.converge_fine_us))) {
        // The refusal, with the terms that caused it. Overwritten per refusing chunk so the report
        // carries the most recent one; see ServoState::gate_seen.
        st.gate_seen = true;
        st.gate_rate_lock_ok = st.rate_lock_ok;
        st.gate_converged = st.converged;
        st.gate_median_err_us = static_cast<int32_t>(median_err_us);
      }
      if (st.rate_lock_ok && (st.converged || std::abs(gate_err_us) <= this->config_.converge_fine_us)) {
        // DELAY LOOP: at most one PI step per completed measurement block (~3 Hz); between blocks,
        // and through everything that suppresses tags, the demand is left exactly where it was --
        // which is what "hold the last trim, never the last error" means concretely. The gain
        // schedule, bumpless transfer, conditional integration and the clamp all live inside.
        this->delay_loop_update_(st);
        // THE SPLIT-PENDING TRIM HOLD IS DELETED, not improved: it existed because the rate loop
        // steered on a prediction the split detector was about to declare wrong, and the loop no
        // longer consumes that prediction at all. Its inter-device cost was the largest identified
        // term (A frozen at +64.00 ppm against B steering at +38.15, ~26 ppm for 3 s ~ 78 us of
        // skew). The split detector itself survives, guarding the SPLICE fallback below -- the one
        // retained consumer that still acts on the prediction at sub-millisecond scale.
  #ifdef USE_SNAPCLIENT_TIMING_DIAG
        // Report-only: span shows whether the loop tracks a slow offset or chases
        // something it cannot, and railed counts saturation.
        if (st.trim_samples == 0) {
          st.trim_min_ppm = st.trim_max_ppm = st.trim_applied_ppm;
        } else {
          st.trim_min_ppm = std::min(st.trim_min_ppm, st.trim_applied_ppm);
          st.trim_max_ppm = std::max(st.trim_max_ppm, st.trim_applied_ppm);
        }
        st.trim_samples++;
        if (std::abs(st.trim_applied_ppm) >= trim_clamp_ppm(this->config_.converge_fine_us) - 0.5f) {
          st.trim_railed++;
        }
  #endif
        // Programmed EVERY chunk with whatever the loop last demanded -- this line is the hold.
        trim_holds = this->rate_lock_->set_trim_ppm(st.trim_applied_ppm);
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
        //
        // "No valid rate estimate out here" is superseded: the delay loop's integral IS the
        // learned crystal offset and survives disengagement, so holding it -- not nominal --
        // is what stops the error re-accruing at crystal rate (~55 us/s measured) through every
        // muted excursion, which was manufacturing the next excursion. At a cold boot the
        // integral is still 0 and this is exactly the old behaviour.
        {
          const float hold_ppm = std::clamp(st.trim_integral_ppm, -trim_clamp_ppm(this->config_.converge_fine_us),
                                            trim_clamp_ppm(this->config_.converge_fine_us));
          this->rate_lock_->set_trim_ppm(hold_ppm);
          st.trim_applied_ppm = hold_ppm;
        }
      }
#endif
      // While muted (pre-convergence) audibility doesn't constrain splice size, so
      // steer hard to reach the band quickly, then single frames for the end-game
      if (!trim_holds && !coarse_on_tags) {
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
        // only the rate loop to remove it, over minutes at the delay loop's tau. Position
        // correction runs here instead, well above the band, one frame at a time.
        //
        // SIGNAL SELECTION, and it decides the headline inject_split test: while the measured tag
        // error is fresh, splices act on IT, so a ledger bias cannot move audio through this path
        // and the split-pending guard is not applied (the bias is invisible to err_tag, and
        // guarding here would disarm the mechanism against exactly the errors it can safely
        // correct). When tags are stale the demoted prediction is the only estimate left, and
        // there the guard is irreplaceable: a ledger bias is then indistinguishable from a real
        // error, and refusing to act on a suspect one is the best anything can do.
        const bool tag_err_live = st.dl_have_err && now_us() - st.dl_err_at_us < DL_ERR_STALE_US &&
                                  now_us() >= st.tag_fault_until_us;
        // The in-flight horizon is a property of the SIGNAL's measurement lag. For err_tag it is
        // one pipeline depth (a splice is invisible until the spliced audio renders) plus half the
        // averaging block, derived from the measured depth rather than inherited from the median's
        // 15 -- similar magnitude today only by coincidence. The depth comes from the ledger, but
        // only as a WINDOW LENGTH in whole chunks: a bias of even a few ms is a fraction of one
        // chunk here, so this use survives ledger perturbation.
        uint32_t horizon_chunks = SPLICE_HORIZON_PREDICTION_CHUNKS;
        if (tag_err_live && frames > 0) {
          const int64_t chunk_us = static_cast<int64_t>(frames) * 1000000 / rec.params.sample_rate;
          const int64_t depth_chunks = st.pipe_depth_frames / static_cast<int64_t>(frames);
          const int64_t block_half_chunks =
              chunk_us > 0 ? (static_cast<int64_t>(this->tune_block_n_.load(std::memory_order_relaxed) / 2) *
                              DL_ARRIVAL_US) / chunk_us
                           : 0;
          horizon_chunks = static_cast<uint32_t>(std::clamp<int64_t>(
              depth_chunks + block_half_chunks, 1, static_cast<int64_t>(ServoState::SPLICE_HIST)));
        }
        const int32_t fast =
            this->fast_splice_(st, tag_err_live ? st.dl_err_us : median_err_us, rec.params.sample_rate,
                               !tag_err_live && st.drift_excess_since_us != 0, horizon_chunks, tag_err_live);
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
                   // 0 means the delay loop has not completed a block since boot. Printing that 0
                   // reads as "the loop is running at zero gain", which is the same class of lie
                   // as the "(idle)" trim snapshot. Fall back to what the schedule WOULD hand it,
                   // scaled to the DELAY LOOP's gain -- trim_kp_ returns the legacy scale.
                   st.kp_active > 0.0f
                       ? st.kp_active
                       : (1.0f / this->tune_tau_s_.load(std::memory_order_relaxed)) *
                             (this->trim_kp_(st) / TRIM_KP_RUN_PPM_PER_US));
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

    // NO PLAYOUT-HEALTH REPORT TO THE TSF LAYER. There used to be one, because a leader
    // publishing the group's timebase had to hand off while its own playout was diverged. Nobody
    // publishes for anybody now -- each device publishes its own server<->TSF estimate, which is
    // a property of its clock and not of its audio -- so health gates nothing.
    //
    // The cure was worse than the disease anyway: a device is briefly unhealthy after every
    // resync, so the gate produced six leadership changes in seventeen minutes, and when both
    // clients stepped down after a pause the group lost its only publisher and every device sat
    // muted for ~47 s with its own servo already in band.

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
    // The unmute/converged latch reads the measured error while tags are live: a biased ledger
    // otherwise keeps a board 'unconverged' with its audio perfectly placed (or vice versa).
    // UNMUTE ON GROUP AGREEMENT. Mute-until-converged exists to hide MUTUAL desync; the group render
    // delta measures exactly that. After a boot both boards read the same ~-700 us against the server
    // timeline (common: the mapping, not a mutual offset), the gate rightly refuses one-board steps
    // on it, and each board spent tau = 120 s closing it in silence before "Sync locked" (07:10 boot:
    // A +150 s, B +186 s) -- while |group delta| was < 30 us from +30 s. Being a few ms from the
    // server's timeline is tolerable (operator, 2026-08-30: 3-5 ms); being apart from each other is
    // not. So: own error inside the band as before, OR the group agrees (|delta| inside the same
    // band) with own error inside UNMUTE_COMMON_US. Delta unknown -> the own-error rule alone.
    const int64_t unmute_err_us = std::abs(coarse_on_tags ? coarse_err_us : median_err_us);
    const int64_t unmute_band_us = UNMUTE_BAND_DEADBANDS * this->config_.sync_deadband_us;
    bool group_agrees = false;
#ifdef CLOCK_SYNC_TSF_ACTIVE
    if (this->tsf_sync_ != nullptr && this->tsf_sync_->consensus_n() >= 2) {
      const int32_t ugd = this->tsf_sync_->render_group_delta_us();
      group_agrees = ugd != INT32_MIN && std::abs(ugd) <= unmute_band_us && unmute_err_us <= UNMUTE_COMMON_US;
    }
#endif
    if (unmute_err_us <= unmute_band_us || group_agrees) {
#ifdef CLOCK_SYNC_TSF_ACTIVE
      // Don't unmute onto a provisional timebase: a device still on its Kalman fallback while
      // peers are audible will step by up to the plausibility bound when it finally adopts the
      // shared mapping -- audible corrections right after unmute on every speaker join. A device
      // consensing alone has nothing to converge onto, so it is not made to wait.
      const bool timebase_settled = this->tsf_sync_ == nullptr || this->tsf_sync_->consensus_n() < 2 ||
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
          st.unmute_anchor_wait_us = 0;
          if (waited_out) {
            ESP_LOGW(TAG, "Sync locked (median %" PRId64 " us) but the anchor still reads %" PRId32
                          " us after %" PRId64 " s -- unmuting anyway; expect a planted offset of "
                          "about the difference against the other devices",
                     median_err_us, st.drift_med_last_us, UNMUTE_ANCHOR_MAX_WAIT_US / 1000000);
          } else if (group_agrees && unmute_err_us > unmute_band_us) {
            ESP_LOGI(TAG, "Sync locked on group agreement (own err %" PRId64 " us, anchor %" PRId32 " us), unmuting",
                     unmute_err_us, st.drift_med_last_us);
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
      // Both counts, so the re-basing can be SHOWN to fix it rather than assumed to: raw is what a
      // frames-based pivot would have consumed, rebased is what it will consume.
      ESP_LOGD(TAG, "RPUSH n=%" PRIu32 " bad_raw=%" PRIu32 " (%.1f%%) bad_rebased=%" PRIu32 " (%.1f%%) t=%" PRId64,
               st.rpush_samples, st.rpush_bad,
               100.0f * static_cast<float>(st.rpush_bad) / static_cast<float>(st.rpush_samples), st.rpush_bad_reb,
               100.0f * static_cast<float>(st.rpush_bad_reb) / static_cast<float>(st.rpush_samples), now_us());
      st.rpush_samples = 0;
      st.rpush_bad = 0;
      st.rpush_bad_reb = 0;
    }

    this->accumulate_achieved_rate_(st, rec);
    this->reanchor_after_relock_(st);

    this->player_phase_.store(static_cast<uint8_t>(PlayerPhase::SERVO), std::memory_order_relaxed);
    // NEVER_MUTE means never: the start-up silence until "Sync locked" is a mute like any other, and
    // it hid the wire from the analyser for 1.5-4 minutes after every boot (2026-08-30 07:10).
    const bool startup_silent = !st.converged && this->sync_resilience() != SyncResilience::NEVER_MUTE;
    this->push_chunk_(rec, drop_frames, startup_silent);
    // One completed chunk. The main loop watches this for movement; see PlayerPhase.
    this->player_progress_.fetch_add(1, std::memory_order_relaxed);

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
    // Newest depth, kept for sizing the splice in-flight horizon (whole chunks, so ledger noise
    // and even a multi-ms bias are sub-quantum here).
    st.pipe_depth_frames = this->pushed_frames_total_ - this->played_frames_total_;
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
          const int64_t pushed_now = static_cast<int64_t>(this->pushed_frames_total_);
          const int64_t src_now = static_cast<int64_t>(d_meas.dbg_src_received);
          const int64_t r_push = pushed_now - src_now;
          const uint32_t p_epoch = this->playout_epoch_.load(std::memory_order_relaxed);
          if (!st.rpush_base_valid || p_epoch != st.rpush_epoch) {
            st.rpush_epoch = p_epoch;
            st.rpush_base_pushed = pushed_now;
            st.rpush_base_src = src_now;
            st.rpush_base_valid = true;
          }
          const int64_t r_push_reb = (pushed_now - st.rpush_base_pushed) - (src_now - st.rpush_base_src);
          st.rpush_samples++;
          if (r_push < -RPUSH_VALID_FRAMES || r_push > RPUSH_VALID_FRAMES) {
            st.rpush_bad++;
          }
          if (r_push_reb < -RPUSH_VALID_FRAMES || r_push_reb > RPUSH_VALID_FRAMES) {
            st.rpush_bad_reb++;
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

#ifdef USE_I2S_RATE_LOCK
// THREAD CONTEXT: player task. THE DELAY LOOP -- see the constants block at DL_BLOCK_N and
// PLAN-delay-controlled-servo.md for the design and its bench evidence.
// The MEASURED render phase -- TSF(first frame of the latest tagged render) minus that frame's
// server time -- published to the group. Identical to the report-time computation in
// log_sync_report_ (which also logs RENDERTAG/inferred); this one runs per delay-loop block so the
// group delta has a fresh pairing every beacon. Publishes UNKNOWN rather than a stale value: the
// freshness gate is the same 100 ms / blank rule, for the reasons documented at the report site.
void SnapcastClient::publish_render_phase_(bool steady) {
#ifdef CLOCK_SYNC_TSF_ACTIVE
  if (this->tsf_sync_ == nullptr || this->config_.tsf_observer) {
    return;
  }
  // A BOARD IN TRANSIENT PUBLISHES NO PHASE. While its resync window is open or it is not converged
  // its audio is being stepped toward its own deadline; a peer aligning to that chases a moving
  // target. 08:54 (build 62): B reconnected, A's view of B went +1969 (rejected) then +89, +67, +47,
  // +19 as B stepped home, and A walked its bias -6 -> -75 us toward where B had been -- a 70 us
  // excursion on the wire that took four minutes to unwind. Dropping out of the group for the
  // duration leaves the others holding still, which is what a resyncing peer needs from them.
  // The phase is still measured and kept LOCALLY (the resync gate reads my own delta against the
  // peers' phases while my window is open -- that is when it needs it); only the BEACON goes quiet.
  this->tsf_sync_->set_render_phase_broadcast(steady);
  int64_t phase_tsf = 0, phase_local = 0, phase_width = 0;
  if (!TsfSync::raw_tsf_sample(phase_tsf, phase_local, phase_width)) {
    return;
  }
  this->playout_mutex_.lock();
  const TaggedRender tr = this->tagged_render_;
  const int64_t dl_blank = this->dl_blank_until_us_;
  this->playout_mutex_.unlock();
  constexpr int64_t RENDER_TAG_MAX_AGE_US = 100000;
  const int64_t tag_age_us = phase_local - tr.adjusted_ts_us;
  const bool tag_fresh = tr.adjusted_ts_us > 0 && tr.sample_rate > 0 && tag_age_us < RENDER_TAG_MAX_AGE_US &&
                         tag_age_us > -RENDER_TAG_MAX_AGE_US && tr.adjusted_ts_us >= dl_blank;
  if (!tag_fresh) {
    this->tsf_sync_->set_render_phase_us(TsfSync::RENDER_PHASE_UNKNOWN);
    return;
  }
  const int64_t rate = static_cast<int64_t>(tr.sample_rate);
  const int64_t first_frame_local = tr.adjusted_ts_us - static_cast<int64_t>(tr.frames) * 1000000 / rate;
  const int64_t render_tsf = first_frame_local + (phase_tsf - phase_local);
  const int64_t render_server = tr.server_ts_us + static_cast<int64_t>(tr.offset_frames) * 1000000 / rate;
  this->tsf_sync_->set_render_phase_us(render_tsf - render_server, phase_local);
#endif
}

void SnapcastClient::delay_loop_update_(ServoState &st) {
  // Pull the accumulator state. A completed block is drained; a partial one is left to fill --
  // unless the stream has gone stale, in which case the partial block spans a gap and is discarded
  // (samples from before an outage folded into a mean with samples after it describe nothing).
  const int64_t tag_stale_us = static_cast<int64_t>(this->tune_tag_stale_ms_.load(std::memory_order_relaxed)) * 1000;
  const uint32_t block_n = static_cast<uint32_t>(this->tune_block_n_.load(std::memory_order_relaxed));
  this->playout_mutex_.lock();
  const int64_t last = this->dl_acc_last_us_;
  const int64_t now = now_us();
  const bool tags_live = last > 0 && now - last < tag_stale_us;
  const bool ready = tags_live && this->dl_acc_n_ >= block_n;
  const uint32_t n = this->dl_acc_n_;
  const double sum = this->dl_acc_sum_us_;
  const int64_t first = this->dl_acc_first_us_;
  if (ready || !tags_live) {
    this->dl_acc_n_ = 0;
    this->dl_acc_sum_us_ = 0.0;
  }
  this->playout_mutex_.unlock();

  // IN-RANGE HOLD (tag loss, mapping flap): keep the P term and DECAY it toward the integral over
  // tau. Holding the integral alone dropped P instantly -- with P ~ 25 ppm of legitimate response
  // to the common-mode wander that the peer kept applying, every ~1 s mapping flap became a
  // differential rate step and ~130 us of wire skew (measured build 10). A long outage still ends
  // at the crystal offset, which is what the integral-only hold was for. Out-of-range keeps the
  // integral-only rule: that P is mid-transient and untrusted.
  const float clamp_hold = trim_clamp_ppm(this->config_.converge_fine_us);
  auto enter_hold = [&](const char *why) {
    st.dl_active = false;
    st.dl_hold_p_ppm = st.trim_applied_ppm - st.trim_integral_ppm;
    st.dl_hold_since_us = now;
    ESP_LOGD(TAG, "Delay loop: %s, holding integral %+.2f ppm + P %+.2f decaying over tau t=%" PRId64, why,
             st.trim_integral_ppm, st.dl_hold_p_ppm, now);
  };
  auto apply_hold = [&]() {
    const float tau_s = this->tune_tau_s_.load(std::memory_order_relaxed);
    const float age_s = static_cast<float>(now - st.dl_hold_since_us) / 1000000.0f;
    const float p = st.dl_hold_since_us != 0 ? st.dl_hold_p_ppm * std::exp(-age_s / tau_s) : 0.0f;
    st.trim_applied_ppm = std::clamp(st.trim_integral_ppm + p, -clamp_hold, clamp_hold);
  };

  if (!tags_live) {
    // HOLD, not revert: st.trim_applied_ppm keeps being programmed by the caller, so the learned
    // crystal offset keeps cancelling. There is no ledger servo to fall back to -- that is the
    // design, not an omission. dl_active drops so the next fresh block resumes from the integral,
    // and dl_have_err drops so fast_splice_ hands back to the demoted prediction (where the
    // split-pending guard applies again).
    if (st.dl_active) {
      enter_hold("tags stale");
    }
    apply_hold();
    st.dl_have_err = false;
    return;
  }

#ifdef CLOCK_SYNC_TSF_ACTIVE
  // PRECONDITION: the shared TSF mapping. deadline() sits inside err_tag, so the clock-offset
  // estimator is inside this loop; on the shared mapping its wander is common-mode across the
  // group and harmless to alignment, on the local-Kalman fallback it is per-device and steering
  // on it misaligns. Hold trim until the mapping returns (chosen over widening tau: it matches
  // the tag-loss behaviour and needs no second tuning). A device consensing alone has no group
  // to misalign against and is not made to wait.
  const bool mapping_shared = this->tsf_sync_ == nullptr || this->tsf_sync_->consensus_n() < 2 ||
                              this->deadline_on_shared_tsf_;
  if (!mapping_shared) {
    if (st.dl_active) {
      enter_hold("deadline on local fallback");
    }
    apply_hold();
    st.dl_have_err = false;
    return;
  }
#endif

  if (!ready) {
    return;
  }

  const float e = static_cast<float>(sum / static_cast<double>(n));
  st.dl_err_us = static_cast<int64_t>(llroundf(e));
  st.dl_err_at_us = now;
  st.dl_have_err = true;
  st.dl_updates++;
  // PUBLISH THE RENDER PHASE PER BLOCK, NOT PER REPORT. The group delta pairs my phase with a
  // peer's only if the two were sampled within PHASE_PAIR_WINDOW_US (300 ms) of each other; with
  // both boards publishing once per ~3.3 s report that was a coincidence -- delta known on 62 % of
  // reports, unknown for the first 20-40 s after a boot, and the resync gate refusing for want of
  // evidence (build 48 boot, 00:43: RSTEP gd=unknown). Every ~0.65 s block on each board puts ~5
  // samples inside each 3.3 s and a pairing inside every beacon interval.
  // "In transient" = a position step or hard resync landed within the last PHASE_TRANSIENT_US -- the
  // interval in which my measured phase does not yet describe where my audio will be. NOT "window
  // open" and NOT "unconverged": build 63 used those and at boot neither board broadcast anything,
  // so there was no group delta for the gate or the group-agreement unmute, and both crawled in on
  // the PI (+528 us on the wire for a minute, 09:08).
  const bool in_transient = (st.kp_event_us != 0 && now - st.kp_event_us < PHASE_TRANSIENT_US) ||
                            (st.resync_step_at_us != 0 && now - st.resync_step_at_us < PHASE_TRANSIENT_US);
  this->publish_render_phase_(!in_transient);

  // ABOVE THE SPLICE THRESHOLD, SPLICE; BELOW IT, TRIM -- the plan's rule, and the trim half of
  // it: position errors at the millisecond scale belong to the fast path, and a rate loop asked
  // to chase them converts every coarse correction into a rail-to-rail trim excursion. Measured
  // on the first flash without this guard: engaged at err +15081 us and slammed to the +1000 ppm
  // rail at boot, and wound the integral to -994 ppm chasing a delivery stall's displaced audio.
  // Hold the trim (the integral is still the learned crystal offset), keep publishing dl_err so
  // fast_splice_ acts on the measured error, and do not SEED while out of range -- acquisition
  // splices down to the threshold first, and the loop takes over from there.
  const int32_t splice_override = this->tune_splice_us_.load(std::memory_order_relaxed);
  const int64_t splice_threshold =
      splice_override >= 0 ? static_cast<int64_t>(splice_override)
      : this->config_.fast_splice_threshold_us > 0
          ? static_cast<int64_t>(this->config_.fast_splice_threshold_us)
          : static_cast<int64_t>(this->config_.converge_fine_us);
  if (std::abs(st.dl_err_us) >= splice_threshold) {
    if (st.dl_integral_ema_valid && std::abs(st.trim_integral_ppm - st.dl_integral_ema_ppm) > DL_INTEGRAL_SNAP_PPM) {
      ESP_LOGW(TAG, "Delay loop: out of range with integral %+.2f far from its average %+.2f -- snapping to the average",
               st.trim_integral_ppm, st.dl_integral_ema_ppm);
      st.trim_integral_ppm = st.dl_integral_ema_ppm;
    }
    // HOLD THE INTEGRAL, NOT THE WHOLE TRIM -- the split-hold's lesson, verbatim. Out of range is
    // by definition mid-transient, so the last demanded trim carries a P term computed against an
    // error the fast path is about to remove; the integral is the learned crystal offset and is
    // the only part worth holding. Measured without this: boards held arbitrary transient trims
    // (+96, +121 ppm) through 1 ms sawtooth cycles while the error GREW under them.
    st.trim_applied_ppm = std::clamp(st.trim_integral_ppm, -trim_clamp_ppm(this->config_.converge_fine_us),
                                     trim_clamp_ppm(this->config_.converge_fine_us));
    if (now - st.dl_log_us >= DL_LOG_INTERVAL_US) {
      st.dl_log_us = now;
      ESP_LOGD(TAG, "DLLOOP err=%+" PRId64 " us OUT OF RANGE (>=%" PRId64 "), holding integral %+.2f ppm t=%" PRId64,
               st.dl_err_us, splice_threshold, st.trim_applied_ppm, now);
    }
    return;
  }

  const float clamp_ppm = trim_clamp_ppm(this->config_.converge_fine_us);
  // The gain schedule survives, scaled to the loop's own run gain: trim_kp_ still reads a timer
  // since a discrete event (hard resync, timebase re-anchor), never the error, and those events
  // also blank the tag stream -- so by the time a block completes under the elevated gain, the
  // audio it measured is genuinely post-event.
  // Error-proportional gain: the tunables are the floor, |err| raises them (knee_us / tau_min_s).
  const float tau_tuned = this->tune_tau_s_.load(std::memory_order_relaxed);
  const float ti_tuned = this->tune_ti_s_.load(std::memory_order_relaxed);
  const float knee_us = this->tune_knee_us_.load(std::memory_order_relaxed);
  const float tau_min = this->tune_tau_min_s_.load(std::memory_order_relaxed);
  // WILD DESYNC RE-OPENS THE WINDOW. B's error stepped -66 -> -545 us in ten seconds at 22:56:57
  // (a per-board timebase step under a 1-2 ms consensus spread; A flat) thirteen seconds after the
  // boot window had closed, and recovery then ran at steady-state gains: 450 -> 224 us in 35 s. A
  // jump past resync_reopen_us (400 -- the common wander stays under it) is a known displacement
  // whatever caused it, and gets the step-and-verify treatment.
  if (std::abs(e) > this->tune_resync_reopen_us_.load(std::memory_order_relaxed) && now >= st.post_event_until_us) {
    st.post_event_until_us =
        now + static_cast<int64_t>(this->tune_resync_win_s_.load(std::memory_order_relaxed) * 1000000.0f);
    st.resync_step_at_us = 0;  // a fresh window: the ledger may take its first step
    std::fill(std::begin(st.win_step_at_us), std::end(st.win_step_at_us), 0);
    st.resync_inside_since_us = 0;
    ESP_LOGI(TAG, "Delay loop: resync window re-opened on a %+.0f us step t=%" PRId64, e, now);
  }
  // ... AND CLOSES ON CONVERGENCE, not on the timer. Left open for the full 60 s, the in-window
  // coarse steps kept firing on the +-60..150 us post-event noise and the common wander after the
  // wire had already crossed zero (22:59:04 -> -145 us by 22:59:34): each 80 % step over-corrects
  // against the other board. Inside the arm threshold for resync_close_s -> the window is done.
  if (now < st.post_event_until_us) {
    if (std::abs(e) <= this->tune_resync_splice_us_.load(std::memory_order_relaxed)) {
      if (st.resync_inside_since_us == 0) st.resync_inside_since_us = now;
      else if (now - st.resync_inside_since_us >= static_cast<int64_t>(this->tune_resync_close_s_.load(std::memory_order_relaxed) * 1000000.0f)) {
        st.post_event_until_us = now;
        st.resync_inside_since_us = 0;
        ESP_LOGD(TAG, "Delay loop: resync window closed, converged t=%" PRId64, now);
      }
    } else {
      st.resync_inside_since_us = 0;
    }
  }
  // NO PER-BOARD RATE-GAIN BOOST IN THE RESYNC WINDOW. Measured 2026-08-29 22:58-22:59 (300 ms
  // injection on A): with A at kp 0.05 inside its window and B at 0.008 outside, the SAME common
  // deadline wander (+30..+130 us on both) became a 2-4 ppm differential trim -- (0.05-0.008) x 80 us
  // -- and the wire walked away from zero at 2-3 us/s for 30 s in block-sized stairs. Any gain
  // that only one board has turns common-mode error into differential motion; the rate loop's
  // gain must be the same function of the error on every board. Position corrections (the coarse
  // step-and-verify) do the resync: a bounded one-off, not a sustained rate.
  const float boost = std::clamp(std::abs(e) / knee_us, 1.0f, std::max(1.0f, tau_tuned / tau_min));
  const float tau_eff = tau_tuned / boost;
  // Ti is NOT boosted: Ki = kp/Ti already rises with kp. Dividing Ti too made Ki scale with boost^2
  // and wound the (already correct, NVS-restored) integral during the position catch-up -- the
  // -150 us undershoot and the minutes-long tail on A's 13:35 boot. A boot error is position, not
  // rate; P should close it and the integral should barely move.
  const float ti_eff = ti_tuned;
  const float kp_run = 1.0f / tau_eff;
  // FLAT GAIN: Kp = 1/tau, no acquire->run schedule. The schedule bought fast nulling after a
  // hard resync when the rate loop was the only corrector; here anything past the splice
  // threshold belongs to the fast path and anything inside it does not need 2x gain -- while
  // Ki = Kp^2 meant every re-arm wound the integral 4x faster. Measured (build 7 boot): source
  // flaps re-arming every ~25 s drove A's integral to +134 ppm against a ~+60 ppm crystal.
  // kp still changes when tau is retuned over the API; the bumpless transfer below covers that.
  const float kp = kp_run;
  st.kp_active = kp;

  if (!st.dl_active) {
    // ENGAGE WITH THE INTEGRAL AS IT STANDS -- never re-seed it from the applied trim. The
    // integral is the learned crystal offset and survives in RAM across every disengage; the
    // applied trim does not survive the actors that write it (the muted out-of-band branch
    // programs 0). Seeding from applied_ppm() was measured 2026-08-28 19:06 re-engaging at
    // "+0.00 ppm" after a mute cycle: the board then ran ~crystal slow, accrued 1 ms in ~18 s,
    // and sawtoothed through splice/resync cycles -- the audible flutter. At a genuine cold boot
    // the integral is 0 anyway, and that wind-up is the closed-loop PI response to a ~40 ppm rate
    // step: peak ~0.5*crystal/Kp ~ 200 us at tau = 10 s, under the splice threshold.
    st.dl_kp_last = 1.0f / tau_tuned;
#ifdef CLOCK_SYNC_TSF_ACTIVE
    // COLD START SEED. A fresh board has no NVS integral and would wind ~56 ppm through Ki --
    // 10+ minutes at Ti 600, spent near the splice/out-of-range thresholds. The TSF crystal
    // estimate is the same hardware property measured against the radio within seconds of boot;
    // it sits ~14 ppm from the trim the DAC actually needs (int +56 vs crystal +42, measured all
    // day), which the fast boot Ti then absorbs in tens of seconds instead of tens of minutes.
    if (st.dl_cold_start && st.trim_integral_ppm == 0.0f && this->tsf_sync_ != nullptr) {
      const float seed = this->tsf_sync_->own_crystal_ppm();
      if (std::isfinite(seed) && std::abs(seed) <= TRIM_CLAMP_MAX_PPM) {
        st.trim_integral_ppm = seed;
        ESP_LOGI(TAG, "Delay loop: cold start, integral seeded %+.2f ppm from the TSF crystal estimate", seed);
      }
    }
#endif
    st.dl_active = true;
    st.dl_engaged_since_us = now;
    st.post_event_until_us =
        now + static_cast<int64_t>(this->tune_resync_win_s_.load(std::memory_order_relaxed) * 1000000.0f);
    st.resync_step_at_us = 0;  // a fresh window: the ledger may take its first step
    std::fill(std::begin(st.win_step_at_us), std::end(st.win_step_at_us), 0);
    ESP_LOGD(TAG, "Delay loop: engaged, integral %+.2f ppm (err %+" PRId64 " us) t=%" PRId64,
             st.trim_integral_ppm, st.dl_err_us, now);
  }

  // Bumpless transfer for TUNABLE changes only (tau_s over the API): move the output step
  // (kp_old - kp_new) * e into the integrator so the commanded trim is continuous. Keyed on the
  // tuned 1/tau, NOT on the error-proportional kp: that one changes every block, and folding its
  // change into the integral made a hidden integrator of (dkp * e) -- measured build 20, B at
  // 13:45:49-53: integral 54.2 -> 61.5 ppm in four blocks as err crossed +232 -> -189 us, then the
  // wire diverged from zero as the loop chased its own integral. The proportional boost is meant
  // to step the output; that step IS the acquisition.
  const float kp_tuned = 1.0f / tau_tuned;
  if (st.dl_kp_last != kp_tuned) {
    st.trim_integral_ppm = std::clamp(st.trim_integral_ppm + (st.dl_kp_last - kp_tuned) * e, -clamp_ppm, clamp_ppm);
    st.dl_kp_last = kp_tuned;
  }

  // dt is the block's real span, not an assumed cadence -- arrivals pause whenever tagged audio
  // does. Bounded, so a pathological pair of stamps cannot wind the integral by a large step.
  const float dt_s = std::clamp(static_cast<float>(last - first) / 1000000.0f, 0.05f, 2.0f);
  const float p_term = kp * e;
  // Conditional integration (anti-windup): freeze the integral whenever the output is saturated
  // in the error's own direction -- the +164.9 ppm runaway from the realised-slope experiment is
  // what its absence looks like. Ki = Kp / Ti with Ti a separate tunable (default 120 s): Ti = tau
  // (Ki = Kp^2) let the integral swing ~57 ppm p-p chasing the common-mode wander -- see
  // tune_ti_s_. The integral models the crystal, which moves over minutes, and the NVS restore
  // removed the cold-boot reason for a fast one.
  const float unclamped = p_term + st.trim_integral_ppm;
  if (std::abs(unclamped) < clamp_ppm || (unclamped > 0.0f) != (e > 0.0f)) {
    st.trim_integral_ppm =
        std::clamp(st.trim_integral_ppm +
                       (kp / ((st.dl_cold_start && now < DL_TI_BOOT_WINDOW_US) ? DL_TI_BOOT_S : ti_eff)) *
                           e * dt_s,
                   -clamp_ppm, clamp_ppm);
  }
  st.trim_applied_ppm = std::clamp(p_term + st.trim_integral_ppm, -clamp_ppm, clamp_ppm);
  // ALIGN KICK: deliver a render_align bias change as position NOW, by rate, instead of letting the PI
  // walk the audio to the moved deadline over tau = 120 s. That lag is what forced align's gain to
  // 0.03 (0.1 hunted at +-100 us on 2026-08-29; +-10 us "5-minute sawtooth" on 2026-08-30) and made
  // the wire's mean crawl at ~3 us/min. A +D us bias (deadline later) means the audio must play D us
  // later: err_tag reads -D, the PI would slow by kp*D; here the rate is lowered by up to
  // ALIGN_KICK_MAX_PPM until D has been delivered (5 us in ~0.5 s at 10 ppm), and the P-term's
  // contribution at a 5 us error (0.04 ppm) is noise beside it. With the lag gone, align's loop is the
  // ~3.5 s tag visibility and a gain of ~0.3 per 10-s cycle is stable.
  if (st.align_kick_us != 0.0f) {
    const float want_ppm = -st.align_kick_us / dt_s;  // deliver it all this block if allowed
    const float kick_ppm = std::clamp(want_ppm, -ALIGN_KICK_MAX_PPM, ALIGN_KICK_MAX_PPM);
    st.align_kick_us += kick_ppm * dt_s;  // delivered part: -kick_ppm*dt_s us of audio movement
    if (std::abs(st.align_kick_us) < 0.05f) {
      st.align_kick_us = 0.0f;
    }
    st.trim_applied_ppm = std::clamp(st.trim_applied_ppm + kick_ppm, -clamp_ppm, clamp_ppm);
  }

  // Slow average of the integral, for persistence (see dl_integral_ema_ppm).
  if (!st.dl_integral_ema_valid) {
    st.dl_integral_ema_ppm = st.trim_integral_ppm;
    st.dl_integral_ema_valid = true;
  } else {
    const float alpha = std::min(1.0f, dt_s / DL_PERSIST_EMA_S);
    st.dl_integral_ema_ppm += alpha * (st.trim_integral_ppm - st.dl_integral_ema_ppm);
  }
  this->dl_integral_ema_mirror_.store(st.dl_integral_ema_ppm, std::memory_order_relaxed);

  // Persist the learned crystal offset, slowly and only on real change; see DL_PERSIST_DELTA_PPM.
  // dl_saved_at_us == 0 means never saved this boot: the first write is allowed at once, or a
  // board rebooted inside the interval never persists anything (measured: 90 s after engage with
  // the integral at +134 ppm and nothing written, because uptime < 10 min).
  if (this->tune_persist_.load(std::memory_order_relaxed) &&
      std::abs(st.dl_integral_ema_ppm - st.dl_saved_integral_ppm) > DL_PERSIST_DELTA_PPM &&
      st.dl_engaged_since_us != 0 && now - st.dl_engaged_since_us > DL_PERSIST_SETTLE_US &&
      (st.dl_saved_at_us == 0 || now - st.dl_saved_at_us > DL_PERSIST_MIN_INTERVAL_US)) {
    float v = st.dl_integral_ema_ppm;
    if (this->dl_integral_pref_.save(&v)) {
      global_preferences->sync();
      st.dl_saved_integral_ppm = v;
      st.dl_saved_at_us = now;
      ESP_LOGD(TAG, "Delay loop: integral %+.2f ppm persisted t=%" PRId64, v, now);
    }
  }

  // AUTOTUNE (master toggle, default off): adapt tau from the lag-1 autocorrelation of the
  // block-error series, one decision per 64-block (~21 s) window -- a decade slower than the
  // loop, which is what keeps an adapter wrapped around a controller from becoming a second
  // oscillator. Ringing (r1 strongly negative: successive block means alternating) means the
  // loop is too fast for the current lag/noise -> slow 25%. A standing mean with r1 near +1
  // (error walks, loop not keeping up) -> speed 15%. Hard bounds, WARN-logged, never persisted.
  if (this->tune_autotune_.load(std::memory_order_relaxed)) {
    st.at_win[st.at_n++] = e;
    if (st.at_n >= ServoState::AT_WINDOW) {
      const double dn = static_cast<double>(st.at_n);
      double mean = 0.0;
      for (uint32_t i = 0; i < st.at_n; i++) mean += st.at_win[i];
      mean /= dn;
      // Proper lag-1 autocorrelation of the demeaned window: one denominator, |r1| <= 1.
      double num = 0.0, den = 0.0;
      for (uint32_t i = 0; i < st.at_n; i++) {
        const double d = st.at_win[i] - mean;
        den += d * d;
        if (i > 0) num += d * (st.at_win[i - 1] - mean);
      }
      const double var = den / dn;
      const double r1 = den > 1.0 ? num / den : 0.0;
      const double sem = std::sqrt(var / dn);
      const float tau = this->tune_tau_s_.load(std::memory_order_relaxed);
      float new_tau = tau;
      // ONE-SIDED: slow down on ringing only. The speed-up rule ("standing mean with r1 near +1")
      // was measured 2026-08-28 20:27 firing identically on both boards within a second -- it was
      // reading the COMMON-MODE timebase wander as sluggish tracking, and would have ratcheted
      // both to the floor. Loop lag and a moving target are indistinguishable on one device; the
      // wander is common-mode and cancels between devices, so chasing it faster buys nothing and
      // costs noise gain. The starting tau is the operator's; autotune only backs off from it.
      if (r1 < -0.25) {
        new_tau = std::min(tau * 1.25f, 60.0f);
      } else if (r1 > 0.7 && std::abs(mean) > 3.0 * sem) {
        ESP_LOGD(TAG, "SERVOTUNE sluggish-looking window (r1=%+.2f mean=%+.0f) -- not acted on, likely common-mode wander",
                 r1, mean);
      }
      if (new_tau != tau) {
        this->tune_tau_s_.store(new_tau, std::memory_order_relaxed);
        ESP_LOGW(TAG, "SERVOTUNE tau %.1f -> %.1f s (r1=%+.2f mean=%+.0f sd=%.0f us over %" PRIu32 " blocks) t=%" PRId64,
                 tau, new_tau, r1, mean, std::sqrt(std::max(var, 0.0)), st.at_n, now);
      } else {
        ESP_LOGD(TAG, "SERVOTUNE hold tau=%.1f s (r1=%+.2f mean=%+.0f sd=%.0f) t=%" PRId64, tau, r1, mean,
                 std::sqrt(std::max(var, 0.0)), now);
      }
      st.at_n = 0;
    }
  } else if (st.at_n != 0) {
    st.at_n = 0;
  }

  // Own short line, throttled by time -- never a tail on a report line (the 256-byte ceiling).
  if (now - st.dl_log_us >= DL_LOG_INTERVAL_US) {
    st.dl_log_us = now;
    ESP_LOGD(TAG, "DLLOOP err=%+" PRId64 " us trim=%+.2f int=%+.2f ppm kp=%.3f n=%" PRIu32 " dt=%.2f t=%" PRId64,
             st.dl_err_us, st.trim_applied_ppm, st.trim_integral_ppm, kp, n, dt_s, now);
  }
}
#endif  // USE_I2S_RATE_LOCK

// THREAD CONTEXT: player task. See FAST_SPLICE_RELEASE_US for the argument.
int32_t SnapcastClient::fast_splice_(ServoState &st, int64_t err_us, uint32_t sample_rate,
                                     bool hold, uint32_t horizon_chunks, bool measured) {
  // RESYNC WINDOW (ServoState::post_event_until_us): right after an event the error is a known
  // displacement, not wander, so the splice arms at resync_splice_us and immediately; in steady
  // state the configured threshold and the persistence wait keep it off the common wander.
  // Inside the resync window the coarse path does step-and-verify corrections (see the player loop);
  // the continuous splice is off so the two never act on the same block error (build 33 thrash).
  const bool post_event = now_us() < st.post_event_until_us;
  const int64_t cfg_threshold = static_cast<int64_t>(this->config_.fast_splice_threshold_us);
  const int64_t threshold = post_event ? 0 : cfg_threshold;  // 0 = disabled below
  const int64_t persist_us = FAST_SPLICE_PERSIST_US;
  const int64_t frame_us = sample_rate > 0 ? 1000000 / static_cast<int64_t>(sample_rate) : 0;
  int32_t applied = 0;

  const bool repair_settling =
      st.last_repair_us != 0 && now_us() - st.last_repair_us < FAST_SPLICE_REPAIR_HOLDOFF_US;
  // The converged gate exists so this path does not fight muted coarse convergence, which acts
  // on the same PREDICTED error. A MEASURED error carries no such conflict, and gating it created
  // a dead zone: unconverged, rate lock holding, err between the splice threshold and
  // converge_fine, nothing corrected at all -- measured 2026-08-28 19:47-19:51, board A creeping
  // at +20 us/s from +1.0 to +2.0 ms for 4.5 minutes under Never-Mute. Position correction on a
  // measured error is safe at any convergence state; the prediction keeps the gate.
  if (threshold > 0 && frame_us > 0 && (st.converged || measured) && !hold && !repair_settling) {
    // What the SIGNAL has not yet seen: splices applied within its measurement lag. The window is
    // the caller's horizon -- half the median window on the prediction, one pipeline depth plus
    // half the averaging block on err_tag -- summed over the newest entries of the ring, BEFORE
    // this chunk's own contribution is recorded below. Without the subtraction the loop keeps
    // correcting an error it has already fixed and overshoots: the limit cycle this path is on
    // record for.
    const uint32_t win = std::min<uint32_t>(horizon_chunks, ServoState::SPLICE_HIST);
    int32_t in_flight = 0;
    for (uint32_t i = 1; i <= win; i++) {
      in_flight += st.splice_hist[(st.splice_hist_idx + ServoState::SPLICE_HIST - i) % ServoState::SPLICE_HIST];
    }
    const int64_t in_flight_us = static_cast<int64_t>(in_flight) * frame_us;
    const int64_t effective_us = err_us - in_flight_us;
    if (st.fast_splice_active) {
      // Release inside half the ARM threshold: with the resync window arming at 100 us, a fixed
      // 300 us release sat ABOVE the arm point and the splice re-engaged every release (build 31
      // boot: engaged at -102, -104, -104 us in a row). Steady state keeps its 300 us.
      const int64_t release_us = std::min<int64_t>(FAST_SPLICE_RELEASE_US, threshold / 2);
      if (std::abs(effective_us) <= release_us || st.fast_splice_frames >= FAST_SPLICE_MAX_FRAMES) {
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
      } else if (now_us() - st.fast_splice_seen_us >= persist_us) {
        st.fast_splice_active = true;
        st.fast_splice_frames = 0;
        applied = effective_us > 0 ? 1 : -1;
        ESP_LOGI(TAG, "Fast splice engaged: %" PRId64 " us standing for %" PRId64
                      " s, correcting by position at one frame (%" PRId64 " us) per chunk t=%" PRId64,
                 effective_us, persist_us / 1000000, frame_us, now_us());
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
  st.post_event_until_us =
      now + static_cast<int64_t>(this->tune_resync_win_s_.load(std::memory_order_relaxed) * 1000000.0f);
    st.resync_step_at_us = 0;  // a fresh window: the ledger may take its first step
    std::fill(std::begin(st.win_step_at_us), std::end(st.win_step_at_us), 0);
  // Every event that re-arms the gain schedule -- a hard resync, a timebase re-anchor -- also
  // displaced audio or stepped the deadline mapping, so the tags of audio already in flight
  // describe the OLD placement. Blank the delay loop's tag stream for one pipeline depth, same as
  // a setpoint change. Repeated calls during a storm keep extending the blank, which is correct:
  // the mapping is still churning.
  this->playout_mutex_.lock();
  this->dl_blank_until_us_ = now + static_cast<int64_t>(this->tune_blank_ms_.load(std::memory_order_relaxed)) * 1000;
  this->dl_acc_n_ = 0;
  this->dl_acc_sum_us_ = 0.0;
  this->playout_mutex_.unlock();
}

// SHUTDOWN HOOK. THREAD CONTEXT: main loop, from the hub's on_shutdown(). Saves the current EMA
// so an OTA or a restart never restores a stale crystal estimate: the periodic save is gated to
// 10 min / 2 ppm, so the value on flash could be minutes to hours old at the moment of reboot.
void SnapcastClient::persist_now() {
#ifdef USE_I2S_RATE_LOCK
  float v = this->dl_integral_ema_mirror_.load(std::memory_order_relaxed);
  if (std::isfinite(v) && v != 0.0f && std::abs(v) <= TRIM_CLAMP_MAX_PPM && this->dl_integral_pref_.save(&v)) {
    global_preferences->sync();
    ESP_LOGI(TAG, "Delay loop: integral %+.2f ppm persisted at shutdown", v);
  }
#endif
}

// TUNING HOOK. THREAD CONTEXT: main loop (API); every consumer reads atomics. WARN-logged so the
// analyser's timeline carries each change; NOT persisted, so a reboot restores the flashed
// defaults -- a bad experiment is one power cycle from gone.
bool SnapcastClient::set_servo_param(const std::string &name, float value) {
  if (name == "tau_s") {
    if (!(value >= 2.0f && value <= 600.0f)) return false;
    if (this->tune_autotune_.load(std::memory_order_relaxed)) {
      ESP_LOGW(TAG, "SERVOPARAM tau_s set manually while autotune is ON; the next adaptation will move it");
    }
    this->tune_tau_s_.store(value, std::memory_order_relaxed);
  } else if (name == "ti_s") {
    if (!(value >= 10.0f && value <= 1200.0f)) return false;
    this->tune_ti_s_.store(value, std::memory_order_relaxed);
  } else if (name == "block_n") {
    if (!(value >= 8.0f && value <= 64.0f)) return false;
    this->tune_block_n_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "splice_us") {
    if (!(value == -1.0f || (value >= 0.0f && value <= 10000.0f))) return false;
    this->tune_splice_us_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "tag_stale_ms") {
    if (!(value >= 200.0f && value <= 10000.0f)) return false;
    this->tune_tag_stale_ms_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "blank_ms") {
    if (!(value >= 100.0f && value <= 2000.0f)) return false;
    this->tune_blank_ms_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "gap_blank_ms") {
    if (!(value >= 10.0f && value <= 500.0f)) return false;
    this->tune_gap_blank_ms_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "knee_us") {
    if (!(value >= 5.0f && value <= 1000.0f)) return false;
    this->tune_knee_us_.store(value, std::memory_order_relaxed);
  } else if (name == "tau_min_s") {
    if (!(value >= 2.0f && value <= 600.0f)) return false;
    this->tune_tau_min_s_.store(value, std::memory_order_relaxed);
  } else if (name == "align_max_us") {
    if (!(value >= 0.0f && value <= 20000.0f)) return false;
    this->tune_align_max_us_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
    if (value == 0.0f) {
      // Off means off: a bias left standing after the channel is disabled kept A's deadline
      // shifted -339 us at 15:56 with nothing able to clear it short of a reboot.
      this->render_bias_us_.store(0, std::memory_order_relaxed);
    }
  } else if (name == "align_apply") {
    // 0 = SHADOW: compute and log the step, apply nothing. The channel walked A's bias -159 ->
    // -339 us while the group delta GREW -124 -> -305 (15:43-15:56): self-reinforcing, sign still
    // unproven against the wire. Shadow it beside the wire until the sign is measured, then apply.
    this->tune_align_apply_.store(value != 0.0f, std::memory_order_relaxed);
  } else if (name == "align_gain") {
    if (!(value >= 0.0f && value <= 1.0f)) return false;
    this->tune_align_gain_.store(value, std::memory_order_relaxed);
  } else if (name == "align_deadband_us") {
    if (!(value >= 0.0f && value <= 1000.0f)) return false;
    this->tune_align_deadband_us_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "align_reject_us") {
    if (!(value >= 10.0f && value <= 20000.0f)) return false;
    this->tune_align_reject_us_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "align_step_us") {
    if (!(value >= 1.0f && value <= 200.0f)) return false;
    this->tune_align_step_us_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "resync_win_s") {
    if (!(value >= 0.0f && value <= 600.0f)) return false;
    this->tune_resync_win_s_.store(value, std::memory_order_relaxed);
  } else if (name == "resync_local_us") {
    if (!(value >= 100.0f && value <= 5000.0f)) return false;
    this->tune_resync_local_us_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "resync_close_s") {
    if (!(value >= 1.0f && value <= 60.0f)) return false;
    this->tune_resync_close_s_.store(value, std::memory_order_relaxed);
  } else if (name == "resync_reopen_us") {
    if (!(value >= 100.0f && value <= 5000.0f)) return false;
    this->tune_resync_reopen_us_.store(value, std::memory_order_relaxed);
  } else if (name == "resync_gain") {
    if (!(value >= 0.1f && value <= 1.0f)) return false;
    this->tune_resync_gain_.store(value, std::memory_order_relaxed);
  } else if (name == "resync_blank_ms") {
    if (!(value >= 50.0f && value <= 2000.0f)) return false;
    this->tune_resync_blank_ms_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "resync_splice_us") {
    if (!(value >= 20.0f && value <= 5000.0f)) return false;
    this->tune_resync_splice_us_.store(static_cast<int32_t>(value), std::memory_order_relaxed);
  } else if (name == "autotune") {
    this->tune_autotune_.store(value != 0.0f, std::memory_order_relaxed);
  } else if (name == "persist") {
    this->tune_persist_.store(value != 0.0f, std::memory_order_relaxed);
  } else {
    ESP_LOGW(TAG, "SERVOPARAM unknown parameter '%s'", name.c_str());
    return false;
  }
  ESP_LOGW(TAG, "SERVOPARAM %s=%.3f t=%" PRId64, name.c_str(), value, now_us());
  return true;
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
            // REJECT AN INCOHERENT SNAPSHOT RATHER THAN CORRECT IT. Measured 2026-08-28: this
            // check read a steady drift=-52041 on A and -52223 on B against +22 us on the samples
            // either side, 151 times on A alone, and the discriminator is in the same log line --
            // the bad samples carry age=48116 where the good ones carry age=25105.
            //
            // WHY THOSE SAMPLES ARE MEANINGLESS. The mixer stamps the composite with the SINK's
            // snapshot instant (its oldest term) but reads its own ring and transfer buffer NOW.
            // Between the two instants the sink renders R frames, and the stale sink terms still
            // count them, so the total over-states the T_now pipeline by R = the snapshot age
            // (audio renders in real time). The comment at the mixer's publish argues a mixed-age
            // sum is safe because "only what enters at the top or renders at the bottom moves the
            // number" -- but rendering at the bottom is exactly what happens across 48 ms, so the
            // argument holds only while the age is small.
            //
            // WHY NOT SIMPLY SUBTRACT THE AGE, which is what the arithmetic above suggests: the
            // accounting side is brought to `measured.as_of_us` by accounted_at_(), so subtracting
            // the age from the measurement would compare T_sink accounting against a T_now
            // measurement -- one mismatch traded for another. Making both coherent means either
            // ageing the accounting to now as well, or ageing the mixer's own terms backwards,
            // which is not possible from here. And ageing this data has three measured failures
            // behind it (see the DISPROVEN notes in the seed path), so a speculative correction is
            // the wrong instinct in this file specifically.
            //
            // A GATE COSTS NOTHING. This check runs per report and only ever needs to spot a
            // SUSTAINED discrepancy, so discarding the incoherent minority loses no sensitivity --
            // whereas feeding them in puts a -52 ms outlier into a signal whose real magnitude is
            // tens of microseconds. Bound set at one DMA span: past that the sink has certainly
            // rendered a descriptor's worth since the snapshot, which is where the mixed-age
            // argument breaks.
            // Computed EITHER WAY, so the RECON diagnostic below still carries the real number
            // next to the age that condemns it -- that pairing is what identified this in the first
            // place, and zeroing the field would have hidden it. Only `fill_comparable` is
            // withheld, and that is the flag which authorises action: it gates the self-repair and
            // the sync report's fill= field, and nothing else consumes the drift.
            fill_drift_us = static_cast<int32_t>(accounted_us - static_cast<int64_t>(measured.microseconds));
            const int64_t snapshot_age_us = now_us() - measured.as_of_us;
            if (snapshot_age_us < 0 || snapshot_age_us > DEPTH_SNAPSHOT_COHERENT_US) {
              fill_comparable = false;
            }
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
                     //
                     // TESTED 2026-08-27, AND REFUTED. n=4 forced resyncs on one board of a probed
                     // pair, MLS stimulus, analyser at sd 5.8 us:
                     //
                     //     d_pad +3547 frames -> predicted +80432 us, measured   +11.7 us
                     //     d_pad +5192        -> predicted +117734 us, measured +120.8 us
                     //     d_pad +4781        -> predicted +108414 us, measured  +45.8 us
                     //     d_pad +4849        -> predicted +109956 us, measured -159.7 us
                     //     slope +0.0008 against a predicted +1.0, r = +0.11
                     //
                     // Board B accrued 18369 frames (417 ms) of padding across the four; the net
                     // skew moved +18.6 us. Padding does NOT displace the output, because the
                     // repayment above (padding_debt_frames / padding_repay_at_us) already takes
                     // it back out -- the prediction was written as though padding were
                     // unaccounted, and that accounting exists a few hundred lines up.
                     //
                     // So pad= is a diagnostic, not a displacement term. Whatever plants the
                     // hundreds-of-us offsets, it is not this.
                     measured.dbg_padded_frames);
            // THE SAME COUNTERS ON A LINE SHORT ENOUGH TO SURVIVE. pad= is the last field of the
            // RECON line above, which is long enough that the logger truncates it mid-number --
            // it reads as "pad=882" then "pad=88" for a counter in the tens of millions, so the
            // one field the prediction above needs cannot be read from that line at all.
            ESP_LOGD(TAG, "PADDISP pad=%" PRIu32 " clamp=%" PRId64 " pushed=%" PRId64 " played=%" PRId64,
                     measured.dbg_padded_frames, this->dbg_clamped_frames_, dbg_pushed, dbg_played);
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
          } else if (st.dl_have_err && now_us() - st.dl_err_at_us < DL_ERR_STALE_US &&
                     now_us() >= st.tag_fault_until_us) {
            // TAGS LIVE: the ledger is diagnostic-only and a repair is pure harm. It steps
            // pushed_frames_total_ by the drift, the demoted prediction jumps, and the hard-resync
            // path moves REAL audio by that much against a bookkeeping artefact -- measured on A
            // 2026-08-28 21:37:52 / 21:38:15: repairs of -29026 then +29024 us (the mixer-ring drift
            // sawtooth), 1404 frames dropped then 1512 inserted, err_tag reading the true -32 ms in
            // between, an audible skip-then-stutter that "fixed itself" when the sawtooth flipped.
            // The 19:41 "-29 ms" event was the same repair. Disarm the window entirely so a stale
            // split does not fire the instant tags drop out; the fallback re-arms from scratch.
            st.drift_excess_since_us = 0;
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
            st.tag_fault_streak = 0;  // the ledger was the side that slipped; the tag path stands
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
      // DEDICATED LINE, not a field on the Sync report. The Sync line truncates at ~311 chars and
      // the trim parenthetical sits at its END: measured 2026-08-28, only 3 of 142 lines on one
      // board and 0 of 141 on the other survived intact, which silently turned every count taken
      // from that field -- railed, split-hold, gate -- into a count of "did the line happen to fit".
      // Several conclusions were drawn and retracted on the strength of it. See HANDOFF's "Log lines
      // truncate" trap, which says exactly this and predates the mistake.
#if defined(USE_I2S_RATE_LOCK) && defined(USE_SNAPCLIENT_TIMING_DIAG)
      if (st.rate_lock_ok) {
        ESP_LOGD(TAG,
                 "TRIMDBG applied=%+.2f ppm samples=%" PRIu32 " railed=%" PRIu32 " span=%+.0f..%+.0f "
                 "dl=%d dlups=%" PRIu32 " gate=%d lock=%d conv=%d err=%" PRId32,
                 this->rate_lock_->applied_ppm(), st.trim_samples, st.trim_railed,
                 st.trim_samples > 0 ? st.trim_min_ppm : 0.0f, st.trim_samples > 0 ? st.trim_max_ppm : 0.0f,
                 st.dl_active ? 1 : 0, st.dl_updates, st.gate_seen ? 1 : 0, st.gate_rate_lock_ok ? 1 : 0,
                 st.gate_converged ? 1 : 0, st.gate_median_err_us);
      } else {
        ESP_LOGD(TAG, "TRIMDBG rate_lock_ok=0 (no steering this report)");
      }
#endif
      char trim_str[112] = "";
  #ifdef USE_I2S_RATE_LOCK
      if (st.rate_lock_ok) {
#ifdef USE_SNAPCLIENT_TIMING_DIAG
        if (st.trim_samples > 0) {
          snprintf(trim_str, sizeof(trim_str),
                   ", trim %+.2f ppm (span %+.0f..%+.0f, railed %" PRIu32 "/%" PRIu32 ")",
                   this->rate_lock_->applied_ppm(), st.trim_min_ppm, st.trim_max_ppm, st.trim_railed, st.trim_samples);
        } else {
          // Say WHY there was no trim this window. "(idle)" alone is ambiguous between "the loop
          // had nothing to do" and "the loop is holding through a tag outage", and only the
          // second explains a flat frame-rate plateau while a peer keeps steering -- which is a
          // differential rate excursion of tens of ppm, measured on the wire.
          if (st.dl_updates == 0 && !st.dl_active) {
            snprintf(trim_str, sizeof(trim_str), ", trim %+.2f ppm (dl hold, no tags)",
                     this->rate_lock_->applied_ppm());
          } else if (st.gate_seen) {
            snprintf(trim_str, sizeof(trim_str), ", trim %+.2f ppm (gate: lock=%d conv=%d err=%" PRId32 " us)",
                     this->rate_lock_->applied_ppm(), st.gate_rate_lock_ok ? 1 : 0, st.gate_converged ? 1 : 0,
                     st.gate_median_err_us);
          } else {
            snprintf(trim_str, sizeof(trim_str), ", trim %+.2f ppm (idle, no chunks)",
                     this->rate_lock_->applied_ppm());
          }
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
          const TaggedRender tr = this->tagged_render_;
          const int64_t dl_blank = this->dl_blank_until_us_;
          const uint32_t tr_count = this->tagged_render_count_;
          this->tagged_render_count_ = 0;
          const uint32_t d_n = this->delay_n_;
          const double d_mean = this->delay_mean_us_;
          const double d_sd = d_n > 1 ? std::sqrt(this->delay_m2_us_ / static_cast<double>(d_n - 1)) : 0.0;
          double blk_sd[DELAY_BLOCK_LEVELS];
          uint32_t blk_n[DELAY_BLOCK_LEVELS];
          for (size_t lvl = 0; lvl < DELAY_BLOCK_LEVELS; lvl++) {
            DelayBlock &blk = this->delay_blocks_[lvl];
            blk_n[lvl] = blk.n;
            blk_sd[lvl] = blk.n > 1 ? std::sqrt(blk.m2 / static_cast<double>(blk.n - 1)) : 0.0;
            blk = DelayBlock{};
          }
          this->delay_n_ = 0;
          this->delay_mean_us_ = 0.0;
          this->delay_m2_us_ = 0.0;
          this->playout_mutex_.unlock();

          // DELAY: the measured transport delay averaged over the whole report window, with the
          // spread that produced it and the standard error of the mean. sem is the number that
          // decides whether this is controllable: a single observation jitters ~70 us, and if that
          // jitter is independent then sem = sd/sqrt(n) should reach single digits at n~334.
          // If sem does NOT shrink with n, the jitter is real phase movement and no amount of
          // averaging will help -- which is the same answer, arrived at honestly.
          if (d_n > 1) {
            const double sem = d_sd / std::sqrt(static_cast<double>(d_n));
            // sem assumes independence and is therefore a LOWER BOUND. DELAYBLK below is what says
            // whether it can be believed: read the sd column across block widths. Falling as
            // 1/sqrt(B) means independent and sem is honest; flattening at width B means the
            // effective sample size is n/B and the true standard error is sem*sqrt(B).
            ESP_LOGD(TAG, "DELAY mean=%.1f us sd=%.1f n=%" PRIu32 " sem=%.2f (lower bound)", d_mean, d_sd, d_n,
                     sem);
            char blkbuf[160];
            int bn = snprintf(blkbuf, sizeof(blkbuf), "DELAYBLK");
            for (size_t lvl = 0; lvl < DELAY_BLOCK_LEVELS && bn > 0 && bn < static_cast<int>(sizeof(blkbuf)); lvl++) {
              bn += snprintf(blkbuf + bn, sizeof(blkbuf) - bn, " %u:%.1f/%" PRIu32, 1u << lvl, blk_sd[lvl],
                             blk_n[lvl]);
            }
            ESP_LOGD(TAG, "%s", blkbuf);
          }

          // A tagged observation older than this describes a pipeline state that has since changed.
          //
          // TEN DESCRIPTORS, not a second. Tagged renders arrive at the DMA cadence -- one per 441
          // frames, ~10 ms -- for as long as tagged audio is reaching the DAC at all, so a reading
          // even a tenth of a second old does not mean "slightly stale", it means tagged audio
          // STOPPED. Measured on hardware 2026-08-28 during a pipeline disruption: at age 3.26 s the
          // original one-second gate correctly refused, but at age 0.42 s it passed an observation
          // that was wrong by 1.6 ms, in a report where only 13 tagged renders had arrived against a
          // normal 335. Both of those publish nothing at this bound.
          //
          // Symmetric because the age can legitimately go slightly negative: the speaker callback may
          // land between the TSF sandwich and this read. A LARGE negative age is not that, it is a
          // timestamp from somewhere else, and is refused the same way.
          constexpr int64_t RENDER_TAG_MAX_AGE_US = 100000;
          const int64_t tag_age_us = phase_local - tr.adjusted_ts_us;
          // The blank term refuses observations from a stall's late-stamped catch-up burst (see
          // FEEDBACK_GAP_BLANK_US): their age reads NORMAL because both sides of it are the same
          // late clock, so the age gate alone passed phases wrong by +141..+263 ms into the group.
          const bool tag_fresh = tr.adjusted_ts_us > 0 && tr.sample_rate > 0 &&
                                 tag_age_us < RENDER_TAG_MAX_AGE_US && tag_age_us > -RENDER_TAG_MAX_AGE_US &&
                                 tr.adjusted_ts_us >= dl_blank;

          // MEASURED render phase. The descriptor's real audio finished at tr.adjusted_ts_us, so its
          // FIRST real frame -- the one the tag names -- rendered tr.frames earlier; the tag says what
          // that frame's server time is. Both terms are observations of the same frame.
          //
          //   phase = TSF(first frame rendered) - (that frame's server audio time)
          //
          // `pushed` and `played` appear nowhere in it, which is the entire point: a bias in our own
          // frame ledger is then visible IN this number rather than invisible TO it.
          int64_t measured_phase = TsfSync::RENDER_PHASE_UNKNOWN;
          if (tag_fresh) {
            const int64_t rate = static_cast<int64_t>(tr.sample_rate);
            const int64_t first_frame_local = tr.adjusted_ts_us - static_cast<int64_t>(tr.frames) * 1000000 / rate;
            const int64_t render_tsf = first_frame_local + (phase_tsf - phase_local);
            const int64_t render_server = tr.server_ts_us + static_cast<int64_t>(tr.offset_frames) * 1000000 / rate;
            measured_phase = render_tsf - render_server;
          }

          // INFERRED render phase, kept for one purpose: so a single run can compare the two against a
          // known displacement rather than needing two firmwares. It reconstructs the rendering frame's
          // server time from (s_ts, pushed - played), so it is blind to the class of offset it exists to
          // remove -- measured 2026-08-28 at a ratio of 0.003 against inject_split(+1000 us), where a
          // truly external displacement read 1.000. DIAGNOSTIC ONLY. Nothing may act on it.
          int64_t inferred_phase = TsfSync::RENDER_PHASE_UNKNOWN;
          if (p_valid && p_played_ts > 0) {
            const int64_t render_tsf = p_played_ts + (phase_tsf - phase_local);
            const int64_t render_server = rec.server_ts_us - (p_pushed - p_played) * 1000000 /
                                                                 static_cast<int64_t>(rec.params.sample_rate);
            inferred_phase = render_tsf - render_server;
          }

          // Publish the measured one or nothing at all. Falling back to the inferred value would hand
          // render_align a signal that cannot see the displacement it is correcting, dressed as one
          // that can -- and the fallback conditions (a resampler, an announcement mixing over the top)
          // are exactly the moments a wrong correction would be hardest to attribute afterwards.
          // phase_local is the local instant of the TSF sandwich this phase was built from -- pass it
          // so the group only differences phases sampled close together.
          // AN OBSERVER PUBLISHES NO RENDER PHASE. It drives no DAC, its pipeline depth is not a
          // speaker's, and its phase sat +9.2..+10.1 ms from the two speakers' (2026-08-29 13:52-58)
          // -- in the group's weighted mean that made the speakers' render_group_delta bimodal
          // (A: median +5120 us, values near 0 half the time and near +9.5 ms the other half) and
          // unusable for steering. It still receives and logs everyone else's (PHASEIN).
          {
            const int64_t t_now = now_us();
            const bool in_transient = (st.kp_event_us != 0 && t_now - st.kp_event_us < PHASE_TRANSIENT_US) ||
                                      (st.resync_step_at_us != 0 && t_now - st.resync_step_at_us < PHASE_TRANSIENT_US);
            this->tsf_sync_->set_render_phase_broadcast(!in_transient);
          }
          if (measured_phase != TsfSync::RENDER_PHASE_UNKNOWN && !this->config_.tsf_observer) {
            // THE RAW PHASE, NOT A "SETTLED" ONE. Build 45 published phase - err_tag so that align (which
            // moves the deadline and then waits tau = 120 s for the PI to move the audio) would stop
            // stepping into a gap already corrected. It also zeroed the resync gate's evidence by
            // construction -- a board 1443 us early reported a group delta of +45 and every tag step
            // was refused (00:15, RSTEP src=tag ok=0) -- because the gate asks the opposite question:
            // is this error differential NOW. Align's lag needs the peers' err_tag, not ours alone;
            // that is a beacon-format change, not a subtraction here.
            this->tsf_sync_->set_render_phase_us(measured_phase, phase_local);
          } else {
            this->tsf_sync_->set_render_phase_us(TsfSync::RENDER_PHASE_UNKNOWN);
          }

          // SHADOW ERROR: what the servo's error signal WOULD be if it were measured rather than
          // predicted. Computed, logged, and acted on by nothing.
          //
          // The live error is `predict_next_play_us_() - deadline`: a model of when audio will
          // render, extrapolated from an EWMA pivot along the nominal slope. Every defence around
          // it -- the accounting split, the 3 s trim hold, the repair that steps
          // pushed_frames_total_, the starvation re-baseline -- exists because that model can
          // diverge from reality. A render tag measures the same quantity directly, so there is
          // nothing for a split to be a split BETWEEN.
          //
          // Two terms are worth watching in the comparison, both already documented as costs of the
          // predicted form in predict_next_play_us_():
          //   * the nominal-vs-realised slope bias, called out there as ~70% of the differential
          //     floor, and unremovable because the prediction depends on the model. A measured
          //     render instant has no slope at all.
          //   * loop delay. The pivot lags ~141,000 frames (~3.2 s); a tagged observation lags one
          //     pipeline depth (~250-300 ms). The measurement is an order of magnitude fresher.
          //
          // deadline() is linear in server time for a fixed buffer and offset, so the tagged
          // frame's target is the last chunk's target plus their server-time difference. Exact, and
          // free of a second call into chunk_deadline_us_(), which mutates state.
          if (tag_fresh && st.last_deadline_server_ts != 0) {
            const int64_t rate_i = static_cast<int64_t>(tr.sample_rate);
            const int64_t tag_server_us = tr.server_ts_us + static_cast<int64_t>(tr.offset_frames) * 1000000 / rate_i;
            const int64_t tag_local_us = tr.adjusted_ts_us - static_cast<int64_t>(tr.frames) * 1000000 / rate_i;
            const int64_t tag_deadline = st.last_deadline_us + (tag_server_us - st.last_deadline_server_ts);
            const int64_t shadow_err = tag_local_us - tag_deadline;
            ESP_LOGD(TAG, "SHADOW err_tag=%" PRId64 " err_live=%" PRId64 " diff=%" PRId64 " age=%" PRId64,
                     shadow_err, median_err_us, shadow_err - median_err_us, tag_age_us);
          }

#ifdef USE_SNAPCLIENT_TIMING_DIAG
          // The grading line for this signal. `ratio` cannot be computed on-device -- it needs the
          // wire -- so log both phases and the tagged-render rate side by side and take the ratio
          // offline against an injected displacement. tags=0 means no tagged audio reached the DAC in
          // this window, which is a configuration answer (resampler, mixer blending), not a fault.
          //
          // RENDER_PHASE_UNKNOWN prints as `unknown`, never as its INT64_MIN value. Printing the
          // sentinel as a number invites whatever reads this log to subtract it from something, and
          // 2^63 of overflow then looks like a measurement: it cost a wrong diagnosis here before the
          // line was changed.
          char measured_str[24], inferred_str[24], age_str[24];
          format_render_phase_(measured_str, sizeof(measured_str), measured_phase);
          format_render_phase_(inferred_str, sizeof(inferred_str), inferred_phase);
          if (tr.adjusted_ts_us > 0) {
            snprintf(age_str, sizeof(age_str), "%" PRId64, tag_age_us);
          } else {
            snprintf(age_str, sizeof(age_str), "none");
          }
          ESP_LOGD(TAG,
                   "RENDERTAG measured=%s inferred=%s tags=%" PRIu32 " age=%s frames=%" PRIu32 " off=%" PRIu32
                   " sup=%d",
                   measured_str, inferred_str, tr_count, age_str, tr.frames, tr.offset_frames,
                   this->audio_listener_ != nullptr && this->audio_listener_->on_supports_render_tags() ? 1 : 0);
#else
          (void) tr_count;
          (void) inferred_phase;
#endif
        } else {
          // THE TSF SAMPLE FAILED, and the delay accumulators must still be cleared.
          //
          // They were reset only on the success path, so a failed sample left them accumulating
          // across reports: n reached 668, 1002, 1336 and 1673 on one board -- exact multiples of
          // the ~334 per report -- and sd inflated with the extra drift each spanned. Every
          // statistic from such a report described a window of unknown, unreported length. An
          // accumulator whose reset is conditional on an unrelated success is a silent one.
          this->playout_mutex_.lock();
          this->delay_n_ = 0;
          this->delay_mean_us_ = 0.0;
          this->delay_m2_us_ = 0.0;
          for (auto &blk : this->delay_blocks_) {
            blk = DelayBlock{};
          }
          this->playout_mutex_.unlock();
        }
        // tsf= now reports the CONSENSUS, not a role. n is how many raw estimates the adopted
        // mapping averages, our own included: 1 means nobody else is audible and we are playing
        // to our own line, >=2 means a genuinely shared timebase. There is no leader to name.
        const uint8_t consensus_n = this->tsf_sync_->consensus_n();
        const float age_s = this->tsf_sync_->mapping_age_s(now_us());
        const int64_t own_phase = this->tsf_sync_->render_phase_us();
        const int32_t group_delta = this->tsf_sync_->render_group_delta_us();
        if (consensus_n == 0) {
          snprintf(tsf_str, sizeof(tsf_str), ", tsf=inactive(kalman)");
        } else if (consensus_n < 2) {
          snprintf(tsf_str, sizeof(tsf_str), ", tsf=solo(%.1fs, peers %u, phase %s)", age_s,
                   this->tsf_sync_->peer_count(),
                   own_phase == TsfSync::RENDER_PHASE_UNKNOWN ? "unknown" : "set");
        } else {
          // depth= compares buffer OCCUPANCY against the peer mean and is the only instrument
          // that can see an absolute playout offset, which the sync median cannot show by
          // construction. render= is the one to trust of the two; depth= is kept alongside it
          // precisely so the two can be compared against the analyser before anything acts on
          // either. Both are INT32_MIN until the group has published enough to compare.
          const int32_t depth_delta = this->tsf_sync_->pipeline_delta_us();
          char depth_buf[24] = "none";
          char render_buf[24] = "none";
          if (depth_delta != INT32_MIN) {
            snprintf(depth_buf, sizeof(depth_buf), "%+" PRId32, depth_delta);
          }
          if (group_delta != INT32_MIN) {
            snprintf(render_buf, sizeof(render_buf), "%+" PRId32, group_delta);
          }
          snprintf(tsf_str, sizeof(tsf_str), ", tsf=consensus(n%u, %.1fs, depth %s render %s us)", consensus_n,
                   age_s, depth_buf, render_buf);
        }
        if (this->config_.tsf_observer) {
          this->tsf_sync_->log_phase_inputs(now_us());
        }
        // NOTHING NULLS THE DIFFERENCE BETWEEN TWO DEVICES otherwise: each servo drives its OWN
        // error against server time to zero, so a hard resync's residual is held forever by a
        // servo that is already at its setpoint and reports a clean median.
        //
        // Correct the DEADLINE, not the audio. Shifting this device's target lets the existing
        // servo close the gap with its normal trim; a second controller splicing frames would
        // fight the first for the same audio. Gain is deliberately low and the step is clamped:
        // this runs once per report, and the failure mode to avoid is two devices chasing each
        // other.
        //
        // TOWARD THE GROUP AVERAGE. It used to be the leader's phase, which referenced every
        // correction to whoever held the crown while the crown moved six times in seventeen
        // minutes; then a group MEDIAN, which is discontinuous over three values and hopped
        // +-96 us on data sitting at +-12. It is a robustly weighted mean now -- see
        // TsfSync::render_group_delta_us().
        //
        // ONLY WHILE CONVERGED: during a forced 500 ms displacement the loop spent ten reports
        // walking its bias toward a delta that vanished when the displacement was removed. The
        // servo owns the transient; this owns the standing offset the servo cannot see.
        //
        // Only every Nth report: the loop delay is ~2 reports, so correcting on every one means
        // acting on a measurement that does not yet contain the previous correction.
        this->render_align_tick_++;
        const bool align_due = (this->render_align_tick_ % RENDER_ALIGN_EVERY_N_REPORTS) == 0;
        // Cap, gain and deadband are runtime tunables (servo_param align_max_us / align_gain /
        // align_deadband_us); align_max_us defaults to the YAML render_align_max (0 = off).
        const int32_t align_cap = this->tune_align_max_us_.load(std::memory_order_relaxed);
        if (align_cap > 0 && st.converged && align_due && group_delta != INT32_MIN) {
          const int32_t cap = align_cap;
          int32_t bias = this->render_bias_us_.load(std::memory_order_relaxed);
          // SIGN, MEASURED on the wire with a single-board step (2026-08-29 17:03-17:10): removing
          // A's +60 us bias moved the wire B-A from +105 to +8 us, so a POSITIVE bias makes this
          // board play EARLIER on the wire. And while A carried that +60 the wire read +140 (A early)
          // with A's group delta at +58: a POSITIVE delta means this board is EARLY. Early -> must
          // play later -> bias must go NEGATIVE: bias -= delta*gain. That is the original code; the
          // build-23 flip (bias += delta*gain) was read off a run whose group phase was polluted,
          // and both applied runs after it made an early board earlier (15:25-15:56 runaway;
          // 16:48-17:03 wire +140..+173) while the delta tracked the wire at r = +0.96 -- the
          // measurement was right, the correction inverted. Do not re-derive this from the phase
          // formula; measure it.
          // Pairs beyond align_reject_us are ignored (a single +1717 us pair once moved the bias
          // 41 us) and the step is capped at align_step_us: this channel removes a standing offset
          // slowly and must never be able to create one quickly.
          const int32_t reject = this->tune_align_reject_us_.load(std::memory_order_relaxed);
          const int32_t max_step = this->tune_align_step_us_.load(std::memory_order_relaxed);
          if (std::abs(group_delta) > this->tune_align_deadband_us_.load(std::memory_order_relaxed) &&
              std::abs(group_delta) <= reject) {
            // FRACTIONAL ACCUMULATION. gain x delta truncated to whole microseconds made anything
            // under 1/gain of delta a zero step: at gain 0.1 a standing 8 us (A, 06:00-07:00, the
            // wire's -10 us mean, seen by the delta every cycle) was never corrected. The fraction
            // carries over so small gains act on small deltas at the rate they imply -- gain must be
            // SMALL here (~0.03/cycle): the correction reaches the audio through the PI's tau = 120 s,
            // and 0.1 per 10 s on that lag hunted at +-100 us (2026-08-29 evening).
            this->render_align_frac_ += -static_cast<float>(group_delta) * this->tune_align_gain_.load(std::memory_order_relaxed);
            const int32_t step_i = static_cast<int32_t>(this->render_align_frac_);  // toward zero
            const int32_t step = std::clamp<int32_t>(step_i, -max_step, max_step);
            this->render_align_frac_ -= static_cast<float>(step);
            if (this->tune_align_apply_.load(std::memory_order_relaxed)) {
              const int32_t bias_before = bias;
              bias = std::clamp<int32_t>(bias + step, -cap, cap);
              this->render_bias_us_.store(bias, std::memory_order_relaxed);
              st.align_kick_us += static_cast<float>(bias - bias_before);  // see ALIGN KICK in delay_loop_update_
            } else {
              ESP_LOGD(TAG, "RALIGN shadow: group %+" PRId32 " would step %+" PRId32 " us (bias held %+" PRId32 ")",
                       group_delta, step, bias);
            }
          }
          ESP_LOGD(TAG, "RALIGN group %+" PRId32 " -> bias %+" PRId32 " us (cap %" PRId32 ")", group_delta, bias,
                   cap);
        }
      }
  #endif
      // SPLIT ACROSS TWO LINES, and this is not cosmetic. The formatting ceiling is 256 bytes of
      // message; the single combined line ran to exactly that on 140 of 144 reports and was cut
      // mid-token. Everything at its tail -- trim, tsf, split, tbjit -- was therefore absent most
      // of the time, and worse, the analyser's SYNC_RE REQUIRES the trim field, so a truncated
      // line was dropped whole and took that report's frame corrections, hard resyncs and pipeline
      // steps with it. Silent, and it invalidated several conclusions before it was noticed.
      //
      // The first line carries what must never be lost: the error summary and the correction
      // counts. Everything derived or diagnostic goes on SYNCX, which is short enough to survive.
      //
      // trim_str is deliberately NOT here. Keeping it produced a 311-byte line again as soon as the
      // error fields widened -- measured post-split at 14:40:26 -- because a bounded-looking field
      // plus unbounded numbers still reaches the ceiling. TRIMDBG carries the trim series, and the
      // analyser now reads it from there, so this line has no unbounded tail left to lose.
      ESP_LOGD(TAG,
               "Sync: avg %" PRId64 " us, peak %" PRId64 " us, median %" PRId64
               " us | corrected -%" PRIu32 "/+%" PRIu32 " frames, %" PRIu32 " hard resyncs over %" PRIu32 " chunks",
               st.err_accum_us / st.err_count, st.err_peak_us, median_err_us, st.soft_dropped_frames,
               st.soft_inserted_frames, st.hard_resyncs, st.err_count);
      ESP_LOGD(TAG, "SYNCX feedback %" PRId64 " us mean / %" PRId64 " ms max, buffered %" PRIu32 " ms, pipeline %" PRId32
                    " ms%s%s%s%s",
               fb_mean_gap_us, max_gap_us / 1000, buffered_ms, pipeline_ms, fill_str, drift_str, dl_str, tsf_str);
      st.err_accum_us = 0;
      st.err_peak_us = 0;
      st.err_count = 0;
      st.soft_dropped_frames = 0;
      st.soft_inserted_frames = 0;
      st.hard_resyncs = 0;
  #ifdef USE_I2S_RATE_LOCK
      st.trim_samples = 0;
      st.dl_updates = 0;
      st.gate_seen = false;
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
    // REVERTED, MEASURED: predicting with the REALISED slope (nominal / (1 + applied_ppm)) instead
    // of the nominal one. The arithmetic argument was sound -- the pivot lags ~141,000 frames and
    // crossing that lever with the wrong slope is the 3.15 us/ppm bias -- and the prediction really
    // was more exact. It still destabilised the loop, within two minutes on hardware:
    //
    //     trim  a  ~50-65 ppm -> +164.9 ppm      median  a  +-20 us -> oscillating
    //     trim  b  ~50 ppm    -> +23 / +72 ppm   median  b  -103, -107, +47 us
    //
    // Why: the nominal slope's INSENSITIVITY TO TRIM was load-bearing. It kept the controller's
    // own output out of its error signal except through the feedback pivot, which is slow. Making
    // `predicted` depend instantly on applied_ppm adds direct feedthrough from the controller to
    // the error it is measuring, and against the 31-chunk median's ~0.4 s lag that oscillates.
    //
    // The term is still worth removing -- it is ~70% of the differential floor -- but as the
    // frames-based comparison step 4 actually specifies, with KP and KI re-derived for the loop
    // that results. Not as a slope swap into a loop tuned around the old plant.
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

void SnapcastClient::set_stream_identity(const std::string &stream_name) {
  // Empty means the control session has not reported a stream yet; keep 0 ("unknown"), which
  // groups with anyone rather than isolating this device from the timebase.
  const uint32_t h = stream_name.empty() ? 0u : fnv1_hash(stream_name);
  const uint32_t prev = this->stream_id_hash_.exchange(h, std::memory_order_relaxed);
  if (prev != h) {
    ESP_LOGI(TAG, "TSF stream scope: '%s' (%08" PRIx32 ") -- leadership scoped to it", stream_name.c_str(), h);
  }
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
      // A SOURCE SWITCH IS A TIMEBASE STEP OF THE LOCAL-VS-SHARED DISAGREEMENT, and nothing else
      // announces it: it is not a consensus move, so the step reporter and the epoch stay silent.
      // Measured 2026-08-28 19:41: A switched source with the two mappings 29 ms apart -- deadline
      // and published phase both stepped 29 ms, the delay loop (correctly) refused it for a
      // minute, and the coarse machinery walked the audio over audibly. Flag it; the player loop
      // turns it into the same re-arm + tag-stream blank a real re-anchor gets.
      if (!this->deadline_on_shared_tsf_) {
        this->deadline_source_switched_ = true;
      }
      this->deadline_on_shared_tsf_ = true;
      // Follower-side inter-device correction; 0 unless render_align_max is configured. Applied
      // only here, on the shared-TSF path: without a shared mapping the two devices' phases are
      // not comparable and the bias would be meaningless.
      return rec.server_ts_us + buffer_us - shared_offset_us +
             this->render_bias_us_.load(std::memory_order_relaxed);
    }
    if (this->deadline_on_shared_tsf_) {
      this->deadline_source_switched_ = true;
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
  this->player_phase_.store(static_cast<uint8_t>(PlayerPhase::DISCARD), std::memory_order_relaxed);
  int64_t starve_since = 0;
  while (bytes > 0 && !this->shutdown_.load(std::memory_order_relaxed)) {
    size_t n = this->pcm_ring_->read(this->slice_buffer_.get(), std::min(bytes, SLICE_BUFFER_SIZE),
                                     pdMS_TO_TICKS(100));
    bytes -= n;
    // Same silent-unbounded-wait problem as push_chunk_'s read, and the same fix: this is the other
    // place the player task can disappear into without a word.
    if (n == 0) {
      if (starve_since == 0) {
        starve_since = now_us();
      } else if (now_us() - starve_since >= PUSH_STALL_LOG_US) {
        starve_since = now_us();
        ESP_LOGW(TAG, "discard_ring_bytes_ starved: %zu bytes still to drop, ring holds %zu", bytes,
                 this->pcm_ring_->available());
      }
    } else {
      starve_since = 0;
    }
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
    // Explicitly untagged. This silence is ours, not the server's, so it corresponds to no server
    // time -- and the sink must skip a descriptor that starts in it rather than extrapolate the
    // previous chunk's identity across it, which would read the insertion as audio arriving late.
    this->audio_listener_->on_set_render_tag(audio::RenderTag{});
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
    // Untagged for the same reason as inserted silence: a repeated frame is the servo's, not the
    // server's, and giving it the neighbouring chunk's identity would hide the very splice it is.
    this->audio_listener_->on_set_render_tag(audio::RenderTag{});
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
  // Bytes of THIS CHUNK already taken out of the ring, so the frame index of anything in the current
  // slice is (chunk_bytes_done + offset) / frame_bytes. Dropped frames count here: a drop moves the
  // audio that follows it earlier in the chunk, and the identity has to say so.
  size_t chunk_bytes_done = 0;
  // The rate the tag's frame offsets are counted in. Published rather than assumed, because the
  // observation comes back on another thread after this chunk's params are out of scope.
  this->tag_sample_rate_.store(rec.params.sample_rate, std::memory_order_relaxed);

  while (remaining > 0 && !this->shutdown_.load(std::memory_order_relaxed)) {
    const size_t want = std::min(remaining, SLICE_BUFFER_SIZE);
    size_t got = 0;
    // UNBOUNDED BY DESIGN -- emit_pcm_ writes a chunk's PCM before posting its record, so a popped
    // record's bytes are guaranteed present and this loop must not give up on them. But "must not
    // give up" and "must not say anything" are different things: three wedges today ended with the
    // player task silent from the disconnect onward, and a silent unbounded wait cannot be told
    // from a dead task. Say where it is stuck.
    int64_t starve_since = 0;
    this->player_phase_.store(static_cast<uint8_t>(PlayerPhase::RING_READ), std::memory_order_relaxed);
    while (got < want && !this->shutdown_.load(std::memory_order_relaxed)) {
      const size_t n = this->pcm_ring_->read(this->slice_buffer_.get() + got, want - got, pdMS_TO_TICKS(100));
      got += n;
      if (n == 0) {
        if (starve_since == 0) {
          starve_since = now_us();
        } else if (now_us() - starve_since >= PUSH_STALL_LOG_US) {
          starve_since = now_us();
          ESP_LOGW(TAG,
                   "push_chunk_ starved: %zu of %zu bytes for this chunk, ring holds %zu, output_active=%d "
                   "-- the record was posted so the PCM should be here",
                   got, want, this->pcm_ring_->available(),
                   this->output_active_.load(std::memory_order_relaxed) ? 1 : 0);
        }
      } else {
        starve_since = 0;
      }
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
    this->player_phase_.store(static_cast<uint8_t>(PlayerPhase::WRITE), std::memory_order_relaxed);
    while (offset < got) {
      if (this->audio_listener_ == nullptr || !this->output_active_.load(std::memory_order_relaxed) ||
          this->shutdown_.load(std::memory_order_relaxed)) {
        // Consumer went away mid-chunk: discard the rest, deadlines keep us honest
        this->discard_ring_bytes_(remaining);
        return;
      }
      // Identify this payload's FIRST frame: which chunk it belongs to, and how far into that chunk
      // it sits. Both halves are needed -- a DMA descriptor is 441 frames against a chunk's 1152, so
      // descriptors straddle chunks and a bare chunk id could not place the boundary.
      //
      // Re-stated on every write rather than once per chunk. That is not redundant: a write may be
      // short, and the position the audio actually landed at is the only one worth recording. Runs
      // that do turn out contiguous are collapsed downstream (audio::RenderTagTrack), so restating
      // costs nothing.
      //
      // A non-positive server timestamp cannot be a tag: zero is the untagged marker, and a negative
      // one would cast to an enormous positive and read as valid. Leaving such a chunk untagged costs
      // one reading; letting it through would cost a wrong one.
      this->audio_listener_->on_set_render_tag(
          rec.server_ts_us > 0 ? audio::RenderTag{static_cast<uint64_t>(rec.server_ts_us),
                                                 static_cast<uint32_t>((chunk_bytes_done + offset) / frame_bytes)}
                               : audio::RenderTag{});
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
    chunk_bytes_done += got;
  }
}

}  // namespace esphome::snapclient

#endif  // USE_ESP32
