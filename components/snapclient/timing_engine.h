#pragma once
//
// Playout timing engine: observations in, commands out. No protocol knowledge.
//
// Sign convention, used everywhere:
//
//     e = actual_render_instant − intended_deadline   (us)
//     e > 0  played late  -> play faster, or drop frames
//     e < 0  played early -> play slower, or insert silence
//
// Rules this file holds to:
//   * No constant in chunks, frames, arrivals or samples. Codec and rate changes rescale those.
//     Everything is us, ppm, or derived from Profile.
//   * No per-board gain: a gain only one device has turns common error into differential motion.
//   * Gain is set by the noise budget, not by the error's magnitude.
//
// Budget. Position is the integral of rate, so rate-command noise is a position noise budget:
//
//     sigma_rate <= sigma_position / rate_horizon_us   (rate's own delay, not the buffer's)
//
// Rate is also the only continuous actuator, so it owns sub-frame position by necessity;
// position actuators are quantised to one frame.
//
// Delivered position corrections feed the rate estimate (credit_position_correction). Without
// that, the fast path removes the error before the integral can learn a real plant offset, and
// the offset is paid for indefinitely in frame corrections.

#include <cstdint>

namespace esphome::snapclient::timing {

/// Rate- and codec-dependent values, supplied by the transport.
///
/// TWO measured delays, because the two actuators do not share one.
///
///   RATE acts at the DAC. Changing the I2S clock shifts the render instant of everything already
///   buffered, immediately, so rate's dead time is only how long the error takes to be MEASURED
///   and reported -- the pipeline, ~250 ms. The ring is not in its loop at all.
///
///   POSITION acts at the ring INPUT. A dropped frame has to drain the whole ring before the DAC
///   or the measurement can see it, so its dead time carries the buffer: ring + pipe, ~1.25 s
///   measured on the wire (93% of corrections produce a wire step at that lag).
///
/// Sizing rate from position's delay costs a factor of 6 in position error -- measured at median
/// |e| 60 us against 9 us -- because it throttles a loop that has no reason to be slow. It also
/// makes a bigger network buffer degrade clock sync, which it has no business doing.
struct Profile {
  uint32_t frame_rate_hz = 44100;
  /// Render instant -> reported error. RATE's dead time.
  int64_t measurement_lag_us = 250000;
  /// Ring + pipe: a delivered frame correction -> the DAC. POSITION's dead time.
  int64_t position_delay_us = 1250000;
  /// NEIGHBOUR TARGET: how far this board may sit from its PEERS. The audible one.
  ///
  /// Everything sized from this acts on the DIFFERENTIAL -- the position gate tests
  /// |e_diff| >= this, and the rate-command noise budget below feeds Kp, which multiplies e_diff.
  /// So it is a differential-position target throughout, and naming it after position alone
  /// invited the confusion that a single number could serve both errors.
  ///
  /// Tight, because a differential error is the only thing a listener can hear.
  int64_t target_diff_us = 20;

  /// COMMON-MODE TARGET: how far the WHOLE GROUP may sit from the server's deadline. The
  /// inaudible one, and therefore a completely different number.
  ///
  /// A common error shifts every device equally, so no listener can detect it; what it costs is
  /// headroom, because it grows toward the hard-resync threshold and the repair is what breaks
  /// the pair. Measured 2026-09-03 with the loop healthy: both boards sat +1192 and +1282 us from
  /// their deadlines TOGETHER while holding 18 us sd on the wire. That is a perfectly good state
  /// and nothing should spend rate noise correcting it.
  ///
  /// So this is a DEADBAND, not a setpoint: the shared common drain does nothing while the group
  /// is inside it, and works on the excess beyond it. One knob could not express that -- sized
  /// for the differential it chases milliseconds of inaudible error, and sized for the common it
  /// abandons the microseconds that matter.
  int64_t target_common_us = 5000;
  /// Buffer level below which the board is treated as STARVED and the loop holds. Supplied by
  /// the transport, because only it knows the ring's capacity.
  ///
  /// This was keyed to measurement_lag_us when that meant the pipeline (~250 ms). measurement_lag
  /// then became the measured observation cadence (~47 ms), and the threshold silently fell 5x
  /// with it -- so the guard stopped firing while the ring drained to 500 ms and below, 65 times
  /// in one session. Two quantities that happened to be close, and then one of them moved.
  int64_t buffer_floor_us = 0;
  /// How much rate the loop may command on top of the crystal, from the transport that owns the
  /// actuator. AUTHORITY, not noise: Kp = budget/sigma_e already bounds the injected noise, so
  /// clamping the output to the budget as well capped correction at one sigma and made position
  /// pay for every plant wander. Sized by the plant's ppm wander, so it does not scale with rate,
  /// buffer or codec. Not a constant in this file -- it is a property of the hardware.
  float rate_authority_ppm = 100.0f;

