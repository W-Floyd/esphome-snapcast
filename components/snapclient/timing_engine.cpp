#include "timing_engine.h"

#include <algorithm>
#include <cmath>

namespace esphome::snapclient::timing {

namespace {
/// Unconfirmed corrections expire after this many visibility horizons. Expressed in horizons
/// because the horizon is the only thing that says how long landing takes.
constexpr int MAX_IN_FLIGHT_HORIZONS = 3;
/// Baseline for the rate credit, in horizons. One correction says nothing about rate; the net
/// over a long baseline does, because noise-driven corrections have mixed signs and cancel.
constexpr int CREDIT_BASELINE_HORIZONS = 10;
/// A rate offset shows up as a STREAM of same-signed corrections; a one-off displacement shows
/// up as a single large one. Requiring several before crediting is what separates them.
constexpr uint32_t CREDIT_MIN_CORRECTIONS = 3;
/// And cap what one window may contribute. Measured 2026-09-02: a 1.5 ms displacement corrected
/// in one 69-frame step implied 156 ppm of "rate", which wound the estimate to its clamp. A
/// sustained offset still converges, over several windows.
constexpr float CREDIT_MAX_PPM_PER_WINDOW = 5.0f;
/// Phase margin for the crystal integral, in visibility horizons: wn = 1/(this * visibility).
/// K=3 measured at median |error| 7-15 us with <=14 ppm overshoot at horizons up to 3 s; K=1.5
/// overshot 80 ppm at a 1 s horizon and K=5 was needlessly slow. Not a time constant -- the
/// horizon supplies the timescale, so this stays dimensionless and no constant here is tied to
/// rate, chunk size or sample depth.
constexpr float CRYSTAL_DELAY_MARGIN = 3.0f;
/// A crystal is tens of ppm; hundreds means the estimate is tracking something else.
constexpr float CRYSTAL_LIMIT_PPM = 200.0f;
/// NOTE: authority now arrives as Profile::rate_authority_ppm, from the transport that owns the
/// actuator. The text below is kept because it is the measurement that motivated separating
/// authority from the noise budget at all.
///
/// How much rate the loop may command on top of the crystal. This is AUTHORITY, not noise: the
/// noise guarantee comes from Kp = budget/sigma_e, which bounds the injected noise to the budget
/// whatever this is set to. Clamping the P OUTPUT to the budget as well capped authority at one
/// sigma -- with sigma floored at a frame, P saturated at |e| ~ 23 us while the coarse gate sat at
/// 23-30 us, so rate ran out of authority at exactly the error where stepping began, and position
/// paid for every plant wander. Measured: P contributed 0.4 ppm median against a plant wandering
/// +-30 ppm, and corrections fired every 4.0 s (the whole serialisation window) at 8-9 frames.
/// The value belongs to the hardware: it is what the trim actuator accepts, which the transport
/// knows and this file cannot.
/// Error-filter time constant, in TRANSPORT DELAYS rather than seconds. Still a time and not a
/// weight -- a fixed weight makes the filter's timescale depend on the observation cadence -- but
/// the timescale now comes from the plant instead of from this file. Averaging for longer than
/// the plant's own delay adds lag without adding information, and averaging for much less passes
/// noise the loop could not act on before the delay expires anyway, so one delay is the natural
/// scale. A hardcoded 2.0 s happened to be right for a 1724 ms ring; it would have been wrong for
/// any other buffer size, sample rate or network condition, and nothing would have said so.
constexpr float ERR_TAU_HORIZONS = 1.0f;
}  // namespace

void Engine::set_crystal_ppm(float ppm) {
  if (!std::isfinite(ppm)) return;
  crystal_ppm_ = std::clamp(ppm, -CRYSTAL_LIMIT_PPM, CRYSTAL_LIMIT_PPM);
}

void Engine::reset() {
  crystal_ppm_ = 0.0f;
  crystal_at_us_ = 0;
  credit_frames_ = 0;
  credit_count_ = 0;
  credit_since_us_ = 0;
  err_seeded_ = false;
  in_flight_ = false;
  in_flight_id_ = 0;
  in_flight_frames_ = 0;
  in_flight_since_us_ = 0;
  pending_disp_us_ = 0;
  pending_comp_until_us_ = 0;
  pending_ref_us_ = 0;
  serialise_until_us_ = 0;
  last_obs_us_ = 0;
  suppressed_ = 0;
}

int64_t Engine::filter_lag_for(int64_t measurement_lag_us) {
  return static_cast<int64_t>(ERR_TAU_HORIZONS * static_cast<float>(measurement_lag_us));
}

int64_t Profile::filter_lag_us() const { return Engine::filter_lag_for(compensation_us()); }

float Engine::sigma_e_us() const {
  // Floored at ONE FRAME, which is what caps Kp at budget/frame_us -- the gain at which the P
  // term stays LINEAR across the whole range position cannot cover. Below that floor Kp is larger
  // and the clamp to +-budget turns P into a relay for any error past sigma_e: with the floor at
  // a quarter frame, Kp was 4x the cap, P saturated beyond 5.5 us, and the loop bang-banged
  // against the transport delay at +-10-20 us on a 20-40 s period with ZERO frame corrections --
  // a pure rate-loop limit cycle. Measured: at a 0.25-frame floor the crystal swung 2.1-4.6 ppm
  // for ever against a 3.35 ppm plant; at 0.5 frame it still swung; at one frame it converged to
  // 3.347 ppm with the error inside 0.4 us. The floor only binds when the measurement is quiet --
  // at the bench's ~80 us of noise sigma_e is measured and this never applies.
  const float floor_us = static_cast<float>(profile_.frame_us());
  // E|dx| = 2 sigma / sqrt(pi) for Gaussian white noise, so sigma = E|dx| * sqrt(pi)/2.
  return std::max(0.8862f * err_diff_us_, floor_us);
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
  credit_count_++;

  const int64_t baseline_us =
      static_cast<int64_t>(CREDIT_BASELINE_HORIZONS) * profile_.settle_us();
  const int64_t dt_us = now_us - credit_since_us_;
  if (dt_us < baseline_us) return;

  // A single correction in the window is a displacement, not a rate: crediting it is how a
  // one-off 1.5 ms catch-up became 156 ppm and wound the estimate to its clamp.
  if (credit_frames_ != 0 && credit_count_ >= CREDIT_MIN_CORRECTIONS) {
    const float moved_us =
        static_cast<float>(credit_frames_) * static_cast<float>(profile_.frame_us());
    const float implied_ppm = 1e6f * moved_us / static_cast<float>(dt_us);
    // Dropped frames (positive) mean we were late, which needs a positive trim: same sign.
    const float step = std::clamp(implied_ppm, -CREDIT_MAX_PPM_PER_WINDOW, CREDIT_MAX_PPM_PER_WINDOW);
    crystal_ppm_ = std::clamp(crystal_ppm_ + step, -CRYSTAL_LIMIT_PPM, CRYSTAL_LIMIT_PPM);
  }
  credit_frames_ = 0;
  credit_count_ = 0;
  credit_since_us_ = now_us;
}

void Engine::confirm_position_landed(uint64_t correction_id, int64_t now_us) {
  if (!in_flight_ || correction_id != in_flight_id_) return;
  // No feed-forward into err_mean_us_ here. step() compensates every observation by the pending
  // displacement until it becomes visible, so the filter is already in post-correction
  // coordinates; stepping it here as well would count one displacement twice. This is the audio
  // having moved, which is what credit is owed against -- not the move becoming observable.
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
    cmd.decision.rate_ppm = cmd.rate_ppm;   // else the log reads 0 while the command holds
    suppressed_++;
    return cmd;
  }

