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
//     sigma_rate <= sigma_position / visibility_us
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
struct Profile {
  uint32_t frame_rate_hz = 44100;
  /// Time for a correction to become visible in the measurement (pipeline + ring + blocks).
  int64_t visibility_us = 3000000;
  /// Position accuracy aimed at; sets the rate-command noise budget.
  int64_t target_position_us = 20;

  int64_t frame_us() const { return 1000000 / static_cast<int64_t>(frame_rate_hz); }

  float rate_noise_budget_ppm() const {
    if (visibility_us <= 0) return 0.0f;
    return 1e6f * static_cast<float>(target_position_us) / static_cast<float>(visibility_us);
  }
};

/// One measured render event.
struct Observation {
  int64_t at_us = 0;
  int64_t error_us = 0;   ///< e
  bool valid = false;     ///< false: no measurement, engine holds
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


  /// Learned per-board plant rate offset, ppm.
  float crystal_ppm() const { return crystal_ppm_; }

  /// Seed it from NVS at boot. The offset is a property of the hardware, not the session, so
  /// relearning it every boot costs the whole wind-up transient for nothing.
  void set_crystal_ppm(float ppm);

  /// Current estimate of the error signal's own noise (us, 1-sigma). The proportional gain is
  /// budget/sigma_e, so a noisier measurement earns less gain.
  float sigma_e_us() const;

  /// Lag the error filter adds, us. The caller's visibility horizon is the pipeline it can
  /// measure plus this: a correction is not visible until the filter has caught up with it.
  static int64_t filter_lag_us();

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
  float err_mad_us_ = 0.0f;
  bool err_seeded_ = false;

  // Net position movement, for the rate credit. A single correction is not a rate measurement;
  // the net over a long baseline is.
  int32_t credit_frames_ = 0;
  int64_t credit_since_us_ = 0;
  uint32_t credit_count_ = 0;

  /// A correction has been applied but is not yet visible in the measurement. Observations before
  /// pending_visible_at_us_ are in pre-correction coordinates and are shifted by pending_disp_us_
  /// rather than discarded. Distinct from in_flight_: that ends when the audio moves (a pipeline
  /// depth), these when the move becomes observable (a visibility horizon), an order of magnitude
  /// apart.
  int64_t pending_disp_us_ = 0;
  int64_t pending_visible_at_us_ = 0;
  bool in_flight_ = false;
  uint64_t next_id_ = 1;
  uint64_t in_flight_id_ = 0;
  int32_t in_flight_frames_ = 0;
  int64_t in_flight_since_us_ = 0;

  int64_t last_obs_us_ = 0;
  uint32_t suppressed_ = 0;
};

}  // namespace esphome::snapclient::timing