  /// STABILITY CAP ON Kp, as a fraction of the delay-limited critical gain. 0 disables it.
  ///
  /// Kp = budget/sigma_e bounds NOISE INJECTION and says nothing about phase margin. Rate drives
  /// position through an integrator with rate_horizon_us of dead time, and for that plant the
  /// critical proportional gain is Kp_crit = pi/(2L). Where budget/sigma_e lands relative to that
  /// depends entirely on sigma_e -- so a gain can satisfy the noise budget and still ring.
  ///
  /// Measured 2026-09-03, bench L = 2.0 s so Kp_crit = 0.785 /s:
  ///
  ///     target=20  budget 10 ppm / sigma_e 22  ->  Kp 0.455  = 58% of critical
  ///     target=10  budget  5 ppm / sigma_e 22  ->  Kp 0.227  = 29% of critical
  ///
  /// At 58% the loop is lightly damped and rings; the period for a delay-dominated loop near that
  /// point is ~4L = 8 s against 10-14 s measured on the wire, and p2p tracked the gain (target 20
  /// -> 106-146 us, target 10 -> 88-96). Lowering the target improved BOTH p2p and sd, which is
  /// over-gain rather than a trade.
  ///
  /// This also reconciles the simulator, which disagreed all night: its sigma_e is measured at ~80
  /// rather than floored at 22, so the same target puts it at 16% of critical -- UNDER-gained,
  /// where the bench is over-gained. Lowering the target damped the bench and made the sim
  /// sluggish. Opposite sides of the same optimum, not a fidelity bug.
  ///
  /// With the cap in force the target stops controlling ringing, which is what it should never
  /// have controlled: it is a position-accuracy budget, not a phase margin.
  /// 0.20, chosen with margin from a CLIFF rather than for its own number. Swept at bench-like
  /// sigma_e (tests/group 3j, noise 20 us so sigma_e floors at one frame as it does on hardware),
  /// median 2-minute p2p:
  ///
  ///     frac      off    0.30    0.20    0.12    0.07
  ///     tgt=10    7.0    7.0     5.3     3.7    96.1   <- unstable
  ///     tgt=20   10.7    7.4     5.2     3.5     2.8
  ///
  /// 0.12 is better still and sits next to a cliff: at 0.07 both targets cap Kp to the same 0.063
  /// yet one is stable and the other is not, because target_diff_us ALSO gates how often the
  /// crystal integral runs. Starving P while changing the integral's cadence goes unstable, and
  /// the sim's L (1.75 s) and noise are only an approximation of the bench's, so the cliff's exact
  /// position on hardware is unknown. 0.20 keeps a factor of ~3 from it.
  ///
  /// It also makes the loop nearly INDIFFERENT to the target -- 5.3 at tgt=10 against 5.2 at
  /// tgt=20 -- which is the point: the target is a position-accuracy budget and should never have
  /// been what set phase margin.
  ///
  /// On the bench (L = 2.0 s) this gives Kp_max = 0.157 against a current Kp of 0.227 at
  /// target=10, so it binds and reduces the gain ~31%.
  float kp_stability_frac = 0.20f;

