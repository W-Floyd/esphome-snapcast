#include "tsf_sync.h"

#include <string>

#ifdef CLOCK_SYNC_TSF_ACTIVE

#include "consensus_math.h"
#include "esphome/core/log.h"

#include <esp_timer.h>
#include <esp_wifi.h>
#include <fcntl.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <limits>

namespace esphome::clock_sync {

static const char *const TAG = "snapclient.tsf";

// Multicast rendezvous for the mapping packets; TTL 1 (never routed)
static const char *const TSF_GROUP = "239.255.83.84";
static constexpr uint16_t TSF_PORT = 47083;

static constexpr uint32_t TSF_MAGIC = 0x534E5446;  // 'SNTF'
// Bump on a wire change that could make a receiver MISREAD a timebase field. Widening
// pipeline_us from int16 to int32 was not one: it is the last field in the packet, so every
// field the timebase depends on stays at its old offset, and the only thing an older receiver
// gets wrong is the low half of a diagnostics-only value it never feeds into the mapping.
//
// It was bumped anyway, and that cost more than it saved. Rejecting on version makes a
// half-flashed fleet lose the SHARED TIMEBASE outright -- and losing it is expensive: a group
// whose publisher goes quiet leaves every follower unable to unmute, measured at ~50 s of
// silence with each device's own servo already in band. Without the bump the same rollout
// degrades one direction only (a new receiver drops an old sender's shorter packet on the runt
// check) instead of both, and the timebase keeps flowing the other way.
//
// So: amend in place, and reserve the bump for a change that would genuinely be misread.
static constexpr uint8_t TSF_VERSION = 1;

// Pipeline-depth divergence alarm. Absolute playout depth is the one quantity the
// sync median CANNOT see: the median is measured against this device's own
// predicted playout, so if the frame accounting is offset by F, the prediction is
// offset by F too and the error reads ~0 while the audio is physically F late.
// Observed: one device sat at 72-118 ms of pipeline against a fleet norm of ~250 ms
// and was audibly ~150 ms behind its pair, with textbook-clean sync reports on every
// device. The group is the only available reference, so each device compares its own
// depth against the mean of its peers' and shouts when it diverges.
//
// 100 ms -> 5 ms. The old value was sized against the spread of the INSTANTANEOUS depth
// (224-282 ms across four boards = 58 ms), which was almost entirely sampling phase: the
// accounted queue sawtooths by about a chunk, so two devices sampled out of phase differed
// by up to +-50 ms of pure artefact. Publishing the window MEAN removed that, and the
// residual spread has since been characterised over a full session: |delta| median 318 us,
// p95 ~1 ms. 5 ms is an order of magnitude above that floor.
//
// The old threshold left the alarm blind to everything under 100 ms, which is where this
// defect actually lives. Three real offsets were found on a logic analyser hours after the
// fact -- 8.5 ms, 10 ms and 13.4 ms, every one sustained, every one silent here. Each was
// planted by a re-baseline anchoring to a per-device instantaneous measurement, and each
// was invisible to every other metric on the device by construction.
//
// This is a WARN and nothing else: no control action is taken on it, and none should be
// without a far better instrument. The delta is directionally right but biased -- it read
// -13.4 ms for an offset a logic analyser measured at 3.7 ms -- so it is fit to raise an
// alarm and unfit to size a correction.
static constexpr int32_t PIPELINE_DIVERGE_US = 5000;
static constexpr int64_t PIPELINE_DIVERGE_MIN_US = 5000000;
static constexpr int64_t PIPELINE_DIVERGE_LOG_US = 30000000;
static constexpr int32_t PIPELINE_UNKNOWN = INT32_MIN;

// REVERTED to 1 Hz (2026-08-28) after the experiment answered its question. Kept in full because
// the answer is worth having: 30 Hz measured BENIGN -- sd 2.319 us against 1.694 at 1 Hz and 6.326
// before the KP change, zero disconnects across 21 minutes, and peer phase ages fell from ~1000 ms
// to 46-111 ms (so an effective ~10-20 Hz, service() call frequency being the real limit).
//
// THAT RETIRES RADIO CONTENTION as the explanation for the 15x follower-beacon regression, which
// was the one hypothesis never excluded when broadcast_()'s trailing adopt_() was identified as the
// cause (27ad3de). 30x the traffic costs nothing measurable, so the adopt_() explanation stands.
//
// Reverted because it buys nothing with a consumer today: its only real benefit is render-phase
// PAIRING, and render_align is blocked on ledger-independence, so there is nothing to consume the
// extra pairings. Carrying a 30x traffic increase for a benefit nothing uses is risk without
// return -- particularly with one board at -42 dBm. Put it back the day the tag-echo signal lands.
//
// The original reasoning follows, unchanged, because it is what the re-attempt will need:
// EXPERIMENT (2026-08-28): 1 Hz -> 30 Hz, deliberately extreme, to see what the beacon rate is
// worth and to settle a question open since the leaderless work started. WALK THIS BACK unless it
// measures better; 1 Hz is the value everything else was measured at.
//
// WHAT IT SHOULD AND SHOULD NOT BUY. The consensus averages LINES carrying drift_ppm, so a stale
// peer line is extrapolated rather than stale-valued, and 1 s of staleness costs only
// (drift error x 1 s) ~= 1 us. Against that, the measured spread between the three devices' raw
// estimates is 137-920 us. So the beacon rate is NOT the limiting term for the timebase and 30x
// cannot make it one. What it should buy is elsewhere:
//   * RENDER-PHASE PAIRING. PHASE_PAIR_WINDOW_US is 300 ms and our own phase is written once per
//     sync report (~3.3 s), and only ~25% of arrivals currently land inside the window -- measured
//     as "the consumer essentially never saw one; zero corrections ran". 30x the arrivals is 30x
//     the pairing chances, and that is the signal render_align needs.
//   * The one residual path-dependence in the deterministic consensus: devices differ only while
//     they hold different estimate SETS, which is bounded by this interval.
// It also settles whether the 15x sd regression once measured from follower beaconing was really
// the trailing adopt_() (removed) or radio-time contention (never excluded). If sd degrades at
// 30 Hz, contention was real.
//
// THE RISK, stated because it is specific: B currently sits at -42 dBm against -31/-30 for the
// other two and is the board that starves, so 30x the multicast+unicast traffic is most likely to
// hurt exactly the weakest link.
static constexpr int64_t BEACON_INTERVAL_US = 1000000;   // every device publishes at this rate
static constexpr int64_t MAPPING_EXPIRY_US = 5000000;    // stale mapping → Kalman fallback
// A tsf<->server line's drift is the DIFFERENCE OF TWO CRYSTAL RATES, so it is bounded by physics:
// the tsf-vs-local term is already rejected past ±100 ppm ("larger = TSF discontinuity") and the
// Kalman term is the same kind of quantity. ±200 ppm allows both at their limit and still rejects
// everything seen misbehaving. For scale, the servo's own crystal integral clamps at 200 ppm.
//
// Measured 2026-09-02: an offset STEP (reconnect, server restart, stream change) re-seeds the
// Kalman drift across the discontinuity, and the significance gate that admits it is inverted in
// practice -- a real +43 ppm crystal drift NEVER clears `drift² > 4·drift_cov` (0 of 5000 clean
// samples), while a 30 ms step reaches +322 ppm and a 180 ms step +1460 ppm, which do. So the
// filter publishes drift only when the drift is WRONG. Observed on the wire: -1158, +1462, +5059,
// +10930, +61641 and -179896 ppm from whichever board had most recently rebooted, adopted by peers
// with no rejection, dragging their mappings ~1.6 ms/s until two boards sat 250 ms apart.
static constexpr float MAX_PLAUSIBLE_DRIFT_PPM = 200.0f;
static constexpr int64_t SHARED_HOLD_GRACE_US = 3000000;  // a TSF-sample blip shorter than this keeps the shared deadline (see shared_server_offset_us)
// MUST TRACK THE BEACON RATE. service() returns early inside this interval, so it is a hard ceiling
// on the beacon rate no matter what BEACON_INTERVAL_US says -- at 200 ms the beacons would cap at
// 5 Hz and the experiment would silently not happen. service() is called per arriving message
// (~38/s), so 20 ms is reachable.
static constexpr int64_t SERVICE_MIN_INTERVAL_US = 200000;
// How often the consensus is recomputed and re-adopted. Faster than the beacon rate on purpose:
// the slew below is expressed per second, so a coarse cadence would make each adoption step
// bigger for the same tracking speed, and a fresh adoption is also what keeps the mapping inside
// MAPPING_EXPIRY_US when beacons are lost.
// Also tracks the beacon rate: adopting at 2 Hz while beacons arrive at 30 Hz would throw away
// most of what the experiment is buying. Still slower than the beacon rate, since adoption is a
// pure function of the live set and gains nothing from running faster than the set changes.
static constexpr int64_t CONSENSUS_INTERVAL_US = 500000;
static constexpr int64_t CONSENSUS_LOG_INTERVAL_US = 10000000;
// Age clamp on TSF extrapolation: an AP reboot resets TSF to ~zero with the BSSID
// unchanged, leaving tsf_base "hours in the future" — evaluating that mapping would
// produce garbage deadlines (guaranteed hard-resync mute) until the next anchor
static constexpr int64_t MAX_EXTRAPOLATION_US = 10000000;
// Sandwich width above which a TSF sample was interrupted mid-read; retry.
// After all retries, the narrowest attempt is still accepted up to the loose
// bound (the servo median absorbs the noise; a drop to Kalman fallback is worse).
// The bracket is dominated by the DETERMINISTIC cost of esp_wifi_get_tsf_time(), not by
// jitter. Measured on four devices: median 42 us, full range 42-49 us. Two things follow.
//
// First, a threshold below ~42 us is unreachable, so demanding one just burns retries: an
// earlier attempt at 20 us with 8 attempts spent ~340 us of blocking work per deadline and
// returned the same best-of value it would have accepted immediately.
//
// Second, the error model behind that attempt was wrong. A CONSISTENT bracket means the TSF
// is latched at a consistent point within the call, so the midpoint carries a consistent
// BIAS -- and identical devices share that bias, so it cancels between them. The noise that
// actually matters is the variation in bracket width, ~7 us peak-to-peak, i.e. a few us.
// TSF read noise is therefore NOT the dominant timing term; it was assumed to be.
// Threshold just above the clean-device floor with enough attempts to retry when a
// device is not clean. Measured after loosening this too far: with a 60 us threshold and
// 3 attempts, b/c/d returned on the first read at 46-50 us but a read 83 us median with
// excursions to 122, and half of a's samples then failed the trust gate and stopped
// updating its offset filter entirely. Best-of-N had been hiding that -- at 8 attempts
// every device reported ~42 us. So keep enough retries for the best-of to matter on a
// noisy device, while a clean one still returns on its first read.
static constexpr int64_t SANDWICH_MAX_US = 50;
static constexpr int64_t SANDWICH_LOOSE_MAX_US = 400;
static constexpr int SANDWICH_ATTEMPTS = 5;
// The trust threshold is DERIVED per device, not chosen. What distinguishes a good read
// from a bad one is not an absolute width but whether the bracket is near the floor this
// hardware can achieve -- and that floor varies: four otherwise identical boards measured
// 45, 45, 79 and 81 us. Any fixed threshold is therefore wrong for some device, and
// wrong in the worst way: at 70 us, two of the four had EVERY sample rejected and their
// offset filter never updated, frozen at its first value while the true offset drifted
// away from it. A derived threshold cannot do that, because it is defined by samples
// that actually occurred.
//
// Floor = minimum bracket over a recent block of samples; trust anything within
// SANDWICH_TRUST_FACTOR of it. The block bound lets the floor rise if the device gets
// genuinely busier, rather than latching a lucky early read forever.
static constexpr int64_t SANDWICH_TRUST_FACTOR = 2;
static constexpr uint32_t SANDWICH_FLOOR_BLOCK = 256;
// Baseline spacing for this device's own TSF-vs-esp_timer rate measurement
static constexpr int64_t RATE_WINDOW_US = 4000000;
// Beacons arrive once a second; the render phase moves far slower than that.
// The group diagnostics are recomputed on every beacon we SEND, i.e. at BEACON_INTERVAL_US, so
// anything slower than that throws away data the device already has. Measured before this was
// lowered: 2 lines in 600 s, because the log also sat behind role conditions that no longer
// exist, so a 4 s throttle on top produced a series far too sparse to compare against a
// ~58 Hz wire measurement. At one line per second this is the cheapest resolution in the
// system -- and it is differenced between two devices to stand in for the analyser, which is
// exactly the comparison that needs the points.
static constexpr int64_t RENDER_LOG_INTERVAL_US = 1000000;

// Player-side TSF-vs-local rate, used to DE-TREND the offset filter (see
// shared_server_offset_us). Endpoints are trusted sandwiches, whose midpoint noise is a few us
// (the bracket WIDTH varies by ~7 us peak-to-peak; the bias itself is constant), so an 8 s
// baseline resolves the ratio to well under 1 ppm, and the EWMA below divides that again. The
// quantity is a crystal ratio -- it moves with temperature over minutes, not seconds -- so
// heavy smoothing costs nothing.
static constexpr int64_t OFFSET_RATE_WINDOW_US = 8000000;
static constexpr double OFFSET_RATE_ALPHA = 0.25;
// Crystals differ by well under +-100 ppm; anything larger is a TSF discontinuity, not a rate.
static constexpr double OFFSET_RATE_MAX_PPM = 100.0;
// Longest interval the feed-forward will extrapolate across in one step. The filter is called
// per chunk (~26 ms); a gap this long means the player was not running, and predicting across it
// is guessing. Bounds a single step to ~100 us at 50 ppm.
static constexpr int64_t OFFSET_FF_MAX_GAP_US = 2000000;

// The published mapping is slew-limited toward the live Kalman estimate: anchoring
// each beacon to the instantaneous estimate broadcast its sample-to-sample jitter
// (~±100-300 us) as 1 Hz deadline steps that every member's servo then chased
// (observed: trim swinging hundreds of ppm). The slewed line low-passes the jitter.
// Large estimate moves ramp at a faster (but still continuous) rate rather than
// snapping: a snap is a step every member chases clamp-limited and slightly
// time-offset from its peers (observed: ~6 ms snap -> ~2 ms differential for ~10 s),
// while a shared ramp keeps the pair identical throughout. Only implausibly large
// deltas (broken mapping / reconnect re-baseline) snap through.
// Expressed as RATES, scaled by the actual interval between broadcasts. They were
// per-beacon allowances, which silently coupled the tracking speed to
// BEACON_INTERVAL_US: raising the beacon rate to 10 Hz would have turned "50 us/s"
// into 500 us/s with nobody intending it. Using the measured interval also fixes a
// latent bug -- a delayed broadcast (service() rate limiting, task scheduling, a
// missed tick) previously still allowed only one beacon's worth of correction, so the
// published line fell further behind the longer the gap.
static constexpr int64_t TMS_SLEW_MAX_US_PER_S = 50;       // steady state
static constexpr int64_t TMS_SLEW_CATCHUP_US_PER_S = 300;  // once |delta| > 1 ms
static constexpr int64_t TMS_CATCHUP_THRESHOLD_US = 1000;
static constexpr int64_t TMS_SNAP_US = 20000;

// THE ADOPTED CONSENSUS IS SLEW-LIMITED TOO, and for a different reason than the published line
// above. That one low-passes our own estimate's jitter; this one bounds what MEMBERSHIP can do.
// A device joining or leaving moves the mean for everyone at once -- by (its disagreement)/N,
// which for a joiner still inside the plausibility bound can be milliseconds -- and a stepped
// timebase IS a hard resync, i.e. exactly the event this whole subsystem exists to avoid. So the
// group walks to the new mean instead of jumping to it, and because every member walks at the
// same rate the walk is common-mode: audible as nothing, and invisible on the wire between them.
//
// Same rates as the publish slew, deliberately. They are the same physical quantity moving at the
// same acceptable pace, and two different numbers here would only invite tuning one of them.
// The snap bound is where "membership moved the mean" stops being a credible explanation: past
// it, the timebase genuinely changed (an AP reboot, a re-anchor after a long outage) and smearing
// a real correction over minutes is worse than one step. A snap bumps timebase_epoch_ so the
// consumer's servo is told, which is the leaderless replacement for watching the role change.
//
// REVERTED, MEASURED: the adoption slew cost 2.7x on sd and has been removed. The reasoning below
// was sound for a per-device correction and wrong for a shared timebase -- see the long note at
// update_consensus_(). The publish slew above STAYS: it low-passes this device's own Kalman jitter
// before the number goes on the wire, which is a different job, and every device's contribution
// being individually smooth is what makes their mean smooth.
//
// What replaced it: adopt the mean exactly, and merely REPORT a move larger than this so the
// consumer's servo is told the timebase stepped. Sized at the old snap threshold, since that was
// the point past which a move stopped being explicable as membership or slew.
static constexpr int64_t MAP_STEP_REPORT_US = TMS_SNAP_US;

// OUTLIER ROBUSTNESS WITHOUT A MEDIAN. A median is robust and DISCONTINUOUS: over three values it
// is the middle one, so it steps whenever the ordering changes even though nothing moved.
// Measured 2026-08-28 on the render-phase group delta, which used one: it hopped +-96 us while
// the underlying data sat at +-12, and the correction driven by it was worse than no correction.
//
// So: one reweighting pass around the plain mean. Each value's weight falls off smoothly with its
// distance from that first mean, measured in units of the sample's own spread --
//
//     w_i = 1 / (1 + (d_i / (CONSENSUS_REWEIGHT_K * scale))^2),  scale = max(MAD, floor)
//
// -- which is continuous in every input, so no reordering can step the result, while a value far
// outside the pack still contributes almost nothing. With two devices the deviations are equal by
// construction, the weights are equal, and this is exactly their mean.
//
// The scale FLOOR is what stops a tight cluster from turning the reweighting into a median by
// another route: without it, three values spread over 3 us would give one of them a large d/scale
// and near-zero weight, which is discrimination against noise. Below the floor everything is
// noise and everything should count equally.
static constexpr double CONSENSUS_PHASE_SCALE_FLOOR_US = 20.0;

// Low-pass on the shared offset: 1/32 over per-chunk calls (~26 ms) is a ~0.8 s time constant,
// cutting uncorrelated TSF read noise by ~sqrt(32). See shared_server_offset_us().
// 1/64 over per-chunk calls (~26 ms) is a ~1.7 s time constant, cutting uncorrelated
// read noise by ~8x. Nearly free in lag terms: the mapping is drift-compensated, so the
// filtered quantity is near-constant rather than a moving signal.
// 1/64 -> 1/256. Per-chunk calls (~26 ms) make this a ~6.7 s time constant, cutting the
// uncorrelated TSF read noise by ~16x rather than ~8x.
//
// This is the one disturbance that does NOT cancel between devices. The shared mapping makes
// the offset ESTIMATE common-mode, but each device re-reads the TSF on every call and that read
// noise is its own -- so it lands differentially in the deadline, which is exactly the quantity
// stereo imaging cares about. Measured tonight against a logic analyser, with everything else
// tightened: common-mode disturbance MAD 122 us against a differential of MAD 12 us, and a wire
// floor of 31.5 us peak-to-peak. The differential is what is left to remove.
//
// The cost is tracking lag, and it is affordable BECAUSE it is common-mode. What the filter has
// to follow is the local-versus-server crystal difference, measured here at -48 to -52 ppm, so a
// 6.7 s constant lags by ~340 us. Both devices share the same server clock and lag identically,
// so that is 340 us of absolute latency and 0 us of inter-device skew. Room-to-room and
// lip-sync would notice; a stereo pair would not.
//
// Cheap in lag terms for a second reason: the published mapping carries drift_ppm and
// evaluate_mapping_ extrapolates with it, so the filtered quantity is already drift-compensated
// and near-constant. The 340 us above is the residual the compensation does not remove.
//
// If absolute latency ever matters more than imaging, this is the first constant to put back.
static constexpr double OFFSET_EWMA_ALPHA = 1.0 / 256.0;
// Above this, the raw sample is a real re-anchor (a consensus snap, a reconnect), not slew or
// noise: the published mapping moves at most TMS_SLEW_CATCHUP_US_PER_S, and read
// noise is bounded by the sandwich. Snap instead of smearing it in over ~0.8 s.
static constexpr double OFFSET_SNAP_US = 2000.0;

// All ESP32 variants are little-endian; fields are sent raw (no htonl)
struct __attribute__((packed)) TsfPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t bssid[6];
  uint8_t sender_mac[6];
  // 0 = this packet carries a raw server<->TSF estimate; 1 = PHASE REPORT ONLY, the mapping
  // fields are zero and must not be read.
  //
  // There is no leader, so this is no longer a role: every device publishes its own raw estimate
  // and every device averages all of them. What this flag distinguishes is whether the sender
  // HAS an estimate worth pooling -- a device whose Kalman has not settled publishes phase only,
  // because an unsettled estimate converges in 100+ ms steps and pooling those drags the mean
  // (the same objection that used to gate leadership, divided by N).
  //
  // ENCODING IS UNCHANGED FROM THE LEADER ERA on purpose, so a rolling reflash degrades rather
  // than misreads: an old leader's 0 still means "mapping present" and an old follower's 1 still
  // means "phase only", both of which are true in the new reading.
  uint8_t no_mapping;
  uint32_t server_id_hash;
  int64_t tsf_base_us;
  int64_t tsf_minus_server_us;
  float drift_ppm;  // d(tsf − server)/dt, in TSF units
  // Sender's playout pipeline depth (pushed-but-unplayed), us; PIPELINE_UNKNOWN
  // before it has played anything. Diagnostics only -- never feeds the timebase.
  int32_t pipeline_us;
  // RENDER PHASE: the TSF instant at which this sender renders server audio time zero.
  //
  //   render_phase = (played_ts + (tsf - tsf_local)) - (s_ts - (pushed - played) * 1e6 / rate)
  //                   \________ when the last frame rendered, in TSF ______/   \__ its server time __/
  //
  // Two devices playing the same stream MUST map a given server frame to the same TSF
  // instant, so differencing this between them is the true relative playout offset -- with
  // the servo, the prediction model and the pipeline depth all outside the measurement.
  // That is the distinction from pipeline_us above, which compares buffer OCCUPANCY: depths
  // legitimately differ between devices that are rendering in perfect sync, which is why it
  // reads -13.4 ms for an offset a logic analyser measures at 3.7 ms.
  //
  // Absolute value is meaningless (TSF and server epochs are unrelated and it is a large
  // number); only differences between devices mean anything. RENDER_PHASE_UNKNOWN before
  // anything has rendered. Diagnostics only -- never feeds the timebase.
  int64_t render_phase_us;
  // CRYSTAL RATE: this sender's d(tsf − local)/dt in ppm, i.e. its own clock measured against
  // the RADIO timebase. NaN before it has been measured.
  //
  // Here because its DIFFERENCE between two devices is their crystal difference, and that is
  // the one term standing between the trim and a usable rate reference. Measured with a logic
  // analyser: the differential trim sits -5.25..-5.40 ppm from the true differential achieved
  // rate, stable across runs, and subtracting this difference removes essentially all of it --
  // integrated error from 505 us per 100 s down to 17. Publishing it lets each device compute
  // that correction for itself, which is the whole point: an analyser on the bench cannot ship
  // inside a speaker.
  //
  // Sourced from offset_rate_ppm_, NOT tsf_rate_ppm_. They are the same physical quantity but
  // the latter is measured on the network task from the beacon's own TSF reads, which stop the
  // moment a device has nothing to publish. offset_rate_ppm_ comes from the samples the offset
  // filter already takes on the player task, which run whenever audio does.
  //
  // Diagnostics only -- never feeds the timebase. Appended at the END and the version is NOT
  // bumped, per the note at TSF_VERSION: a bump makes a half-flashed fleet lose the shared
  // timebase in BOTH directions, which is expensive, and an appended field cannot be misread by
  // an older receiver -- it simply is not there. The receiver accepts both lengths.
  float crystal_ppm;
  // THE SENDER'S ADOPTED MAPPING, evaluated at tsf_base_us above so it is comparable without
  // agreeing on any other instant. TMS_ADOPT_UNKNOWN when it holds none.
  //
  // This design's correctness condition is stated at "ADOPT THE MEAN EXACTLY": every device
  // holding the same estimate set steps to the same value at the same time, so the mapping's own
  // error is common-mode and cancels between devices -- which is how inputs wandering
  // +-100-300 us held 3.6 us on the wire. Nothing verified it. Devices hold different sets for up
  // to PEER_MAP_STALE_US whenever a beacon is lost or membership changes, and their mappings then
  // differ by a wandering fraction of the group spread (measured live at 40-934 us between two
  // devices) which lands straight on the wire, invisibly.
  //
  // One field makes it checkable: a receiver evaluates its OWN adopted mapping at this tsf_base
  // and subtracts. Diagnostics only -- it never feeds the timebase, because a device that
  // steered by its peers' adopted values would be slewing, and slewing was measured WORSE for
  // exactly the reason above (sd 3.6 -> 9.7 us).
  //
  // Appended at the END with no version bump, per the rule the fields above follow.
  int64_t adopted_tms_at_base_us;
  // STREAM IDENTITY: FNV-1a of the snapcast stream this sender is playing, 0 when unknown.
  //
  // render_phase_us above is only comparable between devices playing the SAME stream -- its own
  // contract says so -- because it is expressed against that stream's server audio time. Two
  // devices on different streams of the same server produce a difference that means nothing,
  // and acting on it would drive them apart. Measured 2026-08-27: with the probed pair on one
  // stream and the leader on another, the render delta tracked the true displacement at
  // 94/95/94/119/-42/81/131/119 percent -- i.e. sometimes inverted.
  //
  // So leadership is scoped to the stream as well as the BSS. Appended at the END with NO
  // version bump, per the note at TSF_VERSION and the crystal_ppm precedent: an older sender
  // simply does not carry it, and 0 means "unknown" and is accepted by anyone, so a
  // half-flashed fleet degrades to the previous behaviour instead of splitting in two.
  uint32_t stream_id_hash;
  // Age of render_phase_us at send time, ms (0xFFFF = unknown / older sender). The receiver pairs
  // phases by SAMPLE instant, not by receipt: with the phase published once per block (~0.65 s) and
  // beaconed once a second, the sample can be up to ~1.6 s older than its receipt, and the shared
  // mapping ramps at 2-3 ppm most of the time, so an older phase reads later by ramp x age. Measured
  // 2026-08-30 07:42-08:36: BOTH boards' group deltas read negative (A -6..-18, B ~0; the pair must
  // sum to zero), and render_align marched both deadlines later together at 15 us/min. Appended
  // last so an older sender's shorter packet still parses (see the short-packet defaults on receive).
  uint16_t render_phase_age_ms;
  // This device's render_align bias (us), INT32_MIN = unknown / older sender. A bias common to every
  // device shifts every deadline equally and carries no alignment information; each device subtracts
  // the group mean so only the differential survives (2026-08-30: a residual ~5 us asymmetry in the
  // pairing marched both biases +3 us/min toward the +-500 cap).
  int32_t render_bias_us;
};

