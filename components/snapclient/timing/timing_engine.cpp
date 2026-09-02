#include "timing_engine.h"

#include <algorithm>
#include <cmath>

namespace snapclient {
namespace timing {

namespace {
/// Unconfirmed corrections expire after this many visibility horizons. Expressed in horizons
/// because the horizon is the only thing that says how long landing takes.
constexpr int MAX_IN_FLIGHT_HORIZONS = 3;
/// Baseline for the rate credit, in horizons. One correction says nothing about rate; the net
/// over a long baseline does, because noise-driven corrections have mixed signs and cancel.
constexpr int CREDIT_BASELINE_HORIZONS = 10;
/// A crystal moves with temperature over minutes, so learn it slowly.
constexpr float CRYSTAL_TAU_S = 300.0f;
/// A crystal is tens of ppm; hundreds means the estimate is tracking something else.
constexpr float CRYSTAL_LIMIT_PPM = 200.0f;
/// Error-filter weight. Dimensionless, so it carries no rate or codec dependence.
constexpr float ERR_ALPHA = 0.02f;
}  // namespace

void Engine::reset() {
  crystal_ppm_ = 0.0f;
  crystal_at_us_ = 0;
  credit_frames_ = 0;
  credit_since_us_ = 0;
  err_seeded_ = false;
  in_flight_ = false;
  in_flight_id_ = 0;
  in_flight_frames_ = 0;
  in_flight_since_us_ = 0;
  last_obs_us_ = 0;
  suppressed_ = 0;
}

float Engine::sigma_e_us() const {
  // 1.25 * MAD approximates sigma for a Gaussian. Floored at a quarter frame so a briefly quiet
  // measurement cannot produce unbounded gain.
  const float floor_us = 0.25f * static_cast<float>(profile_.frame_us());
  return std::max(1.25f * err_mad_us_, floor_us);
}

float Engine::proportional_gain_ppm_per_us() const {
  // Kp * sigma_e is the command noise and it integrates into position over the horizon, so
  // Kp = budget / sigma_e. sigma_e is measured, not assumed: with it assumed at one frame and
  // the real noise 80 us, the command ran 129 ppm against a 20 ppm budget.
  const float s = sigma_e_us();
  if (s <= 0.0f) return 0.0f;
  return profile_.rate_noise_budget_ppm() / s;
}

void Engine::credit_position_correction(int32_t frames, int64_t now_us) {
  // NET frames over a long baseline is a rate measurement; one correction is not. Crediting each
  // correction individually turns a noise-driven +-3 frame twitch into tens of ppm of spurious
  // rate, which then integrates into position -- the same amplification this design exists to
  // avoid. Mixed-sign noise cancels in the net; a real offset accumulates.
  if (credit_since_us_ == 0) credit_since_us_ = now_us;
  credit_frames_ += frames;

  const int64_t baseline_us =
      static_cast<int64_t>(CREDIT_BASELINE_HORIZONS) * profile_.visibility_us;
  const int64_t dt_us = now_us - credit_since_us_;
  if (dt_us < baseline_us) return;

  if (credit_frames_ != 0) {
    const float moved_us =
        static_cast<float>(credit_frames_) * static_cast<float>(profile_.frame_us());
    const float implied_ppm = 1e6f * moved_us / static_cast<float>(dt_us);
    // Dropped frames (positive) mean we were late, which needs a positive trim: same sign.
    crystal_ppm_ = std::clamp(crystal_ppm_ + implied_ppm, -CRYSTAL_LIMIT_PPM, CRYSTAL_LIMIT_PPM);
  }
  credit_frames_ = 0;
  credit_since_us_ = now_us;
}

void Engine::confirm_position_landed(uint64_t correction_id, int64_t now_us) {
  if (!in_flight_ || correction_id != in_flight_id_) return;
  // The plant has moved, so the filtered error must move with it. Without this the engine keeps
  // deciding from an error it has already corrected and re-issues the same correction as the
  // filter slowly catches up -- two accountings of one displacement.
  err_mean_us_ -= static_cast<float>(in_flight_frames_) * static_cast<float>(profile_.frame_us());
  credit_position_correction(in_flight_frames_, now_us);
  in_flight_ = false;
  in_flight_id_ = 0;
  in_flight_frames_ = 0;
}

Command Engine::step(int64_t now_us, const Observation &obs, const GroupEvidence &group) {
  Command cmd{};
  cmd.decision.crystal_ppm = crystal_ppm_;
  cmd.decision.suppressed = suppressed_;

  // No measurement: hold. Reverting would inject a step the size of what was learned.
  if (!obs.valid) {
    cmd.rate_ppm = crystal_ppm_;
    cmd.decision.act = Decision::Act::Hold;
    cmd.decision.why = Decision::Why::NoEvidence;
    suppressed_++;
    return cmd;
  }

  const int64_t e = obs.error_us;
  cmd.decision.error_us = e;

  // Track the error's own noise, on the visibility horizon: that is the timescale the gain is
  // priced over.
  {
    const float x = static_cast<float>(e);
    if (!err_seeded_) {
      err_mean_us_ = x;
      // Seed pessimistic (one frame). Seeding at 0 makes the gain maximal exactly when nothing
      // is known about the signal's resolution.
      err_mad_us_ = static_cast<float>(profile_.frame_us());
      err_seeded_ = true;
    } else {
      err_mean_us_ += ERR_ALPHA * (x - err_mean_us_);
      err_mad_us_ += ERR_ALPHA * (std::fabs(x - err_mean_us_) - err_mad_us_);
    }
  }

  // Expire an unconfirmed correction, or the position path freezes if a landing is never reported.
  if (in_flight_ && now_us - in_flight_since_us_ >
                        static_cast<int64_t>(MAX_IN_FLIGHT_HORIZONS) * profile_.visibility_us) {
    in_flight_ = false;
    in_flight_frames_ = 0;
  }

  // Group evidence answers one question: is this error mine or the group's? Only a differential
  // error is audible, and correcting a common one moves the pair apart. Not a gain input.
  bool differential = true;
  if (group.present && group.contributors > 1) {
    const float unhalve =
        static_cast<float>(group.contributors) / static_cast<float>(group.contributors - 1);
    const int64_t mine = static_cast<int64_t>(
        std::llround(std::fabs(static_cast<float>(group.delta_us)) * unhalve));
    differential = mine >= profile_.target_position_us;
  }

  // Coarse: whole frames, one at a time, verified. Decided on the FILTERED error, not the latest
  // sample: dropping frames is irreversible, and at 80 us of measurement noise the instantaneous
  // error crosses a 22 us frame boundary constantly while the audio has not moved at all.
  const int64_t frame_us = profile_.frame_us();
  const int64_t e_filtered = static_cast<int64_t>(std::llround(err_mean_us_));
  // The filter has its own uncertainty: for an EMA, sd_out ~ sd_in * sqrt(alpha/2). Require the
  // filtered error to clear one frame by 2 of those before spending an irreversible correction,
  // so noise alone cannot trigger one. No magic margin -- it is derived from the measured noise.
  const float sigma_filtered = sigma_e_us() * std::sqrt(ERR_ALPHA / 2.0f);
  const int64_t coarse_gate_us =
      frame_us + static_cast<int64_t>(std::llround(2.0f * sigma_filtered));
  const int32_t frames = (frame_us > 0 && std::llabs(e_filtered) >= coarse_gate_us)
                             ? static_cast<int32_t>(e_filtered / frame_us)
                             : 0;

  if (frames != 0 && differential) {
    if (in_flight_) {
      cmd.rate_ppm = crystal_ppm_;
      cmd.decision.act = Decision::Act::Hold;
      cmd.decision.why = Decision::Why::InFlight;
      suppressed_++;
      return cmd;
    }
    in_flight_ = true;
    in_flight_id_ = next_id_++;
    in_flight_frames_ = frames;
    in_flight_since_us_ = now_us;
    cmd.frames = frames;
    cmd.correction_id = in_flight_id_;
    // Rate holds the learned offset only; chasing the same error would deliver it twice.
    cmd.rate_ppm = crystal_ppm_;
    cmd.decision.act = Decision::Act::Position;
    cmd.decision.why = Decision::Why::CoarseError;
    cmd.decision.frames = frames;
    cmd.decision.rate_ppm = cmd.rate_ppm;
    suppressed_ = 0;
    last_obs_us_ = now_us;
    return cmd;
  }

  // Fine: below one frame only rate can express the correction.
  if (crystal_at_us_ == 0) crystal_at_us_ = now_us;

  const int64_t dt_us = last_obs_us_ > 0 ? now_us - last_obs_us_ : 0;
  if (dt_us > 0 && differential) {
    const float dt_s = static_cast<float>(dt_us) / 1e6f;
    const float ki = 1.0f / CRYSTAL_TAU_S;
    const float contrib = std::clamp(proportional_gain_ppm_per_us() * static_cast<float>(e),
                                     -profile_.rate_noise_budget_ppm(),
                                     profile_.rate_noise_budget_ppm());
    crystal_ppm_ = std::clamp(crystal_ppm_ + ki * contrib * dt_s,
                              -CRYSTAL_LIMIT_PPM, CRYSTAL_LIMIT_PPM);
  }
  last_obs_us_ = now_us;

  // The P term is clamped to the budget. Deriving Kp from a measured sigma_e is only as good as
  // the estimate; clamping makes "command noise <= budget" hold by construction. Sustained error
  // larger than the budget is the integral's job, not P's.
  const float budget = profile_.rate_noise_budget_ppm();
  const float p_raw =
      differential ? proportional_gain_ppm_per_us() * static_cast<float>(e) : 0.0f;
  const float p_term = std::clamp(p_raw, -budget, budget);
  cmd.rate_ppm = crystal_ppm_ + p_term;
  cmd.decision.act = differential ? Decision::Act::Rate : Decision::Act::Hold;
  cmd.decision.why =
      differential ? (frames == 0 ? Decision::Why::WithinFrame : Decision::Why::RateOnly)
                   : Decision::Why::Common;
  cmd.decision.rate_ppm = cmd.rate_ppm;
  cmd.decision.crystal_ppm = crystal_ppm_;
  suppressed_ = 0;
  return cmd;
}

}  // namespace timing
}  // namespace snapclient