  /// SHARED COMMON-MODE CORRECTION: SECONDS to drain the group's shared common error over.
  /// 0 = off, which is today's behaviour exactly.
  ///
  /// Today nothing corrects the common error promptly. The crystal integral absorbs it, slowly,
  /// and that is what winds the estimate: a starved board is behind for the whole shortage AND
  /// the whole catch-up, one-signed throughout, so the integral learns a DISPLACEMENT as a
  /// permanent rate. Measured 2026-09-02: a hand-zeroed crystal at +192 ppm against a true +46,
  /// 8 ppm off the rail. The harm is not that the clock cannot keep up -- it has 2000 ppm of
  /// actuator for a 46 ppm job -- but that a railed integral has no headroom and trips
  /// crystal_spent, which hands the work to POSITION, which is audible.
  ///
  /// NOT a fraction per rate horizon, which is what this was first written as and why it failed.
  /// Over a 2 s horizon a 14 ms common error asks for 7000 ppm, so the term pinned at its clamp
  /// for every gain above ~0.005 and a gain sweep returned four identical rows. The horizon is
  /// the wrong timescale: it is how far ahead rate can SEE, not how fast the common error must
  /// go away.
  ///
  /// The real budget is "stay under the 50 ms hard-resync threshold between disturbances", which
  /// is minutes. At 350 ppm of total authority a 51 ms error walks off in 146 s, and at a shared
  /// 50 ppm in 17 min, against a measured disturbance interval of ~13 min on this bench. So the
  /// correction is a bounded, sustained offset, not a fast null -- and unlike the horizon form it
  /// stays proportional for small errors instead of saturating immediately.
  float common_drain_s = 0.0f;

  /// HOLD THE GROUP DELTA WHILE THIS BOARD IS STEPPING ITS OWN AUDIO. Off by default (today's
  /// behaviour exactly), so it can be A/B'd rather than assumed.
  ///
  /// The filter clamps a wild innovation to gmax and then EWMAs it in; it never REJECTS one. So a
  /// 25 ms phantom enters as several hundred us, P = Kp * gd turns that into tens of ppm, and the
  /// slew limiter renders it as a smooth ramp out and back -- the 2 pi cycle the bench plots in
  /// d(rate)/dt. Position corrects a displacement and rate then corrects the correction.
  ///
  /// Holding, not zeroing: the last good differential is the best estimate available while the
  /// board's own phase is meaningless, and zeroing would itself be a step.
  bool gate_gd_on_transient = false;

  /// POSITION MUST NOT SPEND FRAMES ON AN UNVALIDATED DEADLINE ERROR when peers exist.
  ///
  /// `bool differential = true;` is a fail-open initialiser: when the group is absent it is never
  /// overwritten, so differential stays true, have_diff stays false, e_position becomes the
  /// DEADLINE error, and position spends irreversible frames on it with no differential check.
  ///
  /// That is right for a LONE client -- tracking the server is the only meaning "in sync" has when
  /// there is nobody to be in sync with -- and wrong for a board that HAS peers and has merely
  /// lost its delta, which is a signal that has gone missing rather than a signal that says zero.
  ///
  /// Measured 2026-09-03 04:23, board b, with ARRGAP max 130-152 ms (no supply event) and board a
  /// untouched: ESPLIT dif=n/a, then act=2 why=4 frames=+1491 (33.8 ms, matching err=+32815, the
  /// deadline error) followed by frames=-195 four seconds later. The wire went to -4758 us.
  ///
  /// Reproduced in tests/group 3l once the peer stays silent longer than the delta's stale window,
  /// so the HELD value expires and present actually goes false:
  ///
  ///     control                          p2p  33.9 us   corr  0   frames   0
  ///     1 chunk 3 s + peer silent 20 s   p2p 326.3 us   corr 17   frames 276
  ///     2 chunks 3 s + peer silent       p2p 519.6 us   corr 17   frames 288
  ///
  /// With this on, a board that has peers but no delta HOLDS instead: rate still works, the hard
  /// resync still covers a genuine large displacement, and no irreversible frames are spent on an
  /// error nothing has corroborated.
  bool position_needs_diff = true;

  /// Shift gd_mean_us_ by the delivered displacement when a correction came from the DIFFERENTIAL,
  /// exactly as err_mean_us_ is already shifted. Without it the differential filter decays back
  /// through the coarse gate and buys the same correction repeatedly -- measured at 25-54% of all
  /// frames spent (tests/group 3k, decayF).
  /// OFF: it FAILED its own falsifiable test. decayF was predicted to collapse toward zero and
  /// instead the 4 s case came out byte-identical (2194 frames, so the branch never engaged there)
  /// and the 40 s case improved 12% (10431 -> 9142), inside the variance already seen in those
  /// rows. Three possibilities remain unseparated: have_diff is false at those corrections, in
  /// which case err_mean_ is already compensated and the decay has a third cause; the (n-1)/n
  /// factor is wrong; or decayC is counting corrections that are not decay-driven. Kept, off, with
  /// the numbers, rather than deleted -- the reasoning is sound and the test is what disagreed.
  bool compensate_gd_filter = false;