  // A correction has been delivered, but the measurement cannot show it until the visibility
  // horizon passes, so until then every observation describes the position we have already left.
  // Those samples are not useless: they are wrong by an amount we know exactly, because we
  // applied it. Shift them into post-correction coordinates instead of discarding them.
  //
  // apply_frames() moves the plant by -frames * frame_us, so an observation that predates
  // visibility reads high by that much.
  //
  // Discarding them instead -- holding blind for the whole horizon -- was measured on the bench as
  // a 9x REGRESSION in absolute skew (median |offset| 25 us -> 224 us, p90 211 us -> 696 us).
  // Stairstepping fell from 25% of captures to 3.6%, but only because the loop stopped
  // correcting: the hold suppressed the rate path too, so for up to 5 s after every correction
  // the board coasted on a crystal estimate that could be tens of ppm wrong. The measurement
  // being stale is a reason to correct it, not a reason to stop steering.
  // Compensation runs for the TRANSPORT delay only. Past that the raw observation already
  // includes the move, and subtracting it again invents an error of exactly -displacement --
  // measured on the bench as err +444 us -> a 19-frame (431 us) correction -> err -344 us, an
  // equal-and-opposite pair, repeating as a multi-frame ladder up to 12 frames.
  // Stop compensating on EVIDENCE, not on a computed deadline. The deadline is
  // position_delay + measurement_lag, and position_delay comes from buffer accounting that has
  // been wrong twice: ring fill (available(), 1724 ms) and pushed-minus-played (247 ms) disagree
  // 7x for what should be nearly the same quantity, because the latter is rebaselined on feedback
  // gaps -- and the wire says the real landing is ~1.25 s. Compensating past the landing subtracts
  // a displacement the observation already contains, which invents an error of -D and steps a
  // SYNCED board away from sync and back. Observed on the wire as exactly that, and in the log as
  // a 590 us correction followed by a 600 us error, every settle window.
  //
  // The landing is detectable: before it, a raw observation sits near the error we recorded at
  // issue; after it, near that error minus the displacement. Whichever it is closer to says which
  // side of the landing we are on, and for the corrections that matter (26 frames = 590 us against
  // ~80 us of noise) that is unambiguous. The computed deadline stays as an upper bound only.
  if (pending_comp_until_us_ != 0) {
    const int64_t as_not_landed = std::llabs(obs.error_us - pending_ref_us_);
    const int64_t as_landed = std::llabs(obs.error_us - (pending_ref_us_ - pending_disp_us_));
    if (as_landed < as_not_landed || now_us >= pending_comp_until_us_) {
      pending_comp_until_us_ = 0;
      pending_disp_us_ = 0;
      pending_ref_us_ = 0;
    }
  }
  // STARVED: hold everything. When the buffer is below one measurement lag there is not enough
  // audio queued for the render instant to be deadline-driven at all -- the board is playing
  // whatever it has and falling behind, so the growing "error" is a data shortage reported in
  // microseconds. Correcting it with position removes yet more audio, which is the spiral measured
  // on the bench: ring 1724 ms -> 26 ms, then err climbing 3 ms per report while the loop dropped
  // 5584 frames a time. Rate holds the learned offset; nothing is integrated, because the error is
  // not telling us about the clock.
  if (obs.buffer_us > 0 && obs.buffer_us < profile_.measurement_lag_us) {
    cmd.rate_ppm = crystal_ppm_;
    cmd.decision.act = Decision::Act::Hold;
    cmd.decision.why = Decision::Why::NoEvidence;
    cmd.decision.rate_ppm = cmd.rate_ppm;
    suppressed_++;
    return cmd;
  }