// Fields from render_phase_us onward are the newest additions, so an older sender's packet is
// SHORTER. Accept it and default what is missing rather than dropping it on the length check:
// dropping would make a mixed fleet lose the timebase in the direction the version note was
// careful to keep working.
static constexpr size_t TSF_PACKET_MIN_BYTES = offsetof(TsfPacket, crystal_ppm);

/// "I hold no adopted mapping." A sentinel, never printed as a number and never differenced --
/// subtracting it would give 2^63 of overflow that looks like a divergence measurement.
static constexpr int64_t TMS_ADOPT_UNKNOWN = INT64_MIN;

/// Report an adopted-mapping divergence beyond this. Two devices holding the same set agree
/// EXACTLY (the mean is deterministic), so anything above the arithmetic's own noise means the
/// sets differ. Set at one frame at 44.1 kHz: below that the divergence is inaudible even if real,
/// above it the wire carries it.
static constexpr int64_t MAP_DIVERGENCE_REPORT_US = 23;

TsfSync::~TsfSync() {
  if (this->sock_ >= 0) {
    ::close(this->sock_);
  }
}

bool TsfSync::sample_tsf_(int64_t &tsf_us, int64_t &local_us, int64_t *width_out) {
  // Sandwich the TSF read between esp_timer reads; a wide sandwich means an
  // interrupt (or a slow driver path) landed mid-read and the pairing is noisy.
  // Prefer a tight sandwich, but accept the best attempt up to a looser bound --
  // the servo's median absorbs occasional noisy samples, while a hard failure
  // drops the whole mapping to the Kalman fallback.
  int64_t best_width = INT64_MAX;
  for (int attempt = 0; attempt < SANDWICH_ATTEMPTS; attempt++) {
    const int64_t l1 = esp_timer_get_time();
    const int64_t tsf = esp_wifi_get_tsf_time(WIFI_IF_STA);
    const int64_t l2 = esp_timer_get_time();
    if (tsf == 0) {
      return false;  // radio asleep / not associated; retrying won't help now
    }
    const int64_t width = l2 - l1;
    if (width < best_width) {
      best_width = width;
      tsf_us = tsf;
      local_us = l1 + width / 2;
    }
    if (width <= SANDWICH_MAX_US) {
      if (width_out != nullptr) {
        *width_out = width;
      }
      return true;
    }
  }
  if (width_out != nullptr) {
    *width_out = best_width;
  }
  return best_width <= SANDWICH_LOOSE_MAX_US;
}