  /// SIZE Kp FROM THE DIFFERENTIAL'S NOISE rather than the deadline error's, whenever the group
  /// supplies a differential. Off by default so it can be A/B'd on hardware; see the note at its
  /// use for why the current default is a known mis-sizing rather than a choice.
  bool kp_from_diff_sigma = false;

  /// Ceiling on the shared correction, separate from rate_authority_ppm because it answers a
  /// different question: authority bounds what this board may do ALONE (and so bounds the
  /// differential it can inject), while this bounds a motion the whole group makes together and
  /// which is inaudible by construction. Sized to leave the integral's job small rather than to
  /// bound audibility.
  float common_authority_ppm = 50.0f;

  int64_t frame_us() const { return 1000000 / static_cast<int64_t>(frame_rate_hz); }

  /// Lag the error filter adds. A multiple of the TRANSPORT delay (compensation_us), not of the
  /// measurement lag: the filter's job is rejecting measurement noise and the tens-of-ms
  /// transients this bench delivers, and a filter as short as the pipeline passes them straight
  /// into the crystal -- measured railing the estimate to its 200 ppm clamp. So the filter stays
  /// slow, and rate's horizon carries that lag honestly rather than pretending to be faster.
  /// Error-filter length in compensation horizons: filter_lag = this * compensation_us().
  /// Default 1.0 is the compiled ERR_TAU_HORIZONS. Sweepable because it, not the crystal integral,
  /// is what sets the loop's oscillation period (tests/group 3b).
  float err_tau_horizons = 1.0f;

  int64_t filter_lag_us() const;

  /// SPLIT FILTER, off by default (0 = use filter_lag_us() for everything, i.e. today's behaviour).
  ///
  /// One EWMA currently serves two actuators with very different physics. Its time constant is
  /// ERR_TAU_HORIZONS * compensation_us(), and compensation_us() is dominated by position_delay_us
  /// (~1.25 s of ring and pipe) -- so RATE's knowledge of the error is smoothed on the timescale of
  /// an actuator rate does not use. Measured on the bench: rate_horizon 2.0 s, of which the
  /// position delay is the bulk, while rate's own measurement lag is ~250 ms.
  ///
  /// The filter is slow for a reason: position is irreversible and quantised to a frame, so it must
  /// not fire on noise. Rate is continuous and reversible and pays no such penalty for being wrong
  /// briefly. Sharing one filter buys the guarantee that the two actuators never contradict each
  /// other, and costs rate an order of magnitude in responsiveness.
  ///
  /// Whether that trade is right is a design question, not a tuning one, which is why this exists
  /// as a Profile field the simulator can sweep rather than as a constant someone edits.
  int64_t rate_filter_lag_us = 0;

  /// The horizon the RATE path filters on: the split value when set, else the shared one.
  int64_t rate_filter_lag_effective_us() const {
    return rate_filter_lag_us > 0 ? rate_filter_lag_us : filter_lag_us();
  }

  /// RATE's loop delay: measure it, then filter it. Sets Kp, the integral's bandwidth, and how
  /// far rate can reach before position has to act.
  int64_t rate_horizon_us() const { return measurement_lag_us + filter_lag_us(); }

  /// Window over which a delivered correction is still absent from a RAW observation: it must
  /// reach the DAC and then be measured. No filter lag -- compensation applies to raw samples.
  int64_t compensation_us() const { return position_delay_us + measurement_lag_us; }

  /// When the FILTERED error contains a delivered correction. Serialises position, and sets the
  /// credit baseline and the in-flight expiry.
  int64_t settle_us() const { return compensation_us() + filter_lag_us(); }