  const int64_t e = obs.error_us - pending_disp_us_;
  cmd.decision.error_us = e;

  // Track the error and its own noise. alpha comes from the elapsed time, so the filter has the
  // same timescale whether observations arrive per chunk or per tag arrival.
  {
    const float x = static_cast<float>(e);
    if (!err_seeded_) {
      err_mean_us_ = x;
      err_last_us_ = x;
      // Seed pessimistic (one frame): seeding at 0 makes the gain maximal exactly when nothing
      // is known about the signal's resolution.
      err_diff_us_ = static_cast<float>(profile_.frame_us());
      err_seeded_ = true;
    } else {
      const int64_t dt = last_obs_us_ > 0 ? now_us - last_obs_us_ : 0;
      const float dt_s = dt > 0 ? static_cast<float>(dt) / 1e6f : 0.0f;
      const float tau_s = std::max(1e-3f, static_cast<float>(profile_.filter_lag_us()) / 1e6f);
      const float alpha = dt_s > 0.0f ? 1.0f - std::exp(-dt_s / tau_s) : 0.0f;
      err_mean_us_ += alpha * (x - err_mean_us_);
      // Noise from CONSECUTIVE DIFFERENCES, not from the spread about the mean. |x - mean|
      // counts the loop's own slow excursion as measurement noise, and since Kp = budget/sigma_e
      // that feeds back: a wider excursion lowers the gain, which widens the excursion. Measured
      // as a ~1200 s oscillation, crystal swinging 2.4-4.3 ppm with sigma_e swinging 9.8-12.0 in
      // phase with it, once the integral was fast enough for the amplitude to matter.
      // Differencing high-passes the signal, so drift cancels and only sample-to-sample noise is
      // left. E|dx| = 2 sigma / sqrt(pi) for Gaussian white noise, hence the 1.128.
      err_diff_us_ += alpha * (std::fabs(x - err_last_us_) - err_diff_us_);
      err_last_us_ = x;
    }
  }

