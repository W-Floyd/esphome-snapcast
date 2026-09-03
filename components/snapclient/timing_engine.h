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
  /// Position accuracy aimed at; sets the rate-command noise budget.
  int64_t target_position_us = 20;
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
    return 1e6f * static_cast<float>(target_position_us) / static_cast<float>(h);
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
  bool slew_clipped = false;    ///< the command wanted to move further than authority*dt/horizon
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