  /// sigma_rate <= sigma_position / horizon, over RATE's horizon: the command noise integrates
  /// into position over the time rate takes to respond, which the ring does not lengthen.
  float rate_noise_budget_ppm() const {
    const int64_t h = rate_horizon_us();
    if (h <= 0) return 0.0f;
    return 1e6f * static_cast<float>(target_diff_us) / static_cast<float>(h);
  }
};

/// One measured render event.
struct Observation {
  int64_t at_us = 0;
  int64_t error_us = 0;   ///< e
  bool valid = false;     ///< false: no measurement, engine holds
  /// Audio queued ahead of the DAC. Position corrections are SPENT FROM THIS: dropping frames
  /// removes audio from the buffer, so a loop that drops to fix a late error drains the very
  /// thing that lets it play continuously. Measured on the bench: board a dropped 54951 frames
  /// (1.25 s of audio) from a 1724 ms buffer, drained the ring to 26 ms, then starved -- and a
  /// starved board falls further behind, which reads as a LATER error, which buys more drops.
  /// 122 ms of "error" growing 3 ms per report, entirely self-inflicted.
  /// 0 means unknown, and the engine then applies no buffer reasoning.
  int64_t buffer_us = 0;
};

/// Differential evidence. Absent on a transport without a shared clock; the engine then runs
/// without arbitration.
struct GroupEvidence {
  bool present = false;
  int64_t delta_us = 0;      ///< offset from the group, same sign convention
  int64_t age_us = 0;
  uint8_t contributors = 0;  ///< phase contributors including self

  /// THE GROUP'S SHARED COMMON ERROR: the consensus of every member's e_common, computed the way
  /// the mapping is (robust mean over the set, self included), so every device holding the same
  /// set derives the SAME number.
  ///
  /// That sameness is the whole point, and it is why this may be acted on at a gain the crystal
  /// integral may not. A per-board gain on a per-board estimate turns common error into
  /// differential motion at gain x (noise_A - noise_B) -- the rule stated at the head of this
  /// file, and measured: a resync-window boost on one board alone walked the wire 2-3 us/s.
  /// A correction computed from a SHARED value is the same number on both boards, so it injects
  /// no differential at all, whatever its gain.
  ///
  /// Verified before it was built (tests/group, "is e_common actually COMMON?"): across every
  /// scenario the two boards' e_common agree to 1-4% with r up to 1.00, and disagree by a flat
  /// ~80 us that does not scale with the error -- exactly the per-board measurement noise and
  /// nothing else. Averaging N members cuts that by sqrt(N); making the correction shared removes
  /// its differential component entirely, which is the larger effect at N=2.
  bool common_valid = false;
  int64_t common_us = 0;     ///< consensus e_common; meaningless unless common_valid
  uint8_t common_n = 0;      ///< members contributing to it, self included

  /// THIS BOARD IS STEPPING ITS OWN AUDIO. delta_us is measured as (peer phase - my phase), so
  /// while my own audio is being moved the difference carries MY correction, not the group's
  /// disagreement. The board already knows this -- it stops beaconing its phase for exactly this
  /// reason -- and then feeds the same phase to its own filter anyway.
  ///
  /// Measured 2026-09-03: board a applied frames=+1136 (25.76 ms) and its group delta read
  /// +25005 us on the next sample, 97% of its own step, with a pairing gap of 71 us and
  /// extrap 0.00 -- neither staleness nor extrapolation. Every such sample carried steady=0 and
  /// every clean one steady=1, so the discriminator already exists and is already logged.
  bool self_transient = false;

  /// THIS BOARD HAS PEERS, whether or not a delta is available right now. `present` conflates
  /// "nobody to be differential from" with "somebody, but no usable delta this instant", and those
  /// require opposite handling -- see Profile::position_needs_diff.
  bool has_peers = false;
};

/// Why the engine acted. One record per decision; the log line writes this and nothing else.
struct Decision {
  enum class Act : uint8_t { None, Rate, Position, Hold } act = Act::None;
  enum class Why : uint8_t {
    Idle, NoEvidence, InFlight, WithinFrame, CoarseError, RateOnly, Common
  } why = Why::Idle;
  int64_t error_us = 0;
  int32_t frames = 0;
  float rate_ppm = 0.0f;
  float crystal_ppm = 0.0f;
  uint32_t suppressed = 0;   ///< decisions folded into this one, so the census stays complete