  // Expire an unconfirmed correction, or the position path freezes if a landing is never reported.
  if (in_flight_ && now_us - in_flight_since_us_ >
                        static_cast<int64_t>(MAX_IN_FLIGHT_HORIZONS) * profile_.settle_us()) {
    in_flight_ = false;
    in_flight_frames_ = 0;
  }

  // Group evidence answers one question: is this error mine or the group's? Only a differential
  // error is audible, and correcting a common one moves the pair apart. Not a gain input.
  // How much of MY error is differential -- not merely whether the group delta is small.
  //
  // The old test was `|gd| * unhalve >= target_position_us`, which asks the wrong question. On the
  // bench gd reads 8-42 us between boards that agree with each other to within tens of us while
  // each sits ~600 us off its own deadline: nearly all of that error is COMMON, yet a gd of 21 us
  // crosses a 20 us threshold, the whole 600 us is declared differential, and position steps it.
  // Both boards do so at different moments, so the pair is pulled apart -- the wire showed a
  // synced board stepped AWAY from zero and back every settle window, 64 frames spent in the
  // two-board simulator on an error that was never audible.
  //
  // Only a differential error is audible, so position's error signal IS the differential one when
  // the group can supply it. Tracking the deadline is left to rate and the crystal, which is where
  // it belongs.
  // NO unhalve. group.delta_us is this board's deviation from the group MEAN, and every board in
  // the group is correcting its own at the same time: if each moves by its deviation, they meet.
  // The n/(n-1) factor converts "deviation from the mean" into "error against the others,
  // excluding me", which is the right quantity only for a SOLE corrector. Applied while everyone
  // corrects, each board traverses the whole gap instead of half of it, the pair swaps places, and
  // the consensus oscillates -- 330 corrections and 269 us of skew in the two-board simulator on a
  // purely common drift, against 8 corrections for the old gate.
  bool differential = true;
  int64_t e_diff = 0;
  bool have_diff = false;
  if (group.present && group.contributors > 1) {
    e_diff = group.delta_us;
    have_diff = true;
    differential = std::llabs(e_diff) >= profile_.target_position_us;
  }

