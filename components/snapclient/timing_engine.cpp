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
/// Overridable at BUILD TIME ONLY, so tests/group can sweep it (-DCRYSTAL_DELAY_MARGIN_VALUE=5.0f)
/// without a bench reflash. The default is the measured value and firmware never changes it.
#ifndef CRYSTAL_DELAY_MARGIN_VALUE
#define CRYSTAL_DELAY_MARGIN_VALUE 3.0f
#endif
constexpr float CRYSTAL_DELAY_MARGIN = CRYSTAL_DELAY_MARGIN_VALUE;
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
/// Consecutive over-limit innovations of the same sign before a jump is believed. A measurement
/// spike does not survive its own sample; a deadline re-anchor or an unannounced resync does.
/// Three, so a single fault and a coincidental pair are both still rejected.
constexpr int JUMP_CONFIRM_SAMPLES = 3;
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

int64_t Profile::filter_lag_us() const {
  // Scaled here rather than in filter_lag_for(), which two TRANSPORT estimates also call
  // (snapcast_client.cpp:3006, :4276). Those are travel-time estimates, not the error filter, and
  // must not move when the filter is swept.
  //
  // This is the constant that sets the oscillation period. Measured in tests/group 3b: the period
  // tracks filter lag monotonically, 23.9 -> 8.1 s as the lag goes 1500 -> 187 ms, roughly
  // period ~ sqrt(lag). It is the first evidenced mechanism for the bench's ~11.8 s cycle after
  // three that were proposed and killed, so it is worth being able to sweep on hardware.
  return static_cast<int64_t>(err_tau_horizons * static_cast<float>(compensation_us()));
}

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