  // Why the step was UNAVOIDABLE, or wasn't. "why = CoarseError" only restates that the error
  // crossed the gate; it says nothing about whether rate could have answered it instead, which
  // is the only question worth asking about a step. Recorded on every decision so a step can be
  // audited after the fact rather than reconstructed from drift rates.
  int64_t filtered_us = 0;    ///< the error the gate actually tested
  int64_t gate_us = 0;        ///< the threshold it was tested against
  float needed_ppm = 0.0f;    ///< rate that would remove filtered_us within one horizon
  float authority_ppm = 0.0f; ///< rate the loop was allowed to command

  /// A CONFIRMED JUMP SNAPPED A FILTER, and until now nothing could see it happen.
  ///
  /// On confirmation the filter is assigned outright (`gd_mean_us_ = gx`) rather than stepped
  /// toward the new value -- deliberately, since a real re-anchor should not be averaged in. But
  /// that assignment is a DISCONTINUITY in the signal P is computed from, so P moves by
  /// Kp * (snap size) on the next command: at Kp ~ 0.45 ppm/us a 45 us snap is ~20 ppm.
  ///
  /// The suspicion this exists to test: gd refreshes at ~1 Hz and JUMP_CONFIRM_SAMPLES is 3, so a
  /// ~3 s excursion is "confirmed" as a jump. The filter snaps out to it, P follows, gd returns,
  /// and the return is confirmed as a jump too -- so P follows back. Out and back is a full cycle
  /// in d(rate)/dt with no net change, which is the shape seen on the wire at >20 ppm, on one
  /// board at a time. Unverified: this is the instrument, not the finding.
  int32_t gd_snap_us = 0;     ///< signed size of a gd filter snap this decision, 0 if none
  int32_t err_snap_us = 0;    ///< the same for the deadline-error filter

  /// THE INTEGRAL'S INPUT, SPLIT INTO ITS TWO PARTS. e_common is what this board shares with the
  /// group (its own clock against the server); e_diff is what separates it from its peers. Only
  /// the second is audible, but only the first can wind the crystal without anyone hearing it.
  ///
  /// Both were computed on one line inside the integral and reported NOWHERE, so a wind-up episode
  /// showed its result and never its cause. The simulator says the common part is responsible --
  /// crystals reaching +156/+183 ppm against targets of +25/+55 while the DIFFERENTIAL stayed
  /// correct at +27 against +30, so the pair sounded synchronised the whole way to the rail. This
  /// is what makes that checkable on hardware instead of inferred.
  /// VALID ONLY WHEN THE INTEGRAL RAN. These are written inside the integral block, which is
  /// skipped when the decision is not differential -- so a plain 0 here means "not computed this
  /// decision", not "the error was zero". Reporting the two as the same value is the defect this
  /// codebase keeps finding elsewhere; the flag is here so this instrument does not repeat it.
  bool e_split_valid = false;
  int32_t e_common_us = 0;
  int32_t e_diff_us = 0;
  /// What actually reached the integrator this decision, after both clamps. The gap between this
  /// and e_common + e_diff is the clamp doing its work, and is worth seeing.
  int32_t e_bounded_us = 0;

  /// WHY THE RATE MOVED, decomposed at the moment it moved. The command is crystal + P, then
  /// slew-limited, so any change is exactly d_crystal + d_p with the slew possibly clipping it.
  /// Correlating rate excursions against events AFTER THE FACT has now failed twice -- once
  /// against gd steps and once against filter snaps -- because the bench fires candidate events
  /// every couple of seconds and any excursion has one nearby by construction. Attribution at the
  /// point of decision does not have that problem: the loop knows which term it just moved.
  float d_rate_ppm = 0.0f;      ///< change in the issued command since the last decision
  float d_crystal_ppm = 0.0f;   ///< how much of that was the integral
  float d_p_ppm = 0.0f;         ///< how much was the proportional term
  float p_ppm = 0.0f;           ///< P itself, so a standing P is distinguishable from a moving one
  float kp_ppm_per_us = 0.0f;   ///< the gain in force, which sigma_e moves under you
  /// The noise estimate Kp was divided by -- the DIFFERENTIAL's when kp_from_diff_sigma is on and
  /// the group supplies one, the deadline error's otherwise. Logged because "which distribution
  /// was the gain sized from" is exactly the question that went unasked.
  float p_sigma_us = 0.0f;
  /// The DIFFERENTIAL's own noise estimate, formed the same way sigma_e is (0.8862 * EWMA of
  /// consecutive differences) and floored at a quarter frame by the gate that owns it.
  ///
  /// Logged because the ratio sigma(gd)/sigma_e decides whether sizing Kp from the differential
  /// raises or lowers the gain, and that ratio is INVERTED between this bench and the simulator --
  /// so the sign of the fix depends on it. It cannot be recovered from the existing lines: ENGINE
  /// is throttled to one line per 2 s and GDIN to one per second, and differencing at those
  /// cadences measures the loop's slow excursion rather than sample-to-sample noise, which is the
  /// distinction the noise estimator was deliberately built around.
  float gd_sigma_us = 0.0f;
  /// Whether the stability cap bound this decision, i.e. the noise budget asked for a gain past
  /// the delay limit. If this is set most of the time, the budget is not what is setting Kp.
  bool kp_capped = false;