  // Coarse: whole frames, one at a time, verified. Decided on the FILTERED error, not the latest
  // sample: dropping frames is irreversible, and at 80 us of measurement noise the instantaneous
  // error crosses a 22 us frame boundary constantly while the audio has not moved at all.
  const int64_t frame_us = profile_.frame_us();
  const int64_t e_filtered = static_cast<int64_t>(std::llround(err_mean_us_));
  // The filter has its own uncertainty: for an EMA, sd_out ~ sd_in * sqrt(alpha/2). Require the
  // filtered error to clear one frame by 2 of those before spending an irreversible correction,
  // so noise alone cannot trigger one. No magic margin -- it is derived from the measured noise.
  // The filter's residual noise: sd_out ~ sd_in * sqrt(alpha/2) for an EMA, with alpha the
  // effective per-observation weight implied by the current interval and ERR_TAU_S.
  const float tau_gate_s = std::max(1e-3f, static_cast<float>(profile_.filter_lag_us()) / 1e6f);
  const float obs_dt_s =
      last_obs_us_ > 0 && now_us > last_obs_us_
          ? static_cast<float>(now_us - last_obs_us_) / 1e6f
          : tau_gate_s;
  const float alpha_eff = std::min(1.0f, 1.0f - std::exp(-obs_dt_s / tau_gate_s));
  const float sigma_filtered = sigma_e_us() * std::sqrt(std::max(1e-6f, alpha_eff) / 2.0f);
  const int64_t coarse_gate_us =
      frame_us + static_cast<int64_t>(std::llround(2.0f * sigma_filtered));
  // Rate owns anything it can still reach. needed_ppm is what would remove the filtered error
  // within one horizon; while that is inside the loop's authority, a frame correction is not a
  // last resort but a shortcut -- and an irreversible one, quantised to 22.68 us, against an
  // actuator that is continuous. Position is for what rate CANNOT do.
  // Judged over RATE'S OWN horizon: measure, then filter. Not over the position delay, which
  // carries the ring and would claim rate can absorb an error it will in fact overshoot (at a 10 s
  // horizon a 907 us step needs only 91 ppm, passes as "within reach", and settles 290 us the
  // other side).
  // Position's own error signal: the differential where the group supplies one, else the deadline
  // error (a lone client has nothing to be differential from). Everything below judges position on
  // THIS, including whether rate could have reached it -- judging reach on the deadline error
  // while stepping the differential one would compare two different quantities.
  const int64_t e_position = have_diff ? e_diff : e_filtered;
  const float reach_s = static_cast<float>(profile_.rate_horizon_us()) / 1e6f;
  const float needed_ppm =
      reach_s > 0.0f ? static_cast<float>(e_position) / reach_s : 0.0f;
  const bool rate_can_fix = std::fabs(needed_ppm) <= profile_.rate_authority_ppm;
  int32_t frames = (frame_us > 0 && std::llabs(e_position) >= coarse_gate_us && !rate_can_fix)
                       ? static_cast<int32_t>(e_position / frame_us)
                       : 0;
  // A DROP IS SPENT FROM THE BUFFER. Never spend more than half of it on one correction: the
  // buffer is what makes continuous playout possible, and a correction that empties it trades a
  // timing error for a dropout -- then reports the dropout as a bigger timing error. Half, so
  // that no single correction can ever bring the buffer below the level it needs to deliver that
  // correction. Inserts add audio and need no such bound.
  if (frames > 0 && obs.buffer_us > 0 && frame_us > 0) {
    const int32_t affordable = static_cast<int32_t>((obs.buffer_us / 2) / frame_us);
    if (frames > affordable) frames = affordable;
  }
  cmd.decision.filtered_us = e_position;
  cmd.decision.gate_us = coarse_gate_us;
  cmd.decision.needed_ppm = needed_ppm;
  cmd.decision.authority_ppm = profile_.rate_authority_ppm;