void Engine::note_external_move(int64_t applied_us, int64_t now_us) {
  if (applied_us == 0) return;
  // The same coordinate change an issued correction makes: the filter's state moves with its
  // inputs, and observations are compensated until the move is observable. No credit -- this is
  // not evidence about our rate.
  err_mean_us_ -= static_cast<float>(applied_us);
  pending_disp_us_ = applied_us;
  pending_ref_us_ = static_cast<int64_t>(std::llround(err_mean_us_)) + applied_us;
  pending_comp_until_us_ = now_us + profile_.compensation_us();
  serialise_until_us_ = now_us + profile_.settle_us();
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
  if (obs.buffer_us > 0 && profile_.buffer_floor_us > 0 &&
      obs.buffer_us < profile_.buffer_floor_us) {
    // The RECOVERY from a starvation is not a rate measurement either -- a board that has just
    // refilled is still behind, and that error is one-signed for the whole catch-up. Extending
    // this hold past the buffer clearing its floor was tried on 2026-09-02 and REVERTED: see the
    // integral in step() for the measurements. Briefly, a starvation error and a real common drift
    // are the same shape, so suppressing one suppresses the other, and the hold cost 15-25 ppm of
    // legitimate learning whenever a common drift existed.
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
      // RATE-LIMIT THE INNOVATION. Between two observations dt apart, the true error can only
      // move by what the plant can do: the rate range times dt, plus one frame for a correction
      // that may have landed. A 30 ms jump between samples 25 ms apart is 1.2 MILLION ppm -- the
      // plant cannot do that, so such a sample is a measurement fault (tag mis-identity, a resync
      // that moved audio without telling us, an epoch step), not a displacement.
      //
      // Unfiltered, one such spike moves err_mean by alpha * 30 ms = 495 us at this cadence --
      // ten times the coarse gate -- and buys a ~22 frame step. In the two-board simulator with
      // transients at the rate the bench delivers, that produced 240 corrections and 44 ms of p90
      // skew REGARDLESS of authority, which is why raising authority did nothing.
      //
      // Clamping rather than discarding: a genuinely large move is still tracked, just over
      // several samples instead of one, and a spike contributes only its bounded share.
      const float plant_ppm_span = profile_.rate_authority_ppm + CRYSTAL_LIMIT_PPM;
      const float max_move_us =
          plant_ppm_span * dt_s + static_cast<float>(profile_.frame_us());
      const float raw_innov = x - err_mean_us_;
      // A SPIKE AND A STEP LOOK THE SAME FOR ONE SAMPLE. They differ in PERSISTENCE: a
      // measurement fault is gone next sample, a real step -- a deadline re-anchor, an
      // unannounced resync -- is still there. Clamping alone treats both as noise, and that
      // limits tracking to (span * dt + frame) per sample: ~30 us at this cadence, so 1.2 ms/s.
      // A 120 ms deadline step then takes 100 s to track, and the loop is BLIND to a real
      // differential the whole time -- measured in tests/group as 384 ms of skew with ZERO
      // corrections, which is the shape of today's bench regression.
      //
      // So: clamp, but count consecutive over-limit innovations of the same sign, and once they
      // persist, accept the observation outright. Spikes never reach the count.
      // The jump test is scaled to the NOISE, not to plant motion. max_move_us is ~30 us at this
      // cadence while the measurement carries 80 us of noise, so ordinary samples exceed it
      // constantly and three same-sign ones arrive by chance -- the filter then snapped to a noisy
      // sample and amplified it (rate sd 13.31 ppm against an 11.43 ppm budget, and 24 corrections
      // where none were needed). A real step clears several sigma; noise does not.
      const float jump_us = std::max(max_move_us, 4.0f * sigma_e_us());
      if (std::fabs(raw_innov) > jump_us) {
        const int dir = raw_innov > 0.0f ? 1 : -1;
        if (dir == jump_dir_) {
          jump_run_++;
        } else {
          jump_dir_ = dir;
          jump_run_ = 1;
        }
      } else {
        jump_run_ = 0;
        jump_dir_ = 0;
      }
      if (jump_run_ >= JUMP_CONFIRM_SAMPLES) {
        cmd.decision.err_snap_us = static_cast<int32_t>(std::llround(x - err_mean_us_));
        err_mean_us_ = x;          // a real step: stop pretending it is noise
        jump_run_ = 0;
        jump_dir_ = 0;
      } else {
        err_mean_us_ += alpha * std::clamp(raw_innov, -max_move_us, max_move_us);
      }
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
  // The old test was `|gd| * unhalve >= target_diff_us`, which asks the wrong question. On the
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
  float gd_sigma_us = 0.0f;
  // HOLD, don't sample, while this board is stepping its own audio. The differential the filter
  // would read is this board's own correction reflected back at it (bench 2026-09-03: gd read 97%
  // of a 1136-frame step), and the filter clamps rather than rejects, so it would be adopted in
  // part. The last good value stays in gd_mean_us_ and continues to drive P.
  //
  // gd_last_at_us_ IS refreshed during the hold. The first version deliberately did not, reasoning
  // that the interval should reflect real observations -- which is backwards: gmax = (authority +
  // CRYSTAL_LIMIT) * gdt + frame, so letting gdt grow across the hold WIDENS the clamp, and the
  // first sample after release lands harder than if the hold had never happened. Measured: skew
  // p90 1782 -> 15792 us and corrections 61 -> 96 with the interval left to grow.
  const bool gd_hold = profile_.gate_gd_on_transient && group.self_transient;
  if (group.present && group.contributors > 1 && !gd_hold) {
    // FILTERED, with the same discipline as the deadline error: same time constant from the
    // measured transport delay, same consecutive-difference noise estimate, same innovation
    // rate-limit. Acting on the raw delta is what made single measurement spikes into whole-frame
    // steps -- the filter existed but only covered a signal position had stopped using.
    const float gx = static_cast<float>(group.delta_us);
    if (!gd_seeded_) {
      gd_mean_us_ = gx;
      gd_last_us_ = gx;
      gd_diff_us_ = static_cast<float>(profile_.frame_us());
      gd_seeded_ = true;
    } else {
      const int64_t gdt = gd_last_at_us_ > 0 ? now_us - gd_last_at_us_ : 0;
      const float gdt_s = gdt > 0 ? static_cast<float>(gdt) / 1e6f : 0.0f;
      // The DIFFERENTIAL feeds rate's proportional term (e_position = e_diff whenever the group
      // supplies one), so this is the filter that actually gates how fast rate may believe
      // anything. It follows the rate-side horizon, which equals the shared one unless the split
      // is enabled -- see Profile::rate_filter_lag_us.
      const float gtau_s =
          std::max(1e-3f, static_cast<float>(profile_.rate_filter_lag_effective_us()) / 1e6f);
      const float galpha = gdt_s > 0.0f ? 1.0f - std::exp(-gdt_s / gtau_s) : 0.0f;
      const float gspan = profile_.rate_authority_ppm + CRYSTAL_LIMIT_PPM;
      const float gmax = gspan * gdt_s + static_cast<float>(profile_.frame_us());
      const float g_innov = gx - gd_mean_us_;
      // Same spike-versus-step distinction, on the signal position actually acts on.
      const float g_jump_us = std::max(gmax, 4.0f * gd_sigma_prev_us_);
      if (std::fabs(g_innov) > g_jump_us) {
        const int gdir = g_innov > 0.0f ? 1 : -1;
        if (gdir == gd_jump_dir_) {
          gd_jump_run_++;
        } else {
          gd_jump_dir_ = gdir;
          gd_jump_run_ = 1;
        }
      } else {
        gd_jump_run_ = 0;
        gd_jump_dir_ = 0;
      }
      if (gd_jump_run_ >= JUMP_CONFIRM_SAMPLES) {
        cmd.decision.gd_snap_us = static_cast<int32_t>(std::llround(gx - gd_mean_us_));
        gd_mean_us_ = gx;
        gd_jump_run_ = 0;
        gd_jump_dir_ = 0;
      } else {
        gd_mean_us_ += galpha * std::clamp(g_innov, -gmax, gmax);
      }
      // THIS ESTIMATE IS DILUTED, DELIBERATELY, AND MUST NOT BE READ AS A NOISE FIGURE.
      //
      // The group delta is HELD between pairings -- present on every decision but freshly paired
      // on ~32% of them -- so most consecutive pairs are bit-identical and contribute a zero
      // difference. That drags gd_sigma toward its quarter-frame floor: measured 5.5 us in
      // tests/group where the phase noise implies ~14, and mode 5.5 on the bench too.
      //
      // Differencing only FRESH samples was tried (2026-09-03) and REVERTED, because gd_sigma is
      // not only an estimate -- it also sets the position gate (gate_sigma) and the jump detector
      // (4 * gd_sigma_prev), and both were calibrated around the diluted value. Correcting the
      // estimator widened the gate and raised the snap threshold, so position corrected less and
      // the filter admitted more. Measured on the wire at an unchanged target of 10:
      //
      //     diluted (this form)   median 2-min p2p  87.7 us,  sd 14.15   (12 windows)
      //     fresh-only           median 2-min p2p 118.7 us,  sd 17.98   ( 8 windows)
      //
      // 31 us of stereo image wander is not worth a more honest number in a field nothing yet
      // consumes as a noise figure. If a future change needs a true sigma for the differential --
      // sizing Kp from it, say -- compute that SEPARATELY and leave this one feeding the gate and
      // the jump detector it was tuned for. Two consumers, two quantities.
      gd_diff_us_ += galpha * (std::fabs(gx - gd_last_us_) - gd_diff_us_);
      gd_last_us_ = gx;
    }
    gd_last_at_us_ = now_us;
    gd_sigma_us = std::max(0.8862f * gd_diff_us_, 0.25f * static_cast<float>(profile_.frame_us()));
    gd_sigma_prev_us_ = gd_sigma_us;
    e_diff = static_cast<int64_t>(std::llround(gd_mean_us_));
    have_diff = true;
    differential = std::llabs(e_diff) >= profile_.target_diff_us;
  } else if (gd_hold && gd_seeded_) {
    // HOLDING STILL SUPPLIES A DIFFERENTIAL. Dropping have_diff here would send e_position to the
    // DEADLINE error instead -- 700 us to 10 ms on the bench against a differential of tens of us
    // -- so a gate meant to reject a phantom would hand P something an order of magnitude worse.
    // The held value is the last differential measured while the phase still described the audio.
    e_diff = static_cast<int64_t>(std::llround(gd_mean_us_));
    have_diff = true;
    gd_last_at_us_ = now_us;
    gd_sigma_us = gd_sigma_prev_us_;
    differential = std::llabs(e_diff) >= profile_.target_diff_us;
  } else if (profile_.position_needs_diff && group.has_peers) {
    // PEERS EXIST BUT NO DELTA IS AVAILABLE. Fall closed, not open: differential's initialiser is
    // true, which would let position spend frames on the DEADLINE error with nothing corroborating
    // it. A missing signal is not a signal that says zero. Rate is unaffected and the hard resync
    // still covers a genuine large displacement; only the irreversible actuator stands down.
    //
    // A LONE client keeps the old behaviour, because tracking the server is the only meaning "in
    // sync" has when there is nobody to be in sync with -- which is why this needs has_peers and
    // not merely !present.
    differential = false;
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
  // Gate on the noise of the signal position TESTS. With position on the differential error, a
  // gate sized from the deadline error's noise is measuring the wrong distribution.
  const float gate_sigma =
      have_diff ? gd_sigma_us * std::sqrt(std::max(1e-6f, alpha_eff) / 2.0f) : sigma_filtered;
  const int64_t coarse_gate_us =
      frame_us + static_cast<int64_t>(std::llround(2.0f * gate_sigma));
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
  // Rate can fix it only if rate can also HOLD the fix. needed_ppm within authority says the
  // proportional term could reach the error; it says nothing about whether the integral can take
  // over afterwards. When the crystal is already at its clamp in the direction the error demands,
  // it cannot: P corrects transiently, P decays as the error does, and the error returns. Position
  // then stands down for work rate is structurally unable to finish.
  //
  // Measured in test 14 at a 150 ppm deadline drift, past the crystal's 200 ppm clamp: needed
  // 92 ppm against 100 authority read as "rate can fix it", the loop spent ZERO frames, and the
  // error peaked at 162 ms.
  const bool crystal_spent =
      (crystal_ppm_ >= CRYSTAL_LIMIT_PPM && needed_ppm > 0.0f) ||
      (crystal_ppm_ <= -CRYSTAL_LIMIT_PPM && needed_ppm < 0.0f);
  const bool rate_can_fix =
      !crystal_spent && std::fabs(needed_ppm) <= profile_.rate_authority_ppm;
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
  cmd.decision.e_position_us = e_position;
  cmd.decision.e_from_diff = have_diff;
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
    // THE DIFFERENTIAL FILTER NEEDS THE SAME CHANGE OF COORDINATES, and never had it. The comment
    // above is about err_mean_us_, but position is driven by e_position, which is e_diff whenever
    // the group supplies one -- so when a correction comes from the differential, gd_mean_us_ is
    // the filter left "holding the pre-correction value and decaying toward the compensated
    // stream", crossing the coarse gate again on the way down and buying the same correction
    // repeatedly.
    //
    // Measured 2026-09-03 04:52, board a, all three corrections esrc=d: ep decayed
    // -6049 -> -2740 -> -960 us against a raw GDIN gd of +-20, spending -274, -124 and -43 frames
    // through a shrinking gate (48 -> 46 -> 35). tests/group 3k puts 25% of frames in the short
    // case and 54% in the long one AFTER the fault that loaded the filter had gone.
    //
    // MEASURED AND REJECTED, 2026-09-03, tests/group 3m. The flag stays OFF. In the only scenario
    // that exercises this branch (esrc d/l = 279/0, so every correction takes it) turning it on
    // made the repeat signature WORSE, not better: repeat corrections 11 -> 15, frames spent twice
    // 5537 -> 8172, gross frames 68598 -> 76759. The reasoning below is intact and the branch is
    // reached; the sign of the effect is simply not what it predicts. Do not enable it on the
    // strength of the argument alone -- that argument is written out in full here and it is wrong.
    // What is NOT yet separated: whether the share is mis-sized (over-shooting past zero, the risk
    // the paragraph below names) or whether shifting gd_mean_us_ at all is the wrong move. A share
    // sweep is the test that would tell them apart.
    //
    // (n-1)/n, NOT the whole displacement. group.delta_us is this board's deviation from the group
    // MEAN, so moving this board by D moves its deviation by D*(n-1)/n -- D/2 for a pair. This is
    // the same factor the "NO unhalve" note guards, in the opposite direction: over-compensating
    // here would drag gd_mean_ past zero and buy a correction back the other way.
    if (have_diff && group.contributors > 1 && profile_.compensate_gd_filter) {
      const float share = static_cast<float>(group.contributors - 1) /
                          static_cast<float>(group.contributors);
      gd_mean_us_ -= static_cast<float>(pending_disp_us_) * share;
    }
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
    // TRIED AND REVERTED: integrating only the COMMON part (e - e_diff), so the differential
    // could not leak into the crystal. It broke differential correction outright -- group case 2
    // went from 16 us of skew to 199 us -- because a differential caused by a PLANT RATE
    // difference needs integral action to null it, and that change removed the differential from
    // the only integrator there is. P alone leaves a steady-state error of (rate difference)/Kp.
    //
    // Each board's crystal must therefore keep learning its OWN plant from its own deadline
    // error, which is what makes two boards with different crystals converge in rate at all.
    // CLAMP EACH COMPONENT SEPARATELY, or a large common error clips the differential away.
    //
    // The clamp exists to stop measurement transients winding the estimate (test 12), and it must
    // stay. But applied to the whole error it saturates on whichever component is larger: with a
    // 40 ppm common drift the error grows fast, the clamp pins at one frame, and the few
    // microseconds of DIFFERENTIAL riding on top are discarded -- so the crystal can never learn
    // the plant difference, and P is left holding it alone at a steady-state error of
    // (rate difference)/Kp.
    //
    // Measured in tests/group: common drift alone 1 us of skew, a differential alone 16 us, and
    // the two TOGETHER 90 us -- a nonlinear interaction, and this clamp is the only nonlinearity
    // on the path. Bounding the parts independently keeps the transient protection while leaving
    // the differential visible to the integral.
    const float e_common_f = static_cast<float>(have_diff ? e - e_diff : e);
    const float e_diff_f = static_cast<float>(have_diff ? e_diff : 0);
    const float fu = static_cast<float>(frame_us);
    // THE INTEGRAL KEEPS THE COMMON PART even when the shared drain is active. Excluding it was
    // tried and is wrong: pc is PROPORTIONAL, so it needs a standing error to produce output, and
    // a sustained 40 ppm common drift at drain_s=300 would demand a permanent 12 ms error to
    // generate the ppm it needs. Zero steady-state error on the common part is the integral's job
    // and nothing else here can do it.
    //
    // The two are not competing for the same work: the integral handles common DRIFT (a rate that
    // must be matched indefinitely), while pc exists to drain a common DISPLACEMENT before it
    // reaches the resync threshold. pc is bounded and slow enough that the integral simply sees a
    // slightly faster-decaying error.
    const float e_bounded =
        std::clamp(e_common_f, -fu, fu) + std::clamp(e_diff_f, -fu, fu);
    cmd.decision.e_split_valid = true;
    cmd.decision.e_common_us = static_cast<int32_t>(std::llround(e_common_f));
    cmd.decision.e_diff_us = static_cast<int32_t>(std::llround(e_diff_f));
    cmd.decision.e_bounded_us = static_cast<int32_t>(std::llround(e_bounded));
    // BOUND THE STEP BY BOUNDING dt. dt_s is the time since the last observation, and during a
    // storm observations stop for seconds -- so the integral was applying wn^2 * bound * dt in
    // ONE step, as though it had been watching the whole time. With the horizon collapsed to its
    // floor (wn^2 = 2.95) a 2 s gap moves the estimate 134 ppm instantly.
    //
    // Measured on board a, single steps: -93.8 ppm within one log second, -105.7 ppm over 1 s,
    // -110.5 ppm over 2 s, ending railed at -200 ppm from a +179 peak. Neither credit (5 ppm per
    // window) nor the steady integral (0.59 ppm/s at a healthy horizon) can move it that fast;
    // an unbounded dt can.
    //
    // A gap longer than the loop's own horizon means observability was lost, and integrating
    // across it is not justified by anything: past that the estimate should wait, not extrapolate.
    const float dt_int_s = std::min(dt_s, std::max(1e-3f, rate_h_s));
    // TRIED AND REVERTED, 2026-09-02: holding this for one settle window past the buffer clearing
    // its floor, to stop a starvation's one-signed error winding the estimate. It does stop that.
    // It also stops the integral learning a REAL common drift, because the two are the same shape
    // -- a persistent one-signed error -- and nothing here can tell them apart.
    //
    // Measured against the correct target (rate = plant + common, since holding the error still
    // requires cancelling both), plant -15/+15 with common +40, guard off vs on:
    //
    //   starve 10s/20s   want +25/+55   off +23.2/+47.2   on  +2.5/+29.0    off much better
    //   starve  6s/40s   want +25/+55   off +10.4/+38.3   on  +7.7/+33.5    off better
    //   starve 10s/20s, common 0        off +10.2/+33.5   on -2.1/+29.2     on better
    //
    // It only wins when there is no common drift to learn, which is not this bench. The first
    // version of the test asserted crystal ~= PLANT rather than plant + common and so scored the
    // hold as a 20 ppm improvement when it was really a 25 ppm regression.
    crystal_ppm_ = std::clamp(crystal_ppm_ + wn * wn * e_bounded * dt_int_s,
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
  // NO BOOLEAN ON A CONTINUOUS ACTUATOR. `differential` gates POSITION, and a threshold suits it:
  // frames are quantised and a correction is irreversible, so there is a real question of whether
  // to spend one. Rate is continuous and reversible, and gating it on the same boolean made the
  // command switch between 0 and its rail instead of ramping.
  //
  // Measured on the bench 2026-09-02, once the frame corrections were gone and the trace was
  // still jagged: why=Common on 164/269 decisions (61%) on board a and 130/263 (49%) on b, so P
  // was exactly 0 on most decisions and up to +-100 ppm on the rest -- single-step jumps of
  // 100.78 ppm, commanded-rate sd 35.8 ppm against a crystal sd of 5.7. The crystal was smooth;
  // the switch was doing all the jumping.
  //
  // And the two halves disagreed about which error they were about: the gate tested the
  // DIFFERENTIAL (>= target_diff_us, 20 us) while P was computed from the DEADLINE error
  // (median 102 us, p90 351 us). So 21 us of differential unlocked a 100 ppm response driven by
  // an unrelated 350 us number. With sigma_e pinned at its one-frame floor, Kp saturates P past
  // 44 us of error, which is why the rail was reached at all.
  //
  // So P acts on the error position is judged on, continuously: no boolean, and the same quantity
  // throughout. A differential of 21 us now asks for 10 ppm, not 100. Where there is no group to
  // be differential from, it falls back to the deadline error, which is correct for a lone client
  // -- tracking the server is then the only meaning "in sync" has.
  // SIZE Kp FROM THE NOISE OF THE SIGNAL P MULTIPLIES. Kp = budget / sigma is a promise that the
  // command noise integrates to no more than target_diff_us over the horizon, and that promise is
  // only kept if sigma describes the signal actually being multiplied. P acts on e_position, which
  // is the DIFFERENTIAL whenever the group supplies one -- but sigma_e_us() is the DEADLINE error's
  // noise. Two different distributions, and the budget was computed from the wrong one.
  //
  // The identical error was already found and fixed for the position gate ten lines up ("a gate
  // sized from the deadline error's noise is measuring the wrong distribution"); the reasoning was
  // simply never carried across to the gain.
  //
  // Measured 2026-09-03: sigma_e pinned at its 22 us frame floor while sigma(gd) was 30.5 us, so
  // Kp ran ~1.4x the budget it claimed. Halving timing_target_us (which halves Kp) improved BOTH
  // the wire and the rate activity -- offset sd 16.25 -> 12.41 us and rate p2p 87.9 -> 58.6 ppm,
  // with a revert to 20 degrading both again. Two metrics improving together is over-gain, not a
  // trade, and lowering the target only compensated for the wrong denominator by coincidence: it
  // would stop compensating the moment gd's noise moved, and board a's gd p90 reached 333 us
  // earlier the same night.
  //
  // The one-frame floor comes with it, for the reason sigma_e_us() documents: below one frame the
  // clamp turns P into a relay and the loop bang-bangs against the transport delay. gd_sigma_us
  // floors at a QUARTER frame, which is fine for the gate it was built for and not for a gain.
  const float p_sigma_us =
      (profile_.kp_from_diff_sigma && have_diff)
          ? std::max(gd_sigma_us, static_cast<float>(profile_.frame_us()))
          : sigma_e_us();
  float kp_used = p_sigma_us > 0.0f ? profile_.rate_noise_budget_ppm() / p_sigma_us : 0.0f;
  // STABILITY CAP. The budget above is a noise constraint; this is the phase-margin one, and
  // nothing in the design had it. Kp_crit = pi/(2L) for an integrator behind L of dead time, and
  // rate_horizon_us IS that L. Capping can only ever REDUCE the gain, so it cannot introduce an
  // instability of its own -- it removes the one the noise budget was blind to.
  bool kp_capped = false;
  if (profile_.kp_stability_frac > 0.0f) {
    const float horizon_s_cap = static_cast<float>(profile_.rate_horizon_us()) / 1e6f;
    if (horizon_s_cap > 0.0f) {
      const float kp_crit = 1.5707963f / horizon_s_cap;   // pi/(2L)
      const float kp_max = profile_.kp_stability_frac * kp_crit;
      if (kp_used > kp_max) {
        kp_used = kp_max;
        kp_capped = true;
      }
    }
  }
  const float p_raw = kp_used * static_cast<float>(e_position);
  const float p_term = std::clamp(p_raw, -profile_.rate_authority_ppm, profile_.rate_authority_ppm);

  // SLEW-LIMIT THE COMMAND. A continuous actuator's command should be continuous -- that is the
  // whole reason to prefer rate over position -- and a command that steps forfeits it.
  //
  // P is proportional, so whatever steps the FILTER steps the command with it. The jump detector
  // does precisely that by design: on a confirmed step it snaps the mean to the observation, and
  // P then slams by Kp * step, about 100 ppm for a 200 us jump. Measured on the bench after the
  // boolean gate was removed: the trim is smooth at 3-5 ppm per report most of the time, with
  // occasional excursions of 49-117 ppm -- board b's applied trim read
  // +115 +58 +56 +55 +47 +56 +73 +66 across consecutive reports. Removing the boolean
  // discontinuity had left a snap discontinuity in its place.
  //
  // A jump in the differential is a DISPLACEMENT, which is what the position actuator is for: it
  // exceeds the coarse gate and is corrected there. Rate owns the continuous part, so its command
  // may traverse the full authority over one horizon and no faster. Derived from the authority
  // and the horizon, so it carries no time of its own.
  const float horizon_s = std::max(1e-3f, static_cast<float>(profile_.rate_horizon_us()) / 1e6f);
  // dt bounded by the horizon for the same reason the integral's is: a long gap means
  // observability was lost, not that the command earned the right to jump.
  const float dt_cmd_s =
      dt_us > 0 ? std::min(static_cast<float>(dt_us) / 1e6f, horizon_s) : 0.0f;
  const float max_slew_ppm = profile_.rate_authority_ppm * (dt_cmd_s / horizon_s);

  // THE SHARED COMMON-MODE TERM. Recorded unconditionally so it can be shadowed; applied only
  // when a gain is configured.
  //
  // This is the one term in the command that is NOT this board's own opinion. Every member
  // holding the same set computes the same number, so whatever it does, it does to the group
  // together -- which is why it may run at a gain the crystal integral may not, and why it is
  // clamped by common_authority_ppm rather than by the authority that bounds solo motion.
  //
  // It acts on the consensus, never on this board's own e_common: acting on the latter is
  // precisely the per-board gain the design forbids, and would convert the ~80 us of per-board
  // measurement noise straight into differential motion.
  cmd.decision.common_shared_valid = group.common_valid;
  cmd.decision.common_n = group.common_n;
  float pc_term = 0.0f;
  if (group.common_valid) {
    cmd.decision.common_shared_us = static_cast<int32_t>(
        std::clamp<int64_t>(group.common_us, INT32_MIN, INT32_MAX));
    if (profile_.common_drain_s > 0.0f) {
      // DEADBAND AT target_common_us, working only on the EXCESS beyond it. A common error is
      // inaudible, so correcting one inside the target spends rate-command noise -- which lands
      // on the DIFFERENTIAL, the audible quantity -- to fix something nobody can hear. Measured
      // 2026-09-03 with the loop healthy: +1192 and +1282 us of common error on the two boards
      // while the wire held 18 us sd. Chasing that would have been strictly harmful.
      //
      // It is also what lets this coexist with the crystal integral instead of fighting it. An
      // earlier attempt removed the common component from the integral's input so the two would
      // not both chase it, and that was wrong: pc is PROPORTIONAL and cannot deliver zero
      // steady-state error, so a sustained 40 ppm common drift would have demanded a permanent
      // 12 ms error to generate the ppm it needed. With a deadband the division is clean -- the
      // integral owns common DRIFT at all times, pc owns only the large excursions that threaten
      // the resync threshold.
      const int64_t dead_us = std::max<int64_t>(0, profile_.target_common_us);
      int64_t excess_us = 0;
      if (group.common_us > dead_us) {
        excess_us = group.common_us - dead_us;
      } else if (group.common_us < -dead_us) {
        excess_us = group.common_us + dead_us;
      }
      if (excess_us != 0) {
        // us / s == ppm. Sized to drain over MINUTES -- the budget that matters is staying under
        // the resync threshold between disturbances, not rate's 2 s horizon, which no achievable
        // rate can satisfy.
        const float pc_raw = static_cast<float>(excess_us) / profile_.common_drain_s;
        pc_term = std::clamp(pc_raw, -profile_.common_authority_ppm, profile_.common_authority_ppm);
      }
    }
  }
  cmd.decision.pc_ppm = pc_term;

  const float want = crystal_ppm_ + p_term + pc_term;
  const float prev_cmd = last_rate_cmd_;
  cmd.rate_ppm =
      rate_cmd_seeded_
          ? last_rate_cmd_ + std::clamp(want - last_rate_cmd_, -max_slew_ppm, max_slew_ppm)
          : want;
  // ATTRIBUTION, recorded where it is unambiguous. Every term is known here; a consumer trying to
  // reconstruct this later has to guess from timing alone, which is what defeated two attempts to
  // explain the wire's rate excursions.
  if (rate_cmd_seeded_) {
    cmd.decision.d_rate_ppm = cmd.rate_ppm - prev_cmd;
    cmd.decision.d_crystal_ppm = crystal_ppm_ - last_crystal_ppm_;
    cmd.decision.d_p_ppm = p_term - last_p_ppm_;
    cmd.decision.slew_clipped = std::fabs(want - prev_cmd) > max_slew_ppm;
  }
  cmd.decision.p_ppm = p_term;
  // THE GAIN ACTUALLY USED, not a re-derivation. Reporting proportional_gain_ppm_per_us() here
  // while P was computed from a different sigma would make RATEWHY's kp field describe a gain the
  // loop never applied -- and P = kp * dif was the identity that made the excursions explicable in
  // the first place. An instrument that reports a number the code did not use is worse than none.
  cmd.decision.kp_ppm_per_us = kp_used;
  cmd.decision.p_sigma_us = p_sigma_us;
  cmd.decision.gd_sigma_us = gd_sigma_us;
  cmd.decision.kp_capped = kp_capped;
  last_crystal_ppm_ = crystal_ppm_;
  last_p_ppm_ = p_term;
  rate_cmd_seeded_ = true;
  last_rate_cmd_ = cmd.rate_ppm;
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