  /// WHICH SIGNAL e_position TOOK, and its value. e_position = have_diff ? e_diff : e_filtered,
  /// and every actuator decision -- the coarse gate, rate_can_fix, the frame count -- is computed
  /// from it. Nothing logged it, so which quantity drove a correction had to be INFERRED, and that
  /// inference was wrong three times on 2026-09-03: once concluding the position path lacked
  /// confirmation (it has the filter), once concluding a sustained deadline glitch drove it (the
  /// glitch cannot reach position while a delta exists), and once unable to reconcile GDIN gd of
  /// +-20 us with corrections of 124-1038 frames while CMNC reported a valid split.
  ///
  /// One field ends that: esrc says diff or deadline, ep says how much.
  bool e_from_diff = false;
  int64_t e_position_us = 0;
  bool slew_clipped = false;    ///< the command wanted to move further than authority*dt/horizon

  /// THE SHARED COMMON-MODE TERM, and the value it acted on. Recorded even when the correction is
  /// disabled (common_drain_s = 0), so the consensus can be SHADOWED against the live loop -- logged
  /// beside what the board actually did, acting on nothing -- before any gain is applied. Every
  /// mechanism proposed on this bench that skipped that step was withdrawn.
  bool common_shared_valid = false;  ///< false = the group supplied no consensus; NOT "it was zero"
  int32_t common_shared_us = 0;      ///< the consensus value; meaningless unless the flag is set
  uint8_t common_n = 0;              ///< how many members it came from, self included
  float pc_ppm = 0.0f;               ///< what the shared correction contributed to the command
};

/// The two actuators. Separate fields: doing position work through the rate field requires
/// writing the wrong one.
struct Command {
  float rate_ppm = 0.0f;      ///< continuous; owns sub-frame position
  int32_t frames = 0;         ///< one-shot, whole frames, 0 = nothing
  uint64_t correction_id = 0; ///< echoed by confirm_position_landed()
  Decision decision{};
};

class Engine {
 public:
  explicit Engine(const Profile &p) : profile_(p) {}

  void set_profile(const Profile &p) { profile_ = p; }
  const Profile &profile() const { return profile_; }

  /// One decision. Uses now_us for all time bases; assumes no fixed cadence.
  Command step(int64_t now_us, const Observation &obs, const GroupEvidence &group);

  /// Called when a frame correction has provably landed. No further correction is issued until
  /// then: two in flight cannot be distinguished from one that failed.
  void confirm_position_landed(uint64_t correction_id, int64_t now_us);

  /// SOMEONE ELSE MOVED THE AUDIO. The hard-resync path drops chunks and inserts silence on its
  /// own authority, and until this existed it did so without telling the engine -- so the engine's
  /// position model was wrong by however far the resync went, and it read the jump as a plant
  /// movement to be chased. Two controllers on one actuator, one of them silent.
  ///
  /// Measured in tests/group with the fault modelled: 19 us of skew and zero corrections became
  /// 63 ms and 521 corrections. Missing HALF the observations, by comparison, cost 1 us. This was
  /// the dominant reason the simulator disagreed with the bench.
  ///
  /// Treated exactly like a correction the engine issued -- filter shifted, compensation window
  /// armed, position serialised -- except that it is NOT credited to the crystal: someone else's
  /// displacement says nothing about this board's rate.
  void note_external_move(int64_t applied_us, int64_t now_us);