TsfSync::EvalResult TsfSync::evaluate_mapping_(int64_t tsf_base_us, int64_t tsf_minus_server_us, float drift_ppm,
                                               int64_t &offset_us, int64_t &extrapolation_us, int64_t *width_out,
                                               int64_t *tsf_out, int64_t *local_out) {
  int64_t tsf_now, local_now;
  extrapolation_us = 0;
  if (!sample_tsf_(tsf_now, local_now, width_out)) {
    return EvalResult::NO_TSF;
  }
  if (tsf_out != nullptr) {
    *tsf_out = tsf_now;
  }
  if (local_out != nullptr) {
    *local_out = local_now;
  }
  extrapolation_us = tsf_now - tsf_base_us;
  // Small negative skew is legitimate: two stations' TSF free-run on their own
  // crystals between beacons (~us apart), and the sender samples before we do
  if (extrapolation_us < -1000 || extrapolation_us > MAX_EXTRAPOLATION_US) {
    return EvalResult::AGE_CLAMP;  // TSF reset (AP reboot) or mapping far past expiry
  }
  const double tms_now = static_cast<double>(tsf_minus_server_us) +
                         static_cast<double>(drift_ppm) * 1e-6 * static_cast<double>(extrapolation_us);
  const int64_t server_now_us = tsf_now - static_cast<int64_t>(tms_now);
  offset_us = server_now_us - local_now;
  return EvalResult::OK;
}