  if (frames != 0 && differential) {
    // Serialise on VISIBILITY -- the filtered error is what this branch decides from, so a second
    // correction must wait for the filter to have seen the first, not merely for the transport to
    // have delivered it. Compensation is the shorter window (see above); these are different
    // times and using one for both is what produced the equal-and-opposite ladder.
    if (in_flight_ || now_us < serialise_until_us_) {
      cmd.rate_ppm = crystal_ppm_;
      cmd.decision.act = Decision::Act::Hold;
      cmd.decision.why = Decision::Why::InFlight;
      cmd.decision.rate_ppm = cmd.rate_ppm;  // else the log reads 0 while the command holds
      suppressed_++;
      return cmd;
    }
    in_flight_ = true;
    in_flight_id_ = next_id_++;
    in_flight_frames_ = frames;
    in_flight_since_us_ = now_us;
    pending_disp_us_ = static_cast<int64_t>(frames) * frame_us;
    pending_comp_until_us_ = now_us + profile_.compensation_us();
    // The error as the measurement will keep reporting it until the move lands.
    pending_ref_us_ = static_cast<int64_t>(std::llround(err_mean_us_));
    serialise_until_us_ = now_us + profile_.settle_us();
    // Compensation is a change of coordinates, so the filter's STATE moves with its inputs, at
    // the same instant. Shifting only the inputs leaves err_mean_us_ holding the pre-correction
    // value and decaying toward the compensated stream over ERR_TAU_S, which crosses the coarse
    // gate again on the way down and buys 2-3 extra corrections (measured in test 4). Shifting
    // both is not the double-count that a feed-forward plus RAW observations would be.
    err_mean_us_ -= static_cast<float>(pending_disp_us_);
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
    // The integral's bandwidth is set by the TRANSPORT DELAY, not by a fixed time constant and
    // not by damping. A correction is invisible for a whole visibility horizon, so wn must sit
    // well below 1/visibility; above that the loop integrates error it has already answered.
    //
    //     wn = 1 / (CRYSTAL_DELAY_MARGIN * visibility),  Ki = wn^2
    //
    // Measured across the whole [1 s, 5 s] horizon range, at 0 and 80 us of noise. Two other
    // laws were tried and both rail the estimate to its clamp at long horizons:
    //   * ki * clamp(Kp e) with a fixed tau -- slew is ki * budget = ki * target/visibility, so
    //     loop speed scales as 1/visibility^2. Unwinding 60 ppm took 45 min at tau = 300 s; fast
    //     enough to be useful at 3 s, it railed at 5 s.
    //   * Ki = (Kp/2)^2, critical damping. Ignores the delay, which is the dominant dynamic here:
    //     with no measurement noise sigma_e sits on its 0.25-frame floor, Kp is large, and at a
    //     5 s horizon wn ~ 0.35 rad/s wiped out the phase margin (xtal railed to 183-199 ppm).
    // The delay-limited law showed no instability at any K or horizon tested.
    // RATE's horizon. Sized from the position delay this was 6x slower than it needed to be, for
    // no reason: the ring is not in the rate loop.
    const float rate_h_s = static_cast<float>(profile_.rate_horizon_us()) / 1e6f;
    const float wn = rate_h_s > 0.0f ? 1.0f / (CRYSTAL_DELAY_MARGIN * rate_h_s) : 0.0f;
    // BOUNDED input. The error carries transients of tens of milliseconds -- the bench census
    // shows min -26 ms, max +50 ms on a quiet run -- and an integrator fed raw error winds them
    // straight into the estimate: measured railing both boards to the 200 ppm clamp within one
    // 10-minute boot. One frame is the natural bound, because past that the position actuator
    // owns the error and the integral has nothing to say about it. The superseded fixed-tau form
    // was robust here for this reason, integrating clamp(Kp e, +-budget) rather than e; keeping
    // that property while taking the delay-limited bandwidth is the point of this line.
    const float e_bounded = std::clamp(static_cast<float>(e),
                                       -static_cast<float>(frame_us),
                                       static_cast<float>(frame_us));
    crystal_ppm_ = std::clamp(crystal_ppm_ + wn * wn * e_bounded * dt_s,
                              -CRYSTAL_LIMIT_PPM, CRYSTAL_LIMIT_PPM);
  }
  last_obs_us_ = now_us;

  // P acts on the FILTERED error and is clamped to AUTHORITY, not to the noise budget.
  //   * filtered, because the raw error carries transients of tens of milliseconds (bench census:
  //     max +197 ms) and Kp * 197000 us would slam the actuator across its whole range on one
  //     bad sample. The filter is what makes a large clamp safe.
  //   * clamped to authority, because Kp = budget/sigma_e ALREADY bounds the injected noise to the
  //     budget -- and to sigma_filtered * Kp here, which is smaller still. Clamping the output to
  //     the budget as well conflated "how much noise may I add" with "how much error may I
  //     correct", and cost a frame correction every horizon.
  const float p_raw = differential ? proportional_gain_ppm_per_us() * err_mean_us_ : 0.0f;
  const float p_term = std::clamp(p_raw, -profile_.rate_authority_ppm, profile_.rate_authority_ppm);
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

}  // namespace esphome::snapclient::timing