  /// Learned per-board plant rate offset, ppm.
  float crystal_ppm() const { return crystal_ppm_; }

  /// Seed it from NVS at boot. The offset is a property of the hardware, not the session, so
  /// relearning it every boot costs the whole wind-up transient for nothing.
  void set_crystal_ppm(float ppm);

  /// Current estimate of the error signal's own noise (us, 1-sigma). The proportional gain is
  /// budget/sigma_e, so a noisier measurement earns less gain.
  float sigma_e_us() const;

  /// Lag the error filter adds for a given measurement lag. Free function form, for callers that
  /// need the horizon arithmetic before they have a Profile.
  static int64_t filter_lag_for(int64_t measurement_lag_us);

  void reset();

 private:
  float proportional_gain_ppm_per_us() const;
  void credit_position_correction(int32_t frames, int64_t now_us);

  Profile profile_{};

  float crystal_ppm_ = 0.0f;
  int64_t crystal_at_us_ = 0;

  // Online noise estimate of e. Gain follows measured resolution: assuming a value and then
  // claiming a budget does not respect the budget.
  float err_mean_us_ = 0.0f;
  /// Jump detection: a spike and a step are indistinguishable in one sample and differ in
  /// persistence. Counting consecutive same-sign over-limit innovations separates them.
  int jump_run_ = 0;
  int jump_dir_ = 0;
  int gd_jump_run_ = 0;
  int gd_jump_dir_ = 0;
  /// Last sigma of the differential, for the jump test: the current one is not known until after
  /// the filter has been updated with this sample.
  float gd_sigma_prev_us_ = 0.0f;
  float err_diff_us_ = 0.0f;   ///< EWMA of |e_k - e_(k-1)|: noise, immune to slow drift
  float err_last_us_ = 0.0f;

  // The DIFFERENTIAL error gets its own filter and its own noise estimate. Position acts on this
  // signal, not on the deadline error, so smoothing only the latter left the position path acting
  // on a raw one: gd is 8-42 us of noise on this bench before any transient, against a 47 us gate,
  // so single spikes bought whole-frame steps.
  float gd_mean_us_ = 0.0f;
  float gd_diff_us_ = 0.0f;
  float gd_last_us_ = 0.0f;
  bool gd_seeded_ = false;
  int64_t gd_last_at_us_ = 0;
  bool err_seeded_ = false;

  // Net position movement, for the rate credit. A single correction is not a rate measurement;
  // the net over a long baseline is.
  int32_t credit_frames_ = 0;
  int64_t credit_since_us_ = 0;
  uint32_t credit_count_ = 0;

  /// A correction has been applied but the measurement has not caught up. THREE different times
  /// are involved and conflating any two of them has produced a distinct bench failure:
  ///   * in_flight_          -- the audio has moved (one pipeline depth, ~250 ms)
  ///   * pending_comp_until_ -- a RAW observation includes the move (ring + pipe, ~1971 ms).
  ///                            Observations before this are shifted by pending_disp_us_.
  ///   * serialise_until_    -- the FILTERED error includes it (+ filter lag, ~3971 ms). No
  ///                            second correction until then.
  int64_t pending_disp_us_ = 0;
  int64_t pending_comp_until_us_ = 0;
  /// The filtered error recorded when the correction was issued: the value the measurement keeps
  /// reporting until the move lands, and the reference the landing test compares against.
  int64_t pending_ref_us_ = 0;
  int64_t serialise_until_us_ = 0;
  bool in_flight_ = false;
  uint64_t next_id_ = 1;
  uint64_t in_flight_id_ = 0;
  int32_t in_flight_frames_ = 0;
  int64_t in_flight_since_us_ = 0;

  /// Last commanded rate and whether one has been issued: a continuous actuator's command must
  /// not step, so each new command is slew-limited from the previous one.
  float last_rate_cmd_ = 0.0f;
  float last_crystal_ppm_ = 0.0f;
  float last_p_ppm_ = 0.0f;
  bool rate_cmd_seeded_ = false;
  int64_t last_obs_us_ = 0;
  uint32_t suppressed_ = 0;
};

}  // namespace esphome::snapclient::timing