bool TsfSync::ensure_socket_() {
  if (this->sock_ >= 0 && this->joined_) {
    return true;
  }
  if (this->sock_ < 0) {
    this->sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (this->sock_ < 0) {
      return false;
    }
    int reuse = 1;
    setsockopt(this->sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int flags = fcntl(this->sock_, F_GETFL, 0);
    fcntl(this->sock_, F_SETFL, flags | O_NONBLOCK);
    uint8_t ttl = 1;
    setsockopt(this->sock_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    uint8_t loop = 0;  // never hear our own packets back
    setsockopt(this->sock_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(TSF_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(this->sock_, reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0) {
      ::close(this->sock_);
      this->sock_ = -1;
      return false;
    }
  }
  // Group membership needs a live interface; retried each service tick until it takes
  struct ip_mreq mreq = {};
  mreq.imr_multiaddr.s_addr = inet_addr(TSF_GROUP);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  this->joined_ = setsockopt(this->sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
  return this->joined_;
}

// RESET IS FOR THE NETWORK GENUINELY CHANGING UNDERNEATH US -- a roam, a disassociation, a new
// BSSID -- and nothing else. Everything keyed to the AP's TSF counter becomes meaningless at
// that moment: our published line, every peer's, and the consensus of them.
//
// It is deliberately NOT reachable from anything about this device's own audio. Under leader
// election it was, and that cost a measured ~6 s every time a leader stepped down: the ex-leader
// dropped to the raw Kalman fallback with medians at +-1 ms and trim swinging +543/-486 ppm,
// taking 16.0 s to re-lock where a device that had not led took 9.6 s. There is no step-down any
// more, but the rule that produced that bug is worth keeping explicit: a device's playout being
// unhealthy says nothing whatsoever about the validity of a server<->TSF mapping.
void TsfSync::reset_(const char *reason) {
  if (this->mapping_valid_ || this->pub_valid_) {
    ESP_LOGD(TAG, "Reset (%s)", reason);
  }
  this->tsf_rate_valid_ = false;
  this->rate_ref_local_us_ = 0;
  this->pub_valid_ = false;  // TSF timebase changed; the published line with it
  for (Peer &p : this->peer_) {
    p.map_valid = false;  // every peer's line is expressed in the OLD TSF epoch
  }
  this->learned_peers_.clear();  // addresses may change with the network
  this->update_peer_count_();
  this->consensus_n_.store(0, std::memory_order_relaxed);
  this->mapping_mutex_.lock();
  this->mapping_valid_ = false;
  this->mapping_mutex_.unlock();
}

void TsfSync::adopt_(int64_t tsf_base_us, int64_t tsf_minus_server_us, float drift_ppm, int64_t local_now_us) {
  this->mapping_mutex_.lock();
  this->mapping_valid_ = true;
  this->map_tsf_base_us_ = tsf_base_us;
  this->map_tsf_minus_server_us_ = tsf_minus_server_us;
  this->map_drift_ppm_ = drift_ppm;
  this->map_updated_local_us_ = local_now_us;
  this->mapping_mutex_.unlock();
}

void TsfSync::receive_(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash,
                       uint32_t stream_id_hash) {
  TsfPacket pkt;
  struct sockaddr_in from = {};
  while (true) {
    socklen_t from_len = sizeof(from);
    const ssize_t n =
        recvfrom(this->sock_, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&from), &from_len);
    if (n < static_cast<ssize_t>(TSF_PACKET_MIN_BYTES)) {
      if (n < 0) {
        break;  // drained (EWOULDBLOCK) or error; retried next tick either way
      }
      continue;  // runt packet: too short even for the timebase core
    }
    if (n < static_cast<ssize_t>(sizeof(pkt))) {
      // Older sender, before crystal_ppm and stream_id_hash were appended. Default them rather
      // than reject the packet: its timebase fields are all at their original offsets and
      // perfectly usable. BOTH must be reset every iteration, since pkt is reused and would
      // otherwise carry the PREVIOUS sender's values -- for stream_id_hash that means silently
      // accepting a short packet as belonging to whatever stream the last long packet named.
      if (n < static_cast<ssize_t>(offsetof(TsfPacket, render_phase_age_ms))) {
        pkt.crystal_ppm = NAN;
        pkt.stream_id_hash = 0;
      }
      pkt.render_phase_age_ms = 0xFFFF;  // older sender: pair by receipt, as before
      pkt.render_bias_us = INT32_MIN;
      if (n < static_cast<ssize_t>(offsetof(TsfPacket, adopted_tms_at_base_us) +
                                   sizeof(pkt.adopted_tms_at_base_us))) {
        pkt.adopted_tms_at_base_us = TMS_ADOPT_UNKNOWN;  // sender predates the field
      }
    }
    if (pkt.magic != TSF_MAGIC || pkt.version != TSF_VERSION) {
      continue;
    }
    if (memcmp(pkt.sender_mac, this->my_mac_, 6) == 0) {
      continue;  // our own packet (multicast loop disabled, but the AP reflects)
    }
    // Only mappings for our AP's TSF timer apply: a peer on another BSS is a
    // separate TSF island by design (both lead) -- surface it, it usually means
    // the pair should be pinned to one AP with the BSSID select
    if (memcmp(pkt.bssid, this->bssid_, 6) != 0) {
      if (!this->warned_foreign_bss_) {
        this->warned_foreign_bss_ = true;
        ESP_LOGW(TAG,
                 "Peer %02X:%02X:%02X:%02X:%02X:%02X is on another AP (BSSID %02X:%02X:%02X:%02X:%02X:%02X vs our "
                 "%02X:%02X:%02X:%02X:%02X:%02X) - no shared TSF timebase; pin both to one AP to group them",
                 pkt.sender_mac[0], pkt.sender_mac[1], pkt.sender_mac[2], pkt.sender_mac[3], pkt.sender_mac[4],
                 pkt.sender_mac[5], pkt.bssid[0], pkt.bssid[1], pkt.bssid[2], pkt.bssid[3], pkt.bssid[4], pkt.bssid[5],
                 this->bssid_[0], this->bssid_[1], this->bssid_[2], this->bssid_[3], this->bssid_[4], this->bssid_[5]);
      }
      continue;
    }
    if (pkt.server_id_hash != server_id_hash) {
      if (!this->warned_foreign_server_) {
        this->warned_foreign_server_ = true;
        ESP_LOGD(TAG, "Ignoring TSF packets for a different snapserver");
      }
      continue;
    }
    // SAME STREAM ONLY, for the MAPPING. A zero hash means the sender predates this field (or has
    // no stream yet) and is accepted, so a mixed fleet keeps the shared timebase rather than
    // splitting -- which is right for the mapping, because the server<->TSF line is a property of
    // the CLOCK and is common-mode across whatever anyone is playing.
    if (pkt.stream_id_hash != 0 && stream_id_hash != 0 && pkt.stream_id_hash != stream_id_hash) {
      if (!this->warned_foreign_stream_) {
        this->warned_foreign_stream_ = true;
        ESP_LOGD(TAG, "Ignoring TSF packets for a different stream (%08" PRIx32 " != %08" PRIx32 ")",
                 pkt.stream_id_hash, stream_id_hash);
      }
      continue;
    }
    // RENDER PHASE IS STRICTER, and the difference is not a nicety. A phase is expressed against
    // the stream's server audio time, so a delta taken across two streams is not a playout offset
    // at all -- it is the distance between two unrelated timelines. The permissive zero above is
    // exactly wrong here: "I do not know what I am playing" is not evidence of playing the same
    // thing, and one such peer is enough to poison the group statistic, which the 300 ms pairing
    // window leaves with 0-2 contributors 94% of the time.
    //
    // Measured 2026-08-28: two boards on Spotify contributed phases to a group playing MLS44,
    // medians 33 ms and 15 ms out, and were the worst contributor in 705 of 2164 observer reports.
    // Nothing was misbehaving -- those were correct phases for a different stream.
    const bool phase_comparable = pkt.stream_id_hash != 0 && stream_id_hash != 0 &&
                                  pkt.stream_id_hash == stream_id_hash;
    this->rx_peer_count_++;
    // Learn the sender's address for unicast beacons: the server roster can predate
    // a peer's connection (boot race) and multicast may be blocked entirely
    this->learn_peer_(from.sin_addr.s_addr);

    Peer *peer = this->find_peer_(pkt.sender_mac, local_now_us);
    peer->seen_us = local_now_us;
    peer->pipeline_us = pkt.pipeline_us;
    peer->crystal_ppm = pkt.crystal_ppm;
    if (pkt.render_bias_us != INT32_MIN) {
      peer->bias_us = pkt.render_bias_us;
      peer->bias_seen_us = local_now_us;
    }
    // An incomparable phase is dropped, not recorded as UNKNOWN: record_peer_phase_ deliberately
    // keeps a peer's last phase when it reports UNKNOWN, so overwriting would be a no-op anyway,
    // and what we want is for this peer never to enter the statistic at all.
    if (phase_comparable) {
      // Pair by the peer's SAMPLE instant (receipt minus the age it reported), see TsfPacket.
      const int64_t phase_sampled_us =
          pkt.render_phase_age_ms == 0xFFFF ? local_now_us : local_now_us - static_cast<int64_t>(pkt.render_phase_age_ms) * 1000;
      this->record_peer_phase_(*peer, pkt.render_phase_us, phase_sampled_us);
      // AVERAGED DELTA PAIRING (build 84, shadow). Pair this peer sample against the nearest own
      // sample within 60 ms -- both sides now sample at chunk cadence, so nearly every 30 Hz
      // arrival pairs -- and accumulate ((peer - mine) - drift*gap) per peer for the 1 s roll
      // below. Window and sign conventions match recompute_group_delta_.
      if (pkt.render_phase_us != RENDER_PHASE_UNKNOWN) {
        const size_t pi = static_cast<size_t>(peer - this->peer_);
        this->mapping_mutex_.lock();
        const float drift = this->map_drift_ppm_;
        int64_t best_gap = INT64_MAX;
        int64_t best_phase = 0, best_at = 0;
        for (size_t k = 0; k < OWN_PHASE_RING; k++) {
          if (this->own_ph_[k].at_us == 0) continue;
          const int64_t gap = phase_sampled_us - this->own_ph_[k].at_us;
          if (std::abs(gap) < std::abs(best_gap)) {
            best_gap = gap;
            best_phase = this->own_ph_[k].phase_us;
            best_at = this->own_ph_[k].at_us;
          }
        }
        this->mapping_mutex_.unlock();
        if (best_at != 0 && std::abs(best_gap) <= 60000 && pi < MAX_PEERS &&
            this->gdavg_n_[pi] < 0xFFFF) {
          this->gdavg_sum_[pi] += static_cast<double>(pkt.render_phase_us - best_phase) -
                                  static_cast<double>(drift) * 1e-6 * static_cast<double>(best_gap);
          this->gdavg_n_[pi]++;
        }
      }
      // Roll the window once per second: gd_avg = -mean(0, per-peer means), the live delta's
      // convention (mine is in the group; > 0 = I render LATE). Published even when empty
      // (INT32_MIN) so a consumer can tell "no pairs" from "zero".
      if (this->gdavg_roll_us_ == 0) this->gdavg_roll_us_ = local_now_us;
      if (local_now_us - this->gdavg_roll_us_ >= 1000000) {
        this->gdavg_roll_us_ = local_now_us;
        double sum = 0.0;
        uint32_t peers_n = 0, pairs = 0;
        for (size_t i = 0; i < MAX_PEERS; i++) {
          if (this->gdavg_n_[i] > 0) {
            sum += this->gdavg_sum_[i] / this->gdavg_n_[i];
            pairs += this->gdavg_n_[i];
            peers_n++;
          }
          this->gdavg_sum_[i] = 0.0;
          this->gdavg_n_[i] = 0;
        }
        if (peers_n > 0) {
          const double d = -sum / static_cast<double>(peers_n + 1);
          this->render_group_delta_avg_us_.store(
              static_cast<int32_t>(std::clamp(d, -2.0e9, 2.0e9)), std::memory_order_relaxed);
          this->gdavg_pairs_pub_.store(static_cast<uint16_t>(std::min<uint32_t>(pairs, 0xFFFF)),
                                       std::memory_order_relaxed);
          if (++this->gdavg_log_ctr_ >= 3) {
            this->gdavg_log_ctr_ = 0;
            ESP_LOGD(TAG, "GDAVG avg=%+ld n=%u live=%ld",
                     static_cast<long>(this->render_group_delta_avg_us_.load(std::memory_order_relaxed)),
                     static_cast<unsigned>(pairs),
                     static_cast<long>(this->render_group_delta_us_.load(std::memory_order_relaxed)));
          }
        } else {
          this->render_group_delta_avg_us_.store(INT32_MIN, std::memory_order_relaxed);
          this->gdavg_pairs_pub_.store(0, std::memory_order_relaxed);
        }
      }
    }

    // PHASE-ONLY BEACON: the sender has no settled estimate to pool. Its phase and diagnostics
    // are recorded above; the mapping fields are zero and must not be read.
    if (pkt.no_mapping != 0) {
      continue;
    }

    // Estimate gates. A rejected estimate simply does not join the consensus; it never
    // invalidates the mapping we hold, which continues to expire on its own timer.
    int64_t implied_offset_us, extrapolation_us;
    const EvalResult ev =
        evaluate_mapping_(pkt.tsf_base_us, pkt.tsf_minus_server_us, pkt.drift_ppm, implied_offset_us,
                          extrapolation_us);
    if (ev != EvalResult::OK) {
      if (!this->warned_rejected_) {
        this->warned_rejected_ = true;
        if (ev == EvalResult::NO_TSF) {
          ESP_LOGD(TAG, "Rejected peer estimate (TSF unreadable)");
        } else {
          ESP_LOGD(TAG, "Rejected peer estimate (age clamp: extrapolation %" PRId64 " us)", extrapolation_us);
        }
      }
      continue;
    }
    // Plausibility only means something when our own estimate deserves trust: a freshly-booted
    // device's raw Kalman swings +-100 ms under the post-reboot congestion, and vetoing every
    // (maturity-gated, trustworthy) peer on the strength of it leaves us churning on our own bad
    // clock instead -- observed as a whole fleet rejecting sign-flipping "implausible" deltas for
    // minutes after a simultaneous OTA. An immature device pools its peers unconditionally, which
    // is also how it gets a timebase at all: it has none of its own to contribute.
    //
    // The gate is per-peer now rather than per-leader, and that is a real strengthening: one
    // device with a broken clock is excluded by everybody instead of being either the sole
    // authority or nothing.
    if (est.valid && est.mature) {
      const int64_t own_offset_us = static_cast<int64_t>(est.offset_ms * 1000.0);
      if (std::abs(implied_offset_us - own_offset_us) > this->plausibility_us_) {
        if (!this->warned_rejected_) {
          this->warned_rejected_ = true;
          ESP_LOGD(TAG, "Rejected peer estimate (implausible: %+" PRId64 " us vs own estimate)",
                   implied_offset_us - own_offset_us);
        }
        continue;
      }
    }
    this->warned_rejected_ = false;
    // SET DIVERGENCE. The design is correct only while every device holds the same estimate set:
    // the mean is deterministic, so identical sets give identical mappings and the mapping's own
    // error is common-mode and cancels. Sets differ for up to PEER_MAP_STALE_US after a lost
    // beacon or a membership change, and the mappings then differ by a wandering fraction of the
    // group spread -- 40-934 us measured between two devices -- which lands directly on the wire
    // with nothing reporting it. This is that check, and it is the design's own correctness
    // condition rather than a new rule.
    if (pkt.adopted_tms_at_base_us != TMS_ADOPT_UNKNOWN) {
      this->mapping_mutex_.lock();
      const bool have_map = this->mapping_valid_;
      const int64_t mb = this->map_tsf_minus_server_us_;
      const int64_t tb = this->map_tsf_base_us_;
      const float dr = this->map_drift_ppm_;
      this->mapping_mutex_.unlock();
      if (have_map) {
        // Ours at THEIR tsf_base, so no third instant has to be agreed on.
        const int64_t mine_at_theirs =
            mb + static_cast<int64_t>(static_cast<double>(dr) * 1e-6 *
                                      static_cast<double>(pkt.tsf_base_us - tb));
        const int64_t div = pkt.adopted_tms_at_base_us - mine_at_theirs;
        if (std::abs(div) > MAP_DIVERGENCE_REPORT_US &&
            local_now_us - this->last_mapdiv_log_us_ >= 5000000) {
          this->last_mapdiv_log_us_ = local_now_us;
          ESP_LOGW(TAG,
                   "MAPDIV %02X%02X adopted %+" PRId64 " us from ours (theirs %" PRId64
                   ", ours %" PRId64 " at their base) -- the sets differ, so the mapping error is "
                   "NOT common-mode and this lands on the wire",
                   pkt.sender_mac[4], pkt.sender_mac[5], div, pkt.adopted_tms_at_base_us,
                   mine_at_theirs);
        }
      }
    }
    peer->map_valid = true;
    peer->map_seen_us = local_now_us;
    peer->tsf_base_us = pkt.tsf_base_us;
    peer->tms_base_us = pkt.tsf_minus_server_us;
    peer->drift_ppm = pkt.drift_ppm;
  }
}

// Slot for a MAC, allocating or evicting the stalest entry. Never fails: a group larger than
// MAX_PEERS thrashes the last slot rather than allocating on the network task, and a thrashing
// slot still contributes a real estimate to the mean.
TsfSync::Peer *TsfSync::find_peer_(const uint8_t mac[6], int64_t local_now_us) {
  Peer *oldest = &this->peer_[0];
  for (Peer &p : this->peer_) {
    if (p.used && memcmp(p.mac, mac, 6) == 0) {
      return &p;
    }
    if (!p.used) {
      p = Peer{};
      memcpy(p.mac, mac, 6);
      p.used = true;
      p.bias_us = INT32_MIN;  // unknown until this peer reports one
      p.bias_seen_us = 0;
      p.phase_us = RENDER_PHASE_UNKNOWN;
      p.pipeline_us = PIPELINE_UNKNOWN;
      p.crystal_ppm = NAN;
      p.seen_us = local_now_us;
      return &p;
    }
    if (p.seen_us < oldest->seen_us) {
      oldest = &p;
    }
  }
  *oldest = Peer{};
  memcpy(oldest->mac, mac, 6);
  oldest->used = true;
  oldest->phase_us = RENDER_PHASE_UNKNOWN;
  oldest->pipeline_us = PIPELINE_UNKNOWN;
  oldest->crystal_ppm = NAN;
  oldest->seen_us = local_now_us;
  return oldest;
}

// ONE REWEIGHTING PASS AROUND THE MEAN -- the continuous stand-in for a median. See
// CONSENSUS_REWEIGHT_K for why a median is not used and what the scale floor is protecting.
// Values are passed pre-differenced against a reference so they stay small and exact in double.

// THE CORE OF THE LEADERLESS DESIGN. Average every live raw estimate -- ours and every peer's --
// and slew the adopted mapping toward the result.
//
// Estimates are LINES, not points: {tsf_base, tsf-server at base, drift}. That is what makes
// averaging them well-defined without any of the sampling-instant machinery the render phase
// needs -- each line is evaluated at one common TSF instant before they are combined, so peers
// that beaconed at different times still contribute comparable numbers. The reference instant is
// our own published base when we have one (no extrapolation on our own term) and otherwise the
// freshest peer's.
//
// WHAT IS AVERAGED IS EVERY DEVICE'S RAW LINE, INCLUDING OURS. Our contribution is pub_*, the
// line we put on the wire, which is derived from our own Kalman and nothing else. It is NOT
// map_*, the consensus we adopted. Feeding the consensus back would be positive feedback with
// gain 1: the group could then walk together indefinitely while every device agreed with every
// other, and no on-device measurement could see it. This is the one failure mode of the design
// and this is the line of code that prevents it.
void TsfSync::update_consensus_(int64_t local_now_us) {
  // Reference instant: THE FRESHEST BASE IN THE SET, ours included. A FUNCTION OF THE SET, NOT OF
  // THE DEVICE.
  //
  // This used to prefer our own base ("no extrapolation on our own term") and fall back to the
  // freshest peer's. That made ref_tsf device-dependent, and the design's correctness argument
  // does not survive it: identical sets must give identical mappings, so that the mapping's own
  // error is common-mode and cancels on the wire. It is common-mode only if every device computes
  // the SAME number.
  //
  // A plain mean would not have cared -- ref factors out of it as mean(drift)*ref, shifting every
  // device's answer identically. robust_mean is not linear: its weights are 1/(1+d^2) on each
  // line's deviation from the mean, and when the lines' DRIFTS differ their spread grows with the
  // reference instant. So different references give different weights, hence different means.
  // The comment elsewhere that "the estimator is gauge-invariant, so arrival order cannot matter"
  // is true of the mean and false of the reweighted mean.
  //
  // Measured in tests/timebase group 9, three boards with the crystals actually on this bench
  // (46.3 / 42.2 / 37.9 ppm) and beacons 500 ms apart: 43.4 us of spread between the mappings the
  // three devices adopt, from the reference alone, with identical sets. Anchoring on the freshest
  // base in the set takes it to EXACTLY zero -- peers' tsf_base is a transmitted field, so `max`
  // over the same set is the same number on every device.
  //
  // The cost is real and small: our own line is now extrapolated too, by at most one beacon
  // interval, using the drift we just measured. Under a microsecond, against 43 us of disagreement
  // that lands directly on the wire. Extrapolation stays forward for every line (dt >= 0), which
  // is the well-defined direction.
  //
  // This does NOT address sets that genuinely differ -- a beacon lost by one device and not
  // another, for up to PEER_MAP_STALE_US. That is bounded, documented above, and softened by the
  // adopted-consensus slew limiter. This removes only the device-dependent term.
  int64_t ref_tsf = 0;
  bool have_ref = false;
  if (this->pub_valid_) {
    ref_tsf = this->pub_tsf_base_;
    have_ref = true;
  }
  for (const Peer &p : this->peer_) {
    if (!p.used || !p.map_valid || local_now_us - p.map_seen_us > PEER_MAP_STALE_US) {
      continue;
    }
    if (std::fabs(p.drift_ppm) > MAX_PLAUSIBLE_DRIFT_PPM) {
      continue;  // not a contributor (see the intake gate below), so not an anchor either
    }
    if (!have_ref || p.tsf_base_us > ref_tsf) {
      ref_tsf = p.tsf_base_us;
      have_ref = true;
    }
  }
  if (!have_ref) {
    // Nothing to consense over: no estimate of our own and nothing audible. The held mapping
    // expires on its own timer into the Kalman fallback; report zero contributors so the
    // diagnostics say "inactive" rather than claiming a consensus that stopped being computed.
    this->consensus_n_.store(0, std::memory_order_relaxed);
    return;
  }

  // Evaluate every line at ref_tsf. Values are differenced against the first so the doubles
  // carry microseconds of spread rather than the raw tsf-minus-server magnitude.
  double dv[MAX_PEERS + 1];
  double dr[MAX_PEERS + 1];
  size_t n = 0;
  int64_t base_tms = 0;
  // WHO CONTRIBUTED WHAT. dv[] and dr[] alone cannot answer that, and without it a timebase step
  // can only be reasoned about from its magnitude -- which I did, and got wrong twice: first
  // blaming a zero-valued mapping (no such value is reachable: BSSID and server_id are checked on
  // receive, a BSSID change resets, broadcast_ is gated on est.valid && est.mature, and
  // no_mapping continues before map_valid is set), then blaming the anchor (the estimator is
  // gauge-invariant, so arrival order cannot matter). Record the inputs so the next step names
  // its cause instead of inviting another theory.
  struct Contrib {
    bool self;
    uint8_t mac[6];
    int64_t tsf_base;
    int64_t tms_base;
    float drift_ppm;
    int64_t tms_at_ref;
  };
  Contrib contrib[MAX_PEERS + 1] = {};
  const auto add = [&](bool self, const uint8_t *mac, int64_t tsf_base, int64_t tms_base,
                       float drift_ppm) {
    const int64_t dt = ref_tsf - tsf_base;
    const int64_t tms = tms_base + static_cast<int64_t>(static_cast<double>(drift_ppm) * 1e-6 *
                                                        static_cast<double>(dt));
    if (n == 0) {
      base_tms = tms;
    }
    contrib[n].self = self;
    if (mac != nullptr) {
      memcpy(contrib[n].mac, mac, 6);
    }
    contrib[n].tsf_base = tsf_base;
    contrib[n].tms_base = tms_base;
    contrib[n].drift_ppm = drift_ppm;
    contrib[n].tms_at_ref = tms;
    dv[n] = static_cast<double>(tms - base_tms);
    dr[n] = static_cast<double>(drift_ppm);
    n++;
  };
  if (this->pub_valid_) {
    add(true, this->my_mac_, this->pub_tsf_base_, this->pub_tms_base_, this->pub_drift_ppm_);
  }
  for (const Peer &p : this->peer_) {
    if (n >= MAX_PEERS + 1 || !p.used || !p.map_valid || local_now_us - p.map_seen_us > PEER_MAP_STALE_US) {
      continue;
    }
    // NOT VOTABLE. robust_mean reweights around a mean, which cannot survive an outlier this far
    // out with the 2-4 contributors a small group actually has: a single +61641 ppm line carried
    // the mean regardless of how the others were weighted. A rejected peer is correctly ABSENT --
    // it does not contribute a zero, which would be its own wrong answer -- and the peer is still
    // free to rejoin the moment it publishes something physical again.
    if (std::fabs(p.drift_ppm) > MAX_PLAUSIBLE_DRIFT_PPM) {
      if (!this->warned_peer_drift_) {
        this->warned_peer_drift_ = true;
        ESP_LOGW(TAG, "DRIFTREJ peer %02X%02X %+.0f ppm implausible, excluded", p.mac[4], p.mac[5], p.drift_ppm);
      }
      continue;
    }
    add(false, p.mac, p.tsf_base_us, p.tms_base_us, p.drift_ppm);
  }
  if (n == 0) {
    return;
  }
  this->consensus_n_.store(static_cast<uint8_t>(n), std::memory_order_relaxed);

  const int64_t tms_target = base_tms + static_cast<int64_t>(llround(robust_mean(dv, n, CONSENSUS_SCALE_FLOOR_US)));
  // Drift is a rate, so its spread is ppm rather than us; the same floor would swamp it. The
  // reweighting is there for a broken peer, and a broken RATE shows up as tens of ppm.
  const float drift_c = static_cast<float>(robust_mean(dr, n, 1.0));

  // ADOPT THE MEAN EXACTLY. NO SLEW. This inverts step 4 of PLAN-leaderless.md ("Slew, do not
  // step") on the strength of the measurement that plan asked for, and the plan named this as the
  // thing to suspect: "if sd worsens, step 3 or 4 is wrong".
  //
  // MEASURED 2026-08-28, matched 4-minute windows against the analyser, correction disabled:
  //
  //     2-device, leader-based        sd 3.6 us
  //     3-device, leaderless, slewed  sd 8.06, 9.50
  //     2-device, leaderless, slewed  sd 9.72        <- group size was NOT the confound
  //
  // WHY THE SLEW WAS THE COST. A leader published ONE line and every device computed deadlines
  // from that identical line, so the mapping's own error was EXACTLY common-mode and cancelled
  // between devices -- which is how a design whose inputs wander +-100-300 us held 3.6 us on the
  // wire. A slew destroys that: two devices given the SAME estimates but different histories walk
  // different paths to the same target, so their adopted mappings differ by a wandering fraction
  // of the group's spread (measured live at 40-934 us between just two devices). That difference
  // is differential by construction and lands directly on the wire.
  //
  // WHY STEPPING IS SAFE HERE, WHICH IS WHAT THE PLAN GOT WRONG. The plan reasoned "a stepped
  // timebase IS a hard resync". True of a step that hits one device. But the mean of the live
  // estimate set is a DETERMINISTIC function of that set, so every device holding the same set
  // steps to the same value at the same time: the step is common-mode, and common-mode timebase
  // motion is inaudible -- the same argument the plan itself makes for group-wide drift. The
  // danger was never the step, it was the PATH-DEPENDENCE.
  //
  // AND THE LINE IS INDEPENDENT OF WHERE IT IS EVALUATED, which is what makes "same set -> same
  // mapping" true even though each device samples TSF at its own instant:
  //
  //     tms_X(t) = mean_i[tms_i + drift_i*(ref_X - base_i)] + mean(drift)*(t - ref_X)
  //              = mean_i[tms_i - drift_i*base_i] + mean(drift)*t          <- ref_X cancels
  //
  // So ref_tsf below is free: it sets the anchor the line is stored against, not the line.
  //
  // WHAT IS LEFT PATH-DEPENDENT, and it is bounded rather than removed: devices do not hold
  // identical sets at the same instant, because a beacon lost by one and not the other changes
  // that device's set for up to PEER_MAP_STALE_US. That is a transient disagreement of order
  // (spread / n), not an accumulating one, and it self-heals on the next beacon.
  const int64_t tms_adopt = tms_target;
  this->mapping_mutex_.lock();
  const bool held = this->mapping_valid_;
  const int64_t held_tsf_base = this->map_tsf_base_us_;
  const int64_t held_tms_base = this->map_tsf_minus_server_us_;
  const float held_drift = this->map_drift_ppm_;
  this->mapping_mutex_.unlock();
  // ONE SHORT LINE PER CONTRIBUTOR, fixed field count, no variable-length tail: the formatting
  // ceiling is 256 bytes of message and a field near the end of a long line is really a record of
  // "did the line fit". Absolute values, not deviations -- the deviations are what we already had
  // and they cannot identify a bad input.
  const auto dump = [&](const char *why, bool have_expected, int64_t expected) {
    for (size_t i = 0; i < n; i++) {
      ESP_LOGW(TAG,
               "CONSIN %s %zu/%zu %s%02X%02X tsf_base=%" PRId64 " tms_base=%" PRId64
               " drift=%+.3f tms@ref=%" PRId64 " dev=%+.0f",
               why, i + 1, n, contrib[i].self ? "SELF:" : "peer:", contrib[i].mac[4],
               contrib[i].mac[5], contrib[i].tsf_base, contrib[i].tms_base, contrib[i].drift_ppm,
               contrib[i].tms_at_ref, dv[i]);
    }
    // `expected` only exists when a mapping was held. Printed as "n/a" otherwise, never as 0:
    // a zero that reads as a measurement is how a 2^63 overflow got taken seriously on this bench
    // once already, and the whole point of this line is that someone will subtract these fields.
    char exp_str[24];
    if (have_expected) {
      snprintf(exp_str, sizeof(exp_str), "%" PRId64, expected);
    } else {
      snprintf(exp_str, sizeof(exp_str), "n/a");
    }
    ESP_LOGW(TAG,
             "CONSIN %s ref_tsf=%" PRId64 " base_tms=%" PRId64 " adopt=%" PRId64 " held=%d"
             " held_tms=%" PRId64 " held_tsf=%" PRId64 " held_drift=%+.3f expected=%s",
             why, ref_tsf, base_tms, tms_adopt, held ? 1 : 0, held_tms_base, held_tsf_base,
             held_drift, exp_str);
  };

  if (held) {
    // Diagnostics only now: report how far the timebase actually moved, so a membership change or
    // a genuine re-anchor is still visible and still tells the consumer's servo. Nothing is
    // clamped on the strength of it.
    const int64_t expected =
        held_tms_base + static_cast<int64_t>(static_cast<double>(held_drift) * 1e-6 *
                                             static_cast<double>(ref_tsf - held_tsf_base));
    const int64_t delta = tms_adopt - expected;
    if (std::abs(delta) > MAP_STEP_REPORT_US) {
      this->timebase_epoch_.fetch_add(1, std::memory_order_relaxed);
      ESP_LOGI(TAG, "Timebase step %+" PRId64 " us over %zu estimate(s) (common-mode: every device "
                    "holding this set steps identically)", delta, n);
      dump("step", true, expected);
    }
  }
  // ADOPTION WITHOUT CORROBORATION. With one contributor there is no averaging and no dilution:
  // base_tms IS that contributor's value, dv = {0}, robust_mean returns 0, and tms_adopt is its
  // mapping verbatim. It is also the state in which the per-peer plausibility gate is off, since
  // that gate requires our own estimate to be mature and n==1 usually means it is not. Measured
  // 199 such adoptions on board b in one session. Throttled, because n==1 is common and this is
  // for seeing WHICH mapping was taken on trust, not for counting them.
  if (n == 1 && local_now_us - this->last_solo_log_us_ >= 10000000) {
    this->last_solo_log_us_ = local_now_us;
    dump("solo", false, 0);
  }
  this->adopt_(ref_tsf, tms_adopt, drift_c, local_now_us);

  if (local_now_us - this->last_consensus_log_us_ >= CONSENSUS_LOG_INTERVAL_US) {
    this->last_consensus_log_us_ = local_now_us;
    // The spread across the group is the diagnostic that matters: it is what the mean is
    // averaging down, and if it grows the devices are disagreeing about server time.
    double lo = dv[0], hi = dv[0];
    for (size_t i = 1; i < n; i++) {
      lo = std::min(lo, dv[i]);
      hi = std::max(hi, dv[i]);
    }
    ESP_LOGD(TAG, "Consensus over %zu estimate(s): spread %.0f us, adopted %+" PRId64 " us from target, %+.3f ppm",
             n, hi - lo, tms_adopt - tms_target, static_cast<double>(drift_c));
  }
}

void TsfSync::log_phase_inputs(int64_t local_now_us) const {
  const int64_t mine = this->render_phase_us_.load(std::memory_order_relaxed);
  const bool mine_known = mine != RENDER_PHASE_UNKNOWN;
  char buf[192];
  // Print the sentinel as a word. `d=` below is a DIFFERENCE against this value, so an unknown own
  // phase printed as INT64_MIN turns every peer column into 2^63 of overflow -- which is not merely
  // ugly, it is unreadable exactly when the device is in the state worth reading about. Measured: a
  // 2164-row sample showed max|d| of 9.2e18 on all four peers, purely from this.
  int n = snprintf(buf, sizeof(buf), "PHASEIN mine=%s",
                   mine_known ? std::to_string(mine).c_str() : "unknown");
  for (size_t i = 0; i < MAX_PEERS && n > 0 && n < static_cast<int>(sizeof(buf)); i++) {
    if (!this->peer_[i].used || this->peer_[i].phase_us == RENDER_PHASE_UNKNOWN) {
      continue;
    }
    if (!mine_known) {
      // No baseline to difference against: report the peer's own value, not a difference from a
      // sentinel.
      n += snprintf(buf + n, sizeof(buf) - n, " | %02X%02X phase=%lld age=%lldms", this->peer_[i].mac[4],
                    this->peer_[i].mac[5], static_cast<long long>(this->peer_[i].phase_us),
                    static_cast<long long>((local_now_us - this->peer_[i].phase_seen_us) / 1000));
      continue;
    }
    // Age matters as much as the value: an entry is accepted up to PHASE_STALE_US (15 s) old,
    // and a peer that reseeded its counters inside that window contributes a phase describing a
    // position it no longer holds.
    n += snprintf(buf + n, sizeof(buf) - n, " | %02X%02X d=%+lld age=%lldms",
                  this->peer_[i].mac[4], this->peer_[i].mac[5],
                  static_cast<long long>(this->peer_[i].phase_us - mine),
                  static_cast<long long>((local_now_us - this->peer_[i].phase_seen_us) / 1000));
  }
  ESP_LOGD(TAG, "%s | group=%ld", buf,
           static_cast<long>(this->render_group_delta_us_.load(std::memory_order_relaxed)));
}

void TsfSync::record_peer_phase_(Peer &peer, int64_t phase_us, int64_t local_now_us) {
  if (phase_us == RENDER_PHASE_UNKNOWN) {
    return;  // the peer has not rendered; keep whatever it last told us
  }
  peer.phase_us = phase_us;
  peer.phase_seen_us = local_now_us;
  this->recompute_group_delta_(local_now_us);
}

void TsfSync::recompute_group_delta_(int64_t local_now_us) {
  const int64_t mine = this->render_phase_us_.load(std::memory_order_relaxed);
  // A board with no render phase of its own computes no delta and therefore emits NO GDIN. That
  // is the observer's normal state, not a missing signal: it drives no DAC, so it has no playout
  // phase to difference against -- which is precisely why it publishes PHASEIN (the consensus
  // INPUTS) instead. Do not go looking for observer GDIN lines.
  if (mine == RENDER_PHASE_UNKNOWN) {
    this->render_group_delta_us_.store(INT32_MIN, std::memory_order_relaxed);
    this->group_delta_n_.store(0, std::memory_order_relaxed);
    return;
  }
  // PAIR ONLY PHASES SAMPLED AT ROUGHLY THE SAME INSTANT. These are absolute TSF-vs-server
  // offsets that drift continuously, so differencing a fresh peer phase against a stale local one
  // measures drift, not skew. Our phase is written once per sync report (~3.3 s) while beacons
  // arrive up to 4x faster; at ~50 ppm relative drift that mismatch is ~165 us of pure error,
  // which is the whole signal. Observed directly once the observer logged the inputs: `group`
  // disagreed with -d/2 by hundreds of us on the same line.
  const int64_t mine_at = this->render_phase_at_us_.load(std::memory_order_relaxed);
  if (mine_at == 0) {
    return;  // no local instant yet; leave any previous delta alone
  }
  // Our own phase is IN the group, so with two devices each corrects half the gap and they meet
  // in the middle rather than one chasing the other.
  double vals[MAX_PEERS + 1];
  size_t n = 0;
  vals[n++] = 0.0;  // differenced against our own phase, so the doubles carry us of spread
  // STAGE 1 GDIN shadow: the pairing inputs of the MOST RECENT paired peer -- the raw pairwise
  // difference BEFORE self-inclusion (the un-halved quantity the analyser can grade against the
  // wire), the pairing gap, and the drift extrapolation. Shadow-only: nothing here steers. The
  // plan's pass condition is raw tracks the rival-clean wire at slope 1.0 +-0.15; gd (the halved
  // control-path delta) is logged beside it so the two are never confused.
  int64_t gdin_raw = 0, gdin_gap = 0, gdin_n = 0;
  double gdin_drift = 0.0, gdin_extrap = 0.0;
  for (size_t i = 0; i < MAX_PEERS && n < MAX_PEERS + 1; i++) {
    if (!this->peer_[i].used || this->peer_[i].phase_us == RENDER_PHASE_UNKNOWN ||
        local_now_us - this->peer_[i].phase_seen_us > PHASE_STALE_US) {
      continue;
    }
    const int64_t pair_gap = this->peer_[i].phase_seen_us - mine_at;
    if (pair_gap > PHASE_PAIR_WINDOW_US || pair_gap < -PHASE_PAIR_WINDOW_US) {
      continue;  // sampled too far apart to difference; wait for a fresher pairing
    }
    // Extrapolate the peer's phase to MY sample instant: phase = tsf - server drifts at the mapping rate
    // (~41 ppm here), so a sample pair_gap older reads earlier by drift x gap. Measured 2026-08-30 13:25
    // without this: each board read the other ~18 us LATER (A: d=+16, B: d=+20 at ages 0.8-1.5 s) -- a
    // common-mode bias the re-centring absorbed but that inflated every delta.
    vals[n] = static_cast<double>(this->peer_[i].phase_us - mine) -
              static_cast<double>(this->map_drift_ppm_) * 1e-6 * static_cast<double>(pair_gap);
    // GDIN describes the LAST SCANNED peer that paired -- peer_[] order, which is arrival
    // order, not recency. With one phase-contributing peer (the bench pair, since an observer
    // publishes no phase) that is the only peer and the distinction is empty; with two it is an
    // arbitrary choice among them, so do not read this line as "the closest" or "the newest".
    gdin_raw = this->peer_[i].phase_us - mine;
    gdin_gap = pair_gap;
    gdin_drift = this->map_drift_ppm_;
    gdin_extrap = static_cast<double>(this->map_drift_ppm_) * 1e-6 * static_cast<double>(pair_gap);
    gdin_n = static_cast<int64_t>(n) + 1;  // self + peers so far, matching the n used for the delta
    n++;
  }
  if (n < 2) {
    // No peer paired closely enough THIS time. Keep the last valid delta rather than reporting
    // unknown -- see group_delta_at_us_. Only give up once it is genuinely stale.
    if (this->group_delta_at_us_ != 0 &&
        local_now_us - this->group_delta_at_us_ > GROUP_DELTA_STALE_US) {
      this->render_group_delta_us_.store(INT32_MIN, std::memory_order_relaxed);
      this->group_delta_at_us_ = 0;
    }
    return;
  }
  // MEAN, NOT MEDIAN -- see render_group_delta_us(). vals are already relative to our own phase,
  // so the group average is at robust_mean() and our delta from it is the negation.
  const int64_t d = -static_cast<int64_t>(llround(robust_mean(vals, n, CONSENSUS_PHASE_SCALE_FLOOR_US)));
  const int32_t d_clamped =
      static_cast<int32_t>(std::max<int64_t>(INT32_MIN + 1, std::min<int64_t>(INT32_MAX, d)));
  this->render_group_delta_us_.store(d_clamped, std::memory_order_relaxed);
  this->group_delta_at_us_ = local_now_us;
  this->group_delta_n_.store(static_cast<uint8_t>(n), std::memory_order_relaxed);
  // Held copy: same value, never aged out here. See render_group_delta_held_us().
  this->group_delta_held_us_.store(d_clamped, std::memory_order_relaxed);
  this->group_delta_held_at_us_.store(local_now_us, std::memory_order_relaxed);
  // SHADOW GDIN (~1/s on the network task): the raw pairwise difference (un-halved, pre-mean)
  // beside the control-path halved delta, so Stage 1 can grade raw against the analyser's wire.
  // Short, fixed fields; names first, numeric tail last -- a truncation hits t= and not a name.
  if (gdin_n > 0 && local_now_us - this->last_gdin_log_us_ >= 1000000) {
    this->last_gdin_log_us_ = local_now_us;
    // ESP_LOGD, NOT ESP_LOGV: esph_log_v is compiled out below ESPHOME_LOG_LEVEL_VERBOSE and
    // the bench builds at DEBUG, so this line did not exist in the binary at all -- zero GDIN
    // lines in 46 MB of logs while the parser, its self-test and dl-window all reported green.
    // Stage 1 cannot be graded on a signal that is never emitted.
    // steady= is the emitting board's own transient state, straight off the flag
    // publish_render_phase_(!in_transient) already sets -- no new plumbing. It matters for
    // GRADING and not for control: a board in transient stops beaconing but keeps measuring and
    // using its phase locally, on purpose (the resync gate needs it while its window is open),
    // so those samples are computed from a phase whose audio is being stepped and cannot be
    // compared against the wire. Measured 2026-09-02: board B's only block that excluded slope
    // 1.0 was the one carrying 5 hard resyncs, and its residual sd was 2x board A's on an
    // identical MAD. Without this field a grader cannot tell those samples apart.
    ESP_LOGD(TAG,
             "GDIN raw=%+" PRId64 " gd=%+" PRId64 " n=%" PRId64 " gap=%+" PRId64
             " drift=%+.2f extrap=%+.2f steady=%d t=%" PRId64,
             gdin_raw, d, gdin_n, gdin_gap, gdin_drift, gdin_extrap,
             this->render_phase_broadcast_.load(std::memory_order_relaxed) ? 1 : 0, local_now_us);
  }
}

// GROUP-RELATIVE DIAGNOSTICS. Depth, crystal rate and render phase against the MEAN of the peers
// that reported them, computed on our own beacon cadence rather than on somebody else's arrival.
//
// These used to be differenced against the leader, which made every one of them referenced to
// whoever held the crown -- and the crown moved six times in seventeen minutes. Against the peer
// mean they move only when a device actually moves. Peers only, self excluded: these answer "how
// do I compare with everyone else", so including our own value in the reference would shrink
// every delta by 1/N and make a two-device pair read half its true disagreement.
//
// Diagnostics only. Nothing here touches the mapping.
void TsfSync::update_group_diagnostics_(int64_t local_now_us) {
  double pipeline_sum = 0.0, crystal_sum = 0.0, phase_sum = 0.0;
  size_t pipeline_n = 0, crystal_n = 0, phase_n = 0;
  for (const Peer &p : this->peer_) {
    if (!p.used || local_now_us - p.seen_us > PHASE_STALE_US) {
      continue;
    }
    if (p.pipeline_us != PIPELINE_UNKNOWN) {
      pipeline_sum += static_cast<double>(p.pipeline_us);
      pipeline_n++;
    }
    if (!std::isnan(p.crystal_ppm)) {
      crystal_sum += static_cast<double>(p.crystal_ppm);
      crystal_n++;
    }
    if (p.phase_us != RENDER_PHASE_UNKNOWN && local_now_us - p.phase_seen_us <= PHASE_STALE_US) {
      phase_sum += static_cast<double>(p.phase_us);
      phase_n++;
    }
  }

  // Our clock against the radio timebase, minus the group's. Both sides measure themselves
  // against the SAME AP TSF, so the AP's own crystal cancels and what is left is the difference
  // between the devices' crystals -- a hardware property, measured entirely outside the audio
  // servo loop.
  //
  // This is the term that stands between the differential trim and a usable rate reference.
  // With a logic analyser the differential trim sits -5.25..-5.40 ppm from the true
  // differential achieved rate, stable across three runs and two builds, and that offset is
  // exactly this quantity: subtracting it took the integrated error from 505 us per 100 s to 17.
  // Published so a device can compute the correction without an analyser on the bench.
  //
  // NaN means unknown, not zero: either side before its first measurement. Storing zero would
  // read as "the crystals match", which is the one answer that is never true.
  const float mine_crystal = this->pub_crystal_ppm_.load(std::memory_order_relaxed);
  if (crystal_n == 0 || std::isnan(mine_crystal)) {
    this->crystal_delta_ppm_.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
  } else {
    const float group = static_cast<float>(crystal_sum / static_cast<double>(crystal_n));
    this->crystal_delta_ppm_.store(mine_crystal - group, std::memory_order_relaxed);
    // Throttled to the render-phase cadence: it moves only with temperature, so this is already
    // far faster than the quantity changes. t= is esp_timer us, matching the other series lines.
    if (local_now_us - this->last_crystal_log_us_ >= RENDER_LOG_INTERVAL_US) {
      this->last_crystal_log_us_ = local_now_us;
      // VERBOSE: this ran on the snap_net task once a second and sat on the stack of B's 16:08:45
      // logger-ring crash (same class as 07:51). The client logs the same line from the player task.
      ESP_LOGV(TAG, "Crystal: mine %+.3f group %+.3f delta %+.3f ppm t=%" PRId64, mine_crystal, group,
               mine_crystal - group, local_now_us);
    }
  }

  // Render phase against the peer mean, logged on its own line and throttled, because this is the
  // ONLY instrument that can see an absolute inter-device offset -- the sync median compares each
  // device against its own prediction, so an offset moves prediction and audio together and reads
  // as zero there -- and in the sync report it sits behind tsf=, which the 256-byte formatting
  // ceiling truncates away exactly when the report is at its most detailed.
  //
  // NOT the value anything corrects on: this one is not pairing-window gated, so it may be
  // differencing phases sampled seconds apart and measuring drift instead of skew. It is here to
  // be plotted. render_group_delta_us(), which IS window-gated, is what acts.
  const int64_t mine_phase = this->render_phase_us_.load(std::memory_order_relaxed);
  if (phase_n > 0 && mine_phase != RENDER_PHASE_UNKNOWN &&
      local_now_us - this->last_render_log_us_ >= RENDER_LOG_INTERVAL_US) {
    this->last_render_log_us_ = local_now_us;
    const double group = phase_sum / static_cast<double>(phase_n);
    // t= is esp_timer microseconds since boot, the same clock the snapclient diagnostics stamp
    // with, so both components' series land on one axis, and the SPACING between points is the
    // device's own rather than the host's.
  // VERBOSE, not DEBUG: this line is emitted from the snap_net task once a second, and B crashed at
  // 07:51:26 (2026-08-30) inside the logger's TaskLogBuffer ring (ringbuf.c:367 assert) with exactly
  // this call on the stack -- the non-main-thread log path is the fragile one. RALIGN carries the delta.
    ESP_LOGV(TAG, "Render phase mine %+" PRId64 " group(%zu) delta %+" PRId64 " us t=%" PRId64, mine_phase,
             phase_n, mine_phase - static_cast<int64_t>(llround(group)), local_now_us);
    // Published so the PLAYER task can emit it at DEBUG without this fragile call site being
    // re-levelled (R11.1: this line crashed B inside the logger ring at 07:51). Stored, not
    // logged, here.
    this->render_phase_delta_plot_us_.store(
        static_cast<int32_t>(mine_phase - static_cast<int64_t>(llround(group))), std::memory_order_relaxed);
  }

  // Playout depth against the peer mean, with a sustained-divergence alarm.
  const int32_t mine_pipeline = this->pipeline_us_.load(std::memory_order_relaxed);
  if (pipeline_n == 0 || mine_pipeline == PIPELINE_UNKNOWN) {
    this->pipeline_delta_us_.store(INT32_MIN, std::memory_order_relaxed);
    this->pipeline_diverged_since_us_ = 0;
    return;
  }
  const int32_t group_pipeline = static_cast<int32_t>(llround(pipeline_sum / static_cast<double>(pipeline_n)));
  const int32_t delta = mine_pipeline - group_pipeline;
  this->pipeline_delta_us_.store(delta, std::memory_order_relaxed);

  if (std::abs(delta) < PIPELINE_DIVERGE_US) {
    this->pipeline_diverged_since_us_ = 0;
    return;
  }
  if (this->pipeline_diverged_since_us_ == 0) {
    this->pipeline_diverged_since_us_ = local_now_us;
    return;
  }
  // Sustained: a transient counts for nothing, since depth legitimately swings while
  // a device refills after a starvation. Only a depth that STAYS wrong is an offset.
  if (local_now_us - this->pipeline_diverged_since_us_ < PIPELINE_DIVERGE_MIN_US) {
    return;
  }
  if (this->last_diverge_log_us_ != 0 && local_now_us - this->last_diverge_log_us_ < PIPELINE_DIVERGE_LOG_US) {
    return;
  }
  this->last_diverge_log_us_ = local_now_us;
  // WARN, not DEBUG: this is inaudible to every other metric we have. The sync
  // report will look perfect while this device plays |delta| ms out from the group.
  ESP_LOGW(TAG, "Playout depth %+" PRId32 " us vs group (%" PRId32 " vs %" PRId32 " us) for %.0f s: "
                "audio is likely offset by about this much, sync reports notwithstanding",
           delta, mine_pipeline, group_pipeline,
           (local_now_us - this->pipeline_diverged_since_us_) / 1e6);
}

void TsfSync::broadcast_phase_only_(uint32_t server_id_hash, uint32_t stream_id_hash) {
  // FOR A DEVICE WITH NO ESTIMATE TO POOL -- its Kalman has not settled, so it has nothing to
  // contribute to the consensus, but it does have a render phase worth publishing.
  //
  // Deliberately NOT broadcast_(): no TSF sandwich read (45-81 us), no rate state touched,
  // multicast only. Mapping fields stay zero because a receiver seeing no_mapping != 0 records
  // the phase and returns before reading them.
  TsfPacket pkt = {};
  // Zero-init would send 0 here, which reads as a real adopted mapping. These senders carry
  // no tsf_base either, so the value would not be comparable even if it were true.
  pkt.adopted_tms_at_base_us = TMS_ADOPT_UNKNOWN;
  pkt.magic = TSF_MAGIC;
  pkt.version = TSF_VERSION;
  memcpy(pkt.bssid, this->bssid_, 6);
  memcpy(pkt.sender_mac, this->my_mac_, 6);
  pkt.no_mapping = 1;
  pkt.server_id_hash = server_id_hash;
  pkt.stream_id_hash = stream_id_hash;
  pkt.pipeline_us = this->pipeline_us_.load(std::memory_order_relaxed);
  pkt.render_phase_us = this->render_phase_for_beacon_();
  pkt.render_phase_age_ms = this->render_phase_age_ms_();
  pkt.render_bias_us = this->pub_render_bias_us_.load(std::memory_order_relaxed);
  pkt.crystal_ppm = this->pub_crystal_ppm_.load(std::memory_order_relaxed);

  struct sockaddr_in dest = {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(TSF_PORT);
  dest.sin_addr.s_addr = inet_addr(TSF_GROUP);
  sendto(this->sock_, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
}

// PUBLISH OUR OWN RAW ESTIMATE. Every device does this, once a second, unconditionally on having
// a mature Kalman -- there is no role to qualify for.
//
// THE MEASURED HAZARD AND WHY IT DOES NOT APPLY. Making followers beacon through this function
// once degraded skew stability from sd 5.4 us to sd 81.6 us, a factor of ~15, with the
// correction disabled. Three costs were blamed at the time (the TSF sandwich read, mutating the
// rate state, the unicast fan-out) but the mechanism was almost certainly the LAST LINE of the
// old version of this function: it called adopt_() on what it had just sent. A follower running
// it therefore overwrote the shared mapping with its own private line every second, so the
// group stopped sharing a timebase at all -- which is a far better explanation of a 15x
// degradation than a few hundred microseconds of radio time.
//
// That line is gone. Nothing here adopts; adoption happens exactly once, in update_consensus_(),
// from the average. If sd does worsen after this change, this is the first place to look and the
// beacon rate is the first thing to halve.
void TsfSync::broadcast_(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash,
                         uint32_t stream_id_hash) {
  int64_t tsf_now, local_mid;
  if (!sample_tsf_(tsf_now, local_mid)) {
    return;
  }

  // Track our own TSF-vs-esp_timer rate: the published drift must be in TSF units,
  // and the AP-vs-our-crystal difference usually dominates the Kalman drift.
  // Every device measures its own now, which is what publishing a raw estimate requires.
  if (this->rate_ref_local_us_ == 0) {
    this->rate_ref_tsf_us_ = tsf_now;
    this->rate_ref_local_us_ = local_mid;
  } else if (local_mid - this->rate_ref_local_us_ >= RATE_WINDOW_US) {
    const double dl = static_cast<double>(local_mid - this->rate_ref_local_us_);
    const double dt = static_cast<double>(tsf_now - this->rate_ref_tsf_us_);
    const float measured_ppm = static_cast<float>((dt - dl) / dl * 1e6);
    // Sanity: crystals differ by well under ±100 ppm; larger = TSF discontinuity
    if (std::fabs(measured_ppm) < 100.0f) {
      this->tsf_rate_ppm_ = this->tsf_rate_valid_ ? 0.5f * this->tsf_rate_ppm_ + 0.5f * measured_ppm : measured_ppm;
      this->tsf_rate_valid_ = true;
    }
    this->rate_ref_tsf_us_ = tsf_now;
    this->rate_ref_local_us_ = local_mid;
  }

  // server_now at the sandwich midpoint from our Kalman estimate
  const int64_t server_now_us =
      local_mid + static_cast<int64_t>((est.offset_ms + est.drift * ((local_mid - local_now_us) / 1000.0)) * 1000.0);
  // d(tsf−server)/dt = d(tsf−local)/dt + d(local−server)/dt = tsf_rate − kalman_drift
  //
  // THE SAME SANITY THE TSF TERM ABOVE ALREADY GETS. That one rejects past ±100 ppm as a
  // discontinuity; this one had no bound at all, and it is the same class of quantity measured
  // across the same kind of discontinuity. The guard was on the term whose failure was being
  // thought about, not on the term that could do identical damage.
  //
  // ZERO, NOT CLAMPED. A clamped garbage line is still a wrong line -- it keeps a drift the plant
  // does not have and walks the mapping away at the clamp. Zero means "no drift correction, trust
  // the offset", which is exactly what get_drift() already returns when drift is not significant,
  // and is what a healthy board publishes anyway (0 of 5000 clean samples ever cleared the
  // significance gate). So this costs a good board nothing and stops a bad one entirely.
  const float kalman_drift_ppm = static_cast<float>(est.drift * 1e6);
  const bool kalman_plausible = std::fabs(kalman_drift_ppm) <= MAX_PLAUSIBLE_DRIFT_PPM;
  if (!kalman_plausible && !this->warned_kalman_drift_) {
    this->warned_kalman_drift_ = true;
    ESP_LOGW(TAG, "DRIFTREJ own kalman %+.0f ppm implausible, publishing 0", kalman_drift_ppm);
  }
  const float drift_ppm =
      (this->tsf_rate_valid_ ? this->tsf_rate_ppm_ : 0.0f) - (kalman_plausible ? kalman_drift_ppm : 0.0f);

  // Slew-limit the published line toward the live estimate (see TMS_SLEW_MAX_US_PER_S)
  const int64_t tms_target = tsf_now - server_now_us;
  int64_t tms_pub = tms_target;
  if (this->pub_valid_) {
    const int64_t elapsed_us = tsf_now - this->pub_tsf_base_;
    const int64_t tms_expected =
        this->pub_tms_base_ +
        static_cast<int64_t>(static_cast<double>(this->pub_drift_ppm_) * 1e-6 * static_cast<double>(elapsed_us));
    const int64_t delta = tms_target - tms_expected;
    if (std::abs(delta) <= TMS_SNAP_US) {
      const int64_t rate_us_per_s =
          std::abs(delta) > TMS_CATCHUP_THRESHOLD_US ? TMS_SLEW_CATCHUP_US_PER_S : TMS_SLEW_MAX_US_PER_S;
      // At least 1 us of authority, so a very short interval cannot stall tracking entirely
      const int64_t slew = std::max<int64_t>(1, rate_us_per_s * std::max<int64_t>(0, elapsed_us) / 1000000);
      tms_pub = tms_expected + std::clamp<int64_t>(delta, -slew, slew);
    }
  }
  this->pub_valid_ = true;
  this->pub_tsf_base_ = tsf_now;
  this->pub_tms_base_ = tms_pub;
  this->pub_drift_ppm_ = drift_ppm;

  TsfPacket pkt = {};
  // Defensive default only: the real adopted value is filled in below, once tsf_base is set.
  // Zero-init would otherwise send 0 here, which reads as a genuine adopted mapping.
  pkt.adopted_tms_at_base_us = TMS_ADOPT_UNKNOWN;
  pkt.magic = TSF_MAGIC;
  pkt.version = TSF_VERSION;
  memcpy(pkt.bssid, this->bssid_, 6);
  memcpy(pkt.sender_mac, this->my_mac_, 6);
  pkt.server_id_hash = server_id_hash;
  pkt.stream_id_hash = stream_id_hash;
  pkt.no_mapping = 0;
  pkt.tsf_base_us = tsf_now;
  pkt.tsf_minus_server_us = tms_pub;
  // Our ADOPTED mapping at this same tsf_base, so a receiver can compare it against its own
  // without needing any other shared instant. Sentinel when we hold none -- never a number,
  // because the receiver subtracts this field.
  {
    this->mapping_mutex_.lock();
    const bool have_map = this->mapping_valid_;
    const int64_t mb = this->map_tsf_minus_server_us_;
    const int64_t tb = this->map_tsf_base_us_;
    const float dr = this->map_drift_ppm_;
    this->mapping_mutex_.unlock();
    pkt.adopted_tms_at_base_us =
        have_map ? mb + static_cast<int64_t>(static_cast<double>(dr) * 1e-6 *
                                             static_cast<double>(tsf_now - tb))
                 : TMS_ADOPT_UNKNOWN;
  }
  pkt.drift_ppm = drift_ppm;
  // From the player task's mirror: offset_rate_ppm_ is measured there and must not be read
  // directly from this task.
  pkt.crystal_ppm = this->pub_crystal_ppm_.load(std::memory_order_relaxed);
  {
    // Only the sentinel is out of range. The bound used to be INT16's, and the cast used to be to
    // int16_t, because v1 carried this field as int16 MILLISECONDS -- +-32 s, which no real depth
    // reaches. v2 widened it to int32 microseconds (see the version history above) but left this
    // publish site behind, so the same numbers now meant +-32767 MICROSECONDS: +-32 ms.
    //
    // Every real depth is far outside that. Measured on this fleet, they sit at 230-290 ms, so every
    // beacon has been publishing PIPELINE_UNKNOWN and the receiver's divergence check has never once
    // run. That check is the ONLY instrument that can see an absolute inter-device offset -- the sync
    // median is measured against each device's own prediction, so an offset shifts prediction and
    // audio together and reads as zero. Losing it is why a pair could sit milliseconds apart on a
    // logic analyser with both reporting themselves perfect.
    const int32_t depth = this->pipeline_us_.load(std::memory_order_relaxed);
    pkt.pipeline_us = depth;  // PIPELINE_UNKNOWN is INT32_MIN, which passes through unchanged
    pkt.render_phase_us = this->render_phase_for_beacon_();
    pkt.render_phase_age_ms = this->render_phase_age_ms_();
    pkt.render_bias_us = this->pub_render_bias_us_.load(std::memory_order_relaxed);
  }
  struct sockaddr_in dest = {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(TSF_PORT);
  dest.sin_addr.s_addr = inet_addr(TSF_GROUP);
  if (sendto(this->sock_, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest)) < 0 &&
      !this->warned_tx_) {
    this->warned_tx_ = true;
    ESP_LOGW(TAG, "Beacon multicast send failed: errno %d", errno);
  }
  // Unicast to every rostered peer: client-to-client multicast is unreliable on
  // many APs, unicast works wherever snapcast itself does. Own address may be on
  // the roster; the AP hands the packet back and the own-mac check drops it.
  for (const uint32_t addr : this->peers_) {
    dest.sin_addr.s_addr = addr;
    sendto(this->sock_, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
  }
  for (const uint32_t addr : this->learned_peers_) {
    dest.sin_addr.s_addr = addr;
    sendto(this->sock_, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
  }
  this->last_tx_us_ = local_now_us;
  // DELIBERATELY NO adopt_() HERE. What we just published is our own raw opinion; what we play to
  // is the group's average of everyone's, and update_consensus_() is the only place that decides
  // it. Adopting here would make this device play to its own line while claiming to be part of a
  // consensus -- which is what the old leader did, correctly for a leader and catastrophically
  // for everyone else (see the note above this function).
}

// 30 Hz PHASE-ONLY EXCHANGE (build 84, SHADOW). The live group delta pairs one phase sample per
// block (~1.5 Hz each side, 300 ms window) -- ~1 pairing/s with ~10 us-class noise per side, which
// is the floor under align and the sub-arm gate. Raising the beacon rate alone adds nothing (a
// re-sent sample creates no new sample instants); this path raises the SAMPLE rate (the client now
// samples per chunk) and ships each fresh sample in a no_mapping=1 packet the existing receive
// path already understands. Multicast only: the 1 Hz beacon keeps the unicast roster, and touching
// the roster from the player task would race set_peers/learn_peer_ on the network task.
// 50 Hz (every 2nd chunk). The rate only sets pairs/second -- own samples arrive at chunk cadence
// (~94 Hz) so every peer sample pairs regardless -- and per-window noise falls as ~1/sqrt(n) at
// best (chunk samples are correlated; the GDAVG-vs-live shadow measures the real gain). Beyond
// ~50 Hz the window length is the knob, not the packet rate. ~90 B x 50/s of airtime: negligible.
static constexpr int64_t PHASE_TX_INTERVAL_US = 20000;  // default; runtime knob below (WS2.0)
void TsfSync::send_phase_report(int64_t local_now_us) {
  if (local_now_us - this->phase_tx_last_us_ <
      this->phase_tx_interval_us_.load(std::memory_order_relaxed)) {
    return;
  }
  if (!this->have_mac_ || !this->have_bssid_ || this->sock_ < 0) {
    return;
  }
  const int64_t phase = this->render_phase_for_beacon_();
  const int64_t at = this->render_phase_at_us_.load(std::memory_order_relaxed);
  if (phase == RENDER_PHASE_UNKNOWN || at == 0) {
    return;  // transient, or nothing rendered: the beacon side already says so at 1 Hz
  }
  this->phase_tx_last_us_ = local_now_us;
  TsfPacket pkt = {};
  // Zero-init would send 0 here, which reads as a real adopted mapping. These senders carry
  // no tsf_base either, so the value would not be comparable even if it were true.
  pkt.adopted_tms_at_base_us = TMS_ADOPT_UNKNOWN;
  pkt.magic = TSF_MAGIC;
  pkt.version = TSF_VERSION;
  memcpy(pkt.bssid, this->bssid_, 6);
  memcpy(pkt.sender_mac, this->my_mac_, 6);
  pkt.server_id_hash = this->pub_server_id_hash_.load(std::memory_order_relaxed);
  pkt.stream_id_hash = this->pub_stream_id_hash_.load(std::memory_order_relaxed);
  pkt.no_mapping = 1;  // phase report only; mapping fields are zero and must not be read
  pkt.pipeline_us = this->pipeline_us_.load(std::memory_order_relaxed);
  pkt.crystal_ppm = this->pub_crystal_ppm_.load(std::memory_order_relaxed);
  pkt.render_phase_us = phase;
  pkt.render_phase_age_ms =
      static_cast<uint16_t>(std::clamp<int64_t>((local_now_us - at) / 1000, 0, 0xFFFE));
  pkt.render_bias_us = this->pub_render_bias_us_.load(std::memory_order_relaxed);
  struct sockaddr_in dest = {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(TSF_PORT);
  dest.sin_addr.s_addr = inet_addr(TSF_GROUP);
  sendto(this->sock_, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
  // BUILD 86: THE UNICAST ROSTER LOOP IS DISABLED, EXPERIMENTALLY. Build 85 added it (multicast
  // alone delivered n=1-2 pairs/s) and the wire stepped to a REAL, rival-clean -1.5 ms at exactly
  // 85's boot (bucketed medians: +2.2 us at 20:30-20:34, -1460 at 20:34-20:38), standing through
  // converge cycles and through a reboot of the displaced board, invisible to every on-device
  // signal. Mechanism unknown -- candidates: ~100-200 sendto/s on the tag-observation thread, two
  // tasks sending on one socket, 50 Hz unicast at non-TSF snapclients on the roster. This revert is
  // the experiment that decides whether the loop is the cause; the safe re-introduction (send from
  // the network task off a queue) comes after the wire answers.
  (void) 0;
}

void TsfSync::service(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash,
                      uint32_t stream_id_hash) {
  const int64_t since_last_service = local_now_us - this->last_service_us_;
  if (since_last_service < SERVICE_MIN_INTERVAL_US) {
    return;
  }
  this->last_service_us_ = local_now_us;
  // Mirrors for the player-task phase reports (build 84): the hashes otherwise exist only as
  // arguments on this task.
  this->pub_server_id_hash_.store(server_id_hash, std::memory_order_relaxed);
  this->pub_stream_id_hash_.store(stream_id_hash, std::memory_order_relaxed);

  if (!this->have_mac_) {
    this->have_mac_ = esp_wifi_get_mac(WIFI_IF_STA, this->my_mac_) == ESP_OK;
    if (!this->have_mac_) {
      return;
    }
  }

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    if (this->have_bssid_) {
      this->have_bssid_ = false;
      this->reset_("disassociated");
    }
    return;
  }
  if (!this->have_bssid_ || memcmp(ap.bssid, this->bssid_, 6) != 0) {
    // New BSS: its TSF timer is unrelated to the previous one's
    if (this->have_bssid_) {
      this->reset_("BSSID changed");
    }
    memcpy(this->bssid_, ap.bssid, 6);
    this->have_bssid_ = true;
  }

  if (!this->ensure_socket_()) {
    return;
  }

  this->receive_(local_now_us, est, server_id_hash, stream_id_hash);

  // PUBLISH, UNCONDITIONALLY AND SYMMETRICALLY. There is nothing to qualify for: a device with a
  // settled estimate contributes it, a device without one still reports its render phase. No
  // election, no silence timer, no health gate -- a device's own playout being unhealthy says
  // nothing about the validity of its server<->TSF estimate, and gating publication on it is what
  // produced six leadership changes in seventeen minutes.
  //
  // Maturity IS still a gate, for the one reason that survives leaderlessness: an unsettled
  // Kalman converges in 100+ ms steps, and averaging those in would drag the group's mean.
  if (est.valid && est.mature) {
    if (local_now_us - this->last_tx_us_ >= BEACON_INTERVAL_US) {
      this->broadcast_(local_now_us, est, server_id_hash, stream_id_hash);
      this->update_group_diagnostics_(local_now_us);
    }
  } else if (this->render_phase_for_beacon_() != RENDER_PHASE_UNKNOWN &&
             local_now_us - this->last_phase_tx_us_ >= 4 * BEACON_INTERVAL_US) {
    // Nothing to pool yet, but a phase worth publishing. Quarter rate: it feeds a correction that
    // steps every ~10 s, so it need not be fast, and every transmit is radio time the audio path
    // competes for.
    this->last_phase_tx_us_ = local_now_us;
    this->broadcast_phase_only_(server_id_hash, stream_id_hash);
    this->update_group_diagnostics_(local_now_us);
  }

  // Average everything live and slew the adopted mapping toward it. Runs on its own cadence
  // rather than off a beacon arrival, so a device alone in the group still keeps its mapping
  // fresh and a device losing packets still walks toward whatever it can still hear.
  if (local_now_us - this->last_consensus_us_ >= CONSENSUS_INTERVAL_US) {
    this->last_consensus_us_ = local_now_us;
    this->update_consensus_(local_now_us);
  }
}

void TsfSync::set_peers(std::vector<uint32_t> peer_addrs) {
  if (peer_addrs.size() > 16) {
    peer_addrs.resize(16);
  }
  if (peer_addrs != this->peers_) {
    ESP_LOGD(TAG, "Unicast peer roster: %zu entries", peer_addrs.size());
  }
  this->peers_ = std::move(peer_addrs);
  // Drop learned entries the roster now covers (avoids double-sends)
  this->learned_peers_.erase(std::remove_if(this->learned_peers_.begin(), this->learned_peers_.end(),
                                            [this](uint32_t a) {
                                              return std::find(this->peers_.begin(), this->peers_.end(), a) !=
                                                     this->peers_.end();
                                            }),
                             this->learned_peers_.end());
  this->update_peer_count_();
}

void TsfSync::learn_peer_(uint32_t addr) {
  if (std::find(this->peers_.begin(), this->peers_.end(), addr) != this->peers_.end() ||
      std::find(this->learned_peers_.begin(), this->learned_peers_.end(), addr) != this->learned_peers_.end() ||
      this->learned_peers_.size() >= 16) {
    return;
  }
  this->learned_peers_.push_back(addr);
  this->update_peer_count_();
}

// THREAD CONTEXT: player task
bool TsfSync::shared_server_offset_us(int64_t local_now_us, int64_t &offset_us) {
  this->mapping_mutex_.lock();
  const bool valid = this->mapping_valid_ && (local_now_us - this->map_updated_local_us_) <= MAPPING_EXPIRY_US;
  const int64_t tsf_base = this->map_tsf_base_us_;
  const int64_t tms_base = this->map_tsf_minus_server_us_;
  const float drift_ppm = this->map_drift_ppm_;
  this->mapping_mutex_.unlock();
  if (!valid) {
    this->offset_filter_valid_ = false;
    return false;
  }
  int64_t extrapolation_us;
  int64_t raw_us;
  int64_t sandwich_us = 0;
  int64_t sample_tsf_us = 0;
  int64_t sample_local_us = 0;
  const EvalResult ev = evaluate_mapping_(tsf_base, tms_base, drift_ppm, raw_us, extrapolation_us, &sandwich_us,
                                          &sample_tsf_us, &sample_local_us);
  if (ev != EvalResult::OK) {
    // HOLD THROUGH A BLIP. A single unreadable TSF sample (NO_TSF) used to answer "no shared mapping"
    // for that one chunk: the caller's deadline flipped to the local Kalman (~100 us away), the
    // coarse path stepped the audio to it, the delay loop held and re-engaged, and the peer aligned
    // to a board that had really moved. 2026-08-30: 2-32 such flips per board per hour, six in the
    // 2.5 minutes before the 13:14 step test. The mapping is still valid (expiry is 5 s) and the
    // filter still holds a value; answer with it, carried forward by the measured crystal ratio
    // exactly as the feed-forward below does, for up to SHARED_HOLD_GRACE_US. An AGE_CLAMP (TSF
    // reset) is a real fallback and is not held.
    if (ev == EvalResult::NO_TSF && this->offset_filter_seeded_ && this->offset_filter_local_us_ != 0 &&
        local_now_us - this->offset_filter_local_us_ <= SHARED_HOLD_GRACE_US) {
      double held = this->offset_filter_us_;
      if (this->offset_rate_valid_) {
        held += (this->offset_rate_ppm_ - static_cast<double>(drift_ppm)) * 1e-6 *
                static_cast<double>(local_now_us - this->offset_filter_local_us_);
      }
      if (!this->shared_hold_logged_) {
        this->shared_hold_logged_ = true;
        ESP_LOGD(TAG, "Shared mapping: TSF sample failed, holding the filtered offset (age %" PRId64 " ms)",
                 (local_now_us - this->offset_filter_local_us_) / 1000);
      }
      offset_us = static_cast<int64_t>(held);
      return true;
    }
    this->offset_filter_valid_ = false;
    return false;
  }
  this->shared_hold_logged_ = false;

  // Low-pass the offset. Every call takes a FRESH sandwiched TSF sample, so its read noise --
  // up to SANDWICH_MAX_US, and uncorrelated between devices, therefore NOT cancelled by sharing
  // the mapping -- lands directly in each chunk's deadline. Measured on four clients: the
  // per-chunk sync error is white noise (consecutive-difference sigma / sigma = 1.32-1.43,
  // versus sqrt(2) = 1.41 for pure white noise) at ~270 us, which is an order of magnitude
  // coarser than stereo imaging tolerates while being perfectly adequate room-to-room.
  //
  // Filtering costs almost no lag because the quantity is already drift-compensated: the
  // published mapping carries drift_ppm and evaluate_mapping_ extrapolates with it, so what
  // remains should be near-constant and any real movement is ppm-scale. Reset on a new mapping
  // so a re-anchor steps through immediately instead of being smeared.
  // Track this device's bracket floor over a bounded block, so the trust threshold below
  // is derived from reads that actually happened rather than assumed. The block bound lets
  // the floor rise if the device becomes genuinely busier instead of latching a lucky
  // early read forever.
  this->sandwich_block_min_us_ =
      this->sandwich_block_min_us_ == 0 ? sandwich_us : std::min(this->sandwich_block_min_us_, sandwich_us);
  if (++this->sandwich_block_n_ >= SANDWICH_FLOOR_BLOCK || this->sandwich_floor_us_ == 0) {
    this->sandwich_floor_us_ = this->sandwich_block_min_us_;
    this->sandwich_block_min_us_ = 0;
    this->sandwich_block_n_ = 0;
  }
  const int64_t trust_us = this->sandwich_floor_us_ * SANDWICH_TRUST_FACTOR;

  // Track d(tsf - local)/dt from the samples already in hand, and use it to DE-TREND the filter
  // input. This is the fix for the static inter-device offset described at length below: the
  // filter's input is a ramp, an EWMA lags a ramp by rate * tau, and because the two devices'
  // crystals differ (measured 4.8 ppm apart) their lags differ and the DIFFERENCE lands on the
  // wire -- 32 us of it at tau 6.7 s, against a measured static skew of -42 to -50 us.
  //
  // Predicting the ramp forward between calls removes the lag on each device, so both sit at
  // ~zero and the difference goes with it. What makes this safe where the reverted alpha-beta
  // tracker was not: the rate here is NOT estimated from the filter's own residuals -- there is
  // no feedback path, so no way for a rate error to be amplified by the loop it drives -- and it
  // is a pure hardware ratio between two oscillators, which is why it can be averaged over
  // minutes. A mapping re-anchor or a slew cannot corrupt it, because neither of
  // those touch either clock. The residual is the rate ERROR times tau: sub-0.5 ppm gives ~3 us.
  //
  // Only trusted sandwiches are used as endpoints, for the same reason they are the only reads
  // allowed to move the filter -- a wide bracket is interrupt latency, and here it would be
  // divided by the baseline and become a rate error that persists for the whole EWMA.
  if (sandwich_us <= trust_us) {
    if (this->offset_rate_ref_local_us_ == 0) {
      this->offset_rate_ref_tsf_us_ = sample_tsf_us;
      this->offset_rate_ref_local_us_ = sample_local_us;
    } else if (sample_local_us - this->offset_rate_ref_local_us_ >= OFFSET_RATE_WINDOW_US) {
      const double dl = static_cast<double>(sample_local_us - this->offset_rate_ref_local_us_);
      const double dt = static_cast<double>(sample_tsf_us - this->offset_rate_ref_tsf_us_);
      const double measured_ppm = (dt - dl) / dl * 1e6;
      if (std::fabs(measured_ppm) < OFFSET_RATE_MAX_PPM) {
        this->offset_rate_ppm_ = this->offset_rate_valid_
                                     ? this->offset_rate_ppm_ + OFFSET_RATE_ALPHA * (measured_ppm - this->offset_rate_ppm_)
                                     : measured_ppm;
        this->offset_rate_valid_ = true;
        // Mirror for the network task to beacon. Stored here rather than computed there
        // because this is the all-roles measurement; see TsfPacket::crystal_ppm.
        this->pub_crystal_ppm_.store(static_cast<float>(this->offset_rate_ppm_),
                                     std::memory_order_relaxed);
        // Once per baseline (~8 s), i.e. slower than the sync report, and it cannot go in that
        // report: that line is already at the 256-byte formatting ceiling and silently truncates
        // its last field. Both terms are printed because the DIFFERENCE is what the filter
        // predicts with, and a fault in either one shows as a drifting static skew that no
        // on-device field would otherwise name.
        ESP_LOGD(TAG, "Offset ramp %+.2f ppm (tsf-local %+.2f, map %+.2f), raw %+.2f",
                 this->offset_rate_ppm_ - static_cast<double>(drift_ppm), this->offset_rate_ppm_,
                 static_cast<double>(drift_ppm), measured_ppm);
      }
      this->offset_rate_ref_tsf_us_ = sample_tsf_us;
      this->offset_rate_ref_local_us_ = sample_local_us;
    }
  }

  // Feed the ramp forward before comparing against the raw sample, so the snap test below judges
  // the disagreement against where the filter SHOULD be rather than where it was left.
  //
  //   d(raw)/d(local) = d(tsf - local)/dt - d(tsf - server)/dt
  //
  // the first term measured above, the second read straight off the mapping (no differencing, so
  // no step sensitivity). What is deliberately NOT predicted is the adoption slew of tms_base --
  // up to TMS_SLEW_MAX_US_PER_S, and not knowable from the mapping alone -- so the filter still
  // lags that. That lag is shared: the slew is identical on every device and the lags differ only
  // by the crystal ratio, so it stays common-mode, which is the property that matters.
  if (this->offset_filter_seeded_ && this->offset_rate_valid_ && this->offset_filter_local_us_ != 0) {
    const int64_t gap_us = std::clamp<int64_t>(sample_local_us - this->offset_filter_local_us_, 0, OFFSET_FF_MAX_GAP_US);
    this->offset_filter_us_ +=
        (this->offset_rate_ppm_ - static_cast<double>(drift_ppm)) * 1e-6 * static_cast<double>(gap_us);
  }
  this->offset_filter_local_us_ = sample_local_us;

  // Snap on a genuine step, filter otherwise. NOT keyed on adopt_: that runs once per
  // beacon, so re-anchoring there would reset the filter every second and filter nothing.
  // Consecutive mappings differ by at most the slew rate times the interval, so anything
  // this large is a genuine re-anchor, not slew and not read noise.
  //
  // "Not valid" is NOT the same as "nothing to carry over". The filter is invalidated whenever the
  // mapping is momentarily unavailable -- expired, or a failed evaluation. Under leader election
  // that happened at every handover; leaderless it happens on a beacon outage or a TSF read
  // failure, which is rarer but not rare. Snapping then discards a filter that was perfectly good
  // and jumps to raw by
  // whatever LAG it had accumulated, and that lag is drift * tau: at the measured -50 ppm and a
  // 6.7 s constant it is ~340 us. So a routine handover between devices that already agreed
  // produced a ~500 us step in one device's deadline, landing entirely on the wire as differential
  // skew. Measured with the de-trended tbjit field: 1-4 us in steady state, 525 us at the handover,
  // alongside that device's median hitting 792 us and its trim +234 ppm.
  //
  // It also got worse when the filter was smoothed harder -- 4x the time constant is 4x the lag and
  // so 4x the step -- which is the sort of coupling that only shows up once the artefact is
  // measurable.
  //
  // So carry the filter across an invalidation and let the size of the disagreement decide. A step
  // beyond OFFSET_SNAP_US is a genuine re-anchor and still snaps, because smearing a real
  // correction over seconds is worse. Anything smaller smooths, including across a handover.
  // seeded_ is the "have we ever had a value" flag, and unlike valid_ it is never cleared.
  // REVERTED: an alpha-beta tracker here. The diagnosis behind it was right and the cure was
  // worse.
  //
  // The input IS a ramp -- this device's local clock against the server's, -48 to -52 ppm after
  // evaluate_mapping_ has removed the TSF-vs-server drift -- and an EWMA lags a ramp by
  // rate * tau. Because each device has its own crystal the lags DIFFER, and the difference is a
  // static inter-device offset:
  //
  //   a -52.4 ppm, b -47.6 ppm -> 4.8 ppm apart
  //   tau 1.7 s -> 8 us apart      (observed static skew -14.7 us)
  //   tau 6.7 s -> 32 us apart     (observed static skew -50.3 us)
  //
  // Estimating the rate does remove that lag. But an EWMA's lag is DETERMINISTIC, so the
  // difference between two devices is bounded and predictable; an alpha-beta's steady-state
  // position depends on each device's own rate ESTIMATE, which converges independently and
  // noisily, and a small rate error integrated over time is a far larger position error than the
  // lag it replaced. Measured: static skew went from -50 us to -567 us, stable, with both devices
  // reporting agreement within ~20 us and tbjit at 1-3 us -- i.e. invisible on-device, which is
  // the exact failure class this work exists to remove.
  //
  // So a bounded predictable error beats an unbounded unpredictable one -- but bounded is not
  // free, and 32 us of it was the largest single term left on the wire. It is now removed by the
  // feed-forward above, which differs from the alpha-beta in the one way that matters: the rate
  // it uses is a measured crystal ratio, taken from the sandwich samples and never from the
  // filter's own error, so it cannot be driven by the loop it corrects. The EWMA still owns the
  // POSITION -- a rate error only shows up multiplied by tau, it cannot integrate.
  //
  // Carrying the filter across an invalidation is kept, and is separate from the above: the filter
  // is invalidated whenever the mapping is momentarily unavailable, and snapping then discarded a
  // good filter and jumped to raw by its whole accumulated lag -- measured as tbjit 525 us and a
  // ~500 us wire excursion at what was then a leadership handover.
  // seeded_ is "have we ever had a value" and is never cleared, so only a genuine re-anchor beyond
  // OFFSET_SNAP_US still snaps.
  if (!this->offset_filter_seeded_ ||
      std::abs(static_cast<double>(raw_us) - this->offset_filter_us_) > OFFSET_SNAP_US) {
    this->offset_filter_us_ = static_cast<double>(raw_us);
    this->offset_filter_valid_ = true;
    this->offset_filter_seeded_ = true;
  } else if (sandwich_us <= trust_us) {
    this->offset_filter_valid_ = true;
    // Only reads near this device's own floor move the filter. A wider bracket is mostly
    // interrupt latency, and averaging it in spends filter authority on known-bad data.
    this->offset_filter_us_ += OFFSET_EWMA_ALPHA * (static_cast<double>(raw_us) - this->offset_filter_us_);
  }
  // Instrumentation for the post-window wire tail (build 79, 18:14-18:21): on-device err_tag and
  // ledger both read ~0 while the wire and gd read -150..-450 us, i.e. the deadline itself moved.
  // The one per-board term in the deadline is THIS filter's state, and nothing on this path logs at
  // walk-sized granularity (snap is 2000 us and silent below it). One short line, ~0.5 Hz: the
  // raw-vs-filter gap, how far the filter moved since the last line, the sandwich width and trust
  // floor, and the call gap the feed-forward bridged. All fields bounded.
  {
    const int64_t now_dbg = local_now_us;
    if (now_dbg - this->offdbg_at_us_ >= 2000000) {
      const int64_t dflt =
          this->offdbg_at_us_ == 0 ? 0 : static_cast<int64_t>(this->offset_filter_us_ - this->offdbg_flt_);
      ESP_LOGD(TAG, "OFFDBG rawgap=%+" PRId64 " dflt=%+" PRId64 " sandw=%" PRId64 " floor=%" PRId64 " trust=%d",
               std::clamp<int64_t>(raw_us - static_cast<int64_t>(this->offset_filter_us_), -9999999, 9999999),
               std::clamp<int64_t>(dflt, -9999999, 9999999), sandwich_us, this->sandwich_floor_us_,
               sandwich_us <= trust_us ? 1 : 0);
      this->offdbg_at_us_ = now_dbg;
      this->offdbg_flt_ = this->offset_filter_us_;
    }
  }
  offset_us = static_cast<int64_t>(this->offset_filter_us_);
  return true;
}

float TsfSync::mapping_age_s(int64_t local_now_us) {
  this->mapping_mutex_.lock();
  const bool valid = this->mapping_valid_;
  const int64_t updated = this->map_updated_local_us_;
  this->mapping_mutex_.unlock();
  if (!valid) {
    return -1.0f;
  }
  return static_cast<float>(local_now_us - updated) * 1e-6f;
}

}  // namespace esphome::clock_sync

#endif  // CLOCK_SYNC_TSF_ACTIVE
