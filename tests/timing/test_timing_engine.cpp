// Host test for the timing engine. No ESPHome, no hardware:
//
//   c++ -std=c++17 -O0 -Wall -Wextra -o /tmp/tt \
//       components/snapclient/timing/test_timing_engine.cpp \
//       components/snapclient/timing/timing_engine.cpp && /tmp/tt
//
// Simulates a plant with a known rate offset, feeds the engine its own commands, and checks the
// properties the design claims. The bench cannot produce most of these cases on demand.

#include "../../components/snapclient/timing_engine.h"

#include <cmath>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <algorithm>

using namespace esphome::snapclient::timing;

namespace {

int failures = 0;

void check(bool ok, const char *what, double got = 0.0, double want = 0.0) {
  if (ok) {
    printf("  ok    %s\n", what);
  } else {
    printf("  FAIL  %s  (got %.3f, want %.3f)\n", what, got, want);
    failures++;
  }
}

/// Plant: position error integrates (plant_ppm − commanded_ppm). Frame corrections step it.
struct Plant {
  double error_us = 0.0;
  double plant_ppm = 0.0;
  int64_t frame_us = 0;
  double noise_us = 0.0;
  std::mt19937 rng{1};

  void advance(int64_t dt_us, double commanded_ppm) {
    error_us += (plant_ppm - commanded_ppm) * 1e-6 * static_cast<double>(dt_us);
  }
  void apply_frames(int32_t frames) {
    // Dropping frames (positive) removes lateness.
    error_us -= static_cast<double>(frames) * static_cast<double>(frame_us);
  }
  double observe() {
    if (noise_us <= 0.0) return error_us;
    std::normal_distribution<double> d(0.0, noise_us);
    return error_us + d(rng);
  }
};

struct Run {
  double final_error_us = 0.0;
  double final_crystal_ppm = 0.0;
  int position_corrections = 0;
  double peak_abs_error_us = 0.0;
  double rate_sd_ppm = 0.0;
};

/// Run the closed loop for `seconds`, observing every `tick_us`.
///
/// The plant moves when the frames are actually dropped -- one pipeline depth away, ~350 ms --
/// but the MEASUREMENT does not show it for a whole visibility horizon (ring + pipe + the error
/// filter's own lag). Those are different times and the harness used to collapse them into one,
/// landing the plant and confirming at t + visibility together. That made the harness incapable
/// of expressing the hardware's actual behaviour: on the bench the engine sees a full visibility
/// window of pre-correction observations after it has already corrected, and re-issues.
///
/// So observations here come from a delay line, `visibility_us` behind the plant.
Run simulate(Profile p, double plant_ppm, double seconds, double noise_us = 0.0,
             bool group_present = false, int64_t group_delta_us = 0, uint8_t contributors = 2) {
  Engine eng(p);
  Plant plant;
  plant.plant_ppm = plant_ppm;
  plant.frame_us = p.frame_us();
  plant.noise_us = noise_us;

  const int64_t tick_us = 25000;  // ~40 Hz observations; the engine assumes no cadence
  const int64_t pipe_us = 350000;  // when the dropped frames actually leave the pipeline
  Run r{};
  double sum = 0.0, sumsq = 0.0;
  int n = 0;

  // Delay line: what the measurement reports now is where the plant was visibility_us ago.
  std::deque<std::pair<int64_t, double>> history;

  struct Pending {
    uint64_t id;
    int32_t frames;
    int64_t land_at;
    bool live;
  } pend{0, 0, 0, false};

  double last_rate = 0.0;
  for (int64_t t = 0; t < static_cast<int64_t>(seconds * 1e6); t += tick_us) {
    if (pend.live && t >= pend.land_at) {
      plant.apply_frames(pend.frames);
      eng.confirm_position_landed(pend.id, t);
      pend.live = false;
    }

    // Record the truth, then report it visibility_us late.
    history.emplace_back(t, plant.observe());
    double seen = history.front().second;
    while (history.size() > 1 && history.front().first <= t - p.visibility_us) {
      seen = history.front().second;
      history.pop_front();
    }
    Observation obs{t, static_cast<int64_t>(std::llround(seen)), true};
    GroupEvidence g{};
    g.present = group_present;
    g.delta_us = group_delta_us;
    g.contributors = contributors;

    Command cmd = eng.step(t, obs, g);
    if (cmd.frames != 0) {
      r.position_corrections++;
      pend = {cmd.correction_id, cmd.frames, t + pipe_us, true};
    }
    plant.advance(tick_us, cmd.rate_ppm);

    r.peak_abs_error_us = std::max(r.peak_abs_error_us, std::fabs(plant.error_us));
    sum += cmd.rate_ppm;
    sumsq += cmd.rate_ppm * cmd.rate_ppm;
    n++;
    last_rate = cmd.rate_ppm;
  }
  (void) last_rate;
  r.final_error_us = plant.error_us;
  r.final_crystal_ppm = eng.crystal_ppm();
  if (n > 1) {
    const double mean = sum / n;
    r.rate_sd_ppm = std::sqrt(std::max(0.0, sumsq / n - mean * mean));
  }
  return r;
}

}  // namespace

int main() {
  Profile p;
  p.frame_rate_hz = 44100;
  p.visibility_us = 1000000;
  p.target_position_us = 20;

  printf("profile: frame %lld us, budget %.3f ppm, Kp cap implied %.5f ppm/us\n",
         static_cast<long long>(p.frame_us()), p.rate_noise_budget_ppm(),
         p.rate_noise_budget_ppm() / static_cast<float>(p.frame_us()));

  printf("\n1. no plant offset, no noise: nothing should move\n");
  {
    Run r = simulate(p, 0.0, 120.0);
    check(std::fabs(r.final_error_us) < 5.0, "error stays near zero", r.final_error_us, 0.0);
    check(r.position_corrections == 0, "no frame corrections",
          r.position_corrections, 0);
  }

  printf("\n2. plant offset 3.35 ppm (the bench asymmetry): the integral must LEARN it\n");
  {
    Run r = simulate(p, 3.35, 600.0);
    check(std::fabs(r.final_crystal_ppm - 3.35) < 1.0,
          "crystal converges on the plant offset", r.final_crystal_ppm, 3.35);
    check(std::fabs(r.final_error_us) < static_cast<double>(p.frame_us()),
          "residual error under one frame", r.final_error_us, 0.0);
  }

  printf("\n3. same offset, and the rate path must TAKE OVER from the position path\n");
  {
    // Corrections should cluster early and stop once the offset is learned. If they continue at a
    // steady cadence for ever, the integral is not learning -- the failure seen on the bench.
    // 30 ppm, so corrections definitely fire before the offset is learned. The previous version
    // used 3.35 ppm, produced zero corrections in both runs, and compared 0 against 0.
    Run early = simulate(p, 30.0, 60.0);
    Run late = simulate(p, 30.0, 900.0);
    const double rate_early = early.position_corrections / 60.0;
    const double rate_late = late.position_corrections / 900.0;
    printf("        early %.3f/s over 60s, late %.3f/s over 900s\n", rate_early, rate_late);
    check(rate_early > 0.0, "corrections do fire before the offset is learned", rate_early, 0.0);
    check(rate_late < rate_early * 0.6, "correction rate falls as the offset is learned",
          rate_late, rate_early * 0.6);
  }

  printf("\n4. large step (one frame equivalent): delivered once, not repeatedly\n");
  // Swept across the WHOLE visibility range travel_horizon_us_() can produce -- it is clamped to
  // [1 s, 5 s] and varies with ring and pipe depth at runtime, so the property has to hold across
  // it, not at one value. Testing only 1 s hid this: there the stale window is 0.65 s and the
  // filter (tau 2 s) cannot climb back over the coarse gate. At 3 s it can.
  for (int64_t vis_us : {1000000, 2000000, 3000000, 4000000, 5000000}) {
    Profile pv = p;
    pv.visibility_us = vis_us;
    Engine eng(pv);
    Plant plant;
    plant.frame_us = pv.frame_us();
    plant.error_us = 3.0 * static_cast<double>(pv.frame_us());  // 3 frames late
    int corrections = 0;
    uint64_t pend_id = 0;
    int32_t pend_frames = 0;
    bool live = false;
    int64_t land = 0;
    // Same two-timescale model as simulate(): the plant moves at the pipeline depth, the
    // measurement follows a visibility horizon later. Landing and visibility were one time here
    // too, which is why this property passed while the bench re-issued.
    std::deque<std::pair<int64_t, double>> history;
    for (int64_t t = 0; t < 20000000; t += 25000) {
      if (live && t >= land) {
        plant.apply_frames(pend_frames);
        eng.confirm_position_landed(pend_id, t);
        live = false;
      }
      history.emplace_back(t, plant.error_us);
      double seen = history.front().second;
      while (history.size() > 1 && history.front().first <= t - pv.visibility_us) {
        seen = history.front().second;
        history.pop_front();
      }
      Observation obs{t, static_cast<int64_t>(std::llround(seen)), true};
      Command cmd = eng.step(t, obs, GroupEvidence{});
      if (cmd.frames != 0) {
        corrections++;
        pend_id = cmd.correction_id;
        pend_frames = cmd.frames;
        land = t + 350000;  // pipeline depth, NOT the visibility horizon
        live = true;
      }
      plant.advance(25000, cmd.rate_ppm);
    }
    printf("        visibility %lld ms: %d corrections, residual %+.1f us\n",
           static_cast<long long>(vis_us / 1000), corrections, plant.error_us);
    check(corrections <= 2, "3-frame error costs at most 2 corrections", corrections, 2);
    check(std::fabs(plant.error_us) < static_cast<double>(pv.frame_us()),
          "settles inside one frame", plant.error_us, 0.0);
  }

  printf("\n5. measurement noise must not be amplified into the rate command\n");
  {
    Run quiet = simulate(p, 0.0, 300.0, 0.0);
    Run noisy = simulate(p, 0.0, 300.0, 80.0);  // 80 us sd, the measured differential noise
    // Assert the design property, not an arbitrary number: command noise within the budget the
    // profile asks for. budget = target_position / visibility.
    const double budget = p.rate_noise_budget_ppm();
    printf("        rate sd %.2f ppm against a %.2f ppm budget\n", noisy.rate_sd_ppm, budget);
    check(noisy.rate_sd_ppm <= budget * 1.1, "rate command noise stays within budget",
          noisy.rate_sd_ppm, budget);
    check(noisy.position_corrections < 40,
          "noise alone does not drive constant frame corrections",
          noisy.position_corrections, 40);
    (void) quiet;
  }

  printf("\n6. common-mode error with group evidence: do not chase it\n");
  {
    // Group says our offset from the group is 0, so the error is common: no correction at all.
    Run common = simulate(p, 0.0, 120.0, 0.0, true, 0, 2);
    check(common.position_corrections == 0, "no frame corrections on a common error",
          common.position_corrections, 0);
    check(std::fabs(common.final_crystal_ppm) < 0.5,
          "crystal not walked by a common error", common.final_crystal_ppm, 0.0);
  }

  printf("\n7. hold on missing observations\n");
  {
    Engine eng(p);
    Command a = eng.step(0, Observation{0, 500, true}, GroupEvidence{});
    Command b = eng.step(100000, Observation{}, GroupEvidence{});
    check(b.decision.act == Decision::Act::Hold, "act is Hold", 0, 0);
    check(std::fabs(b.rate_ppm - a.decision.crystal_ppm) < 1e-6 ||
              std::fabs(b.rate_ppm - eng.crystal_ppm()) < 1e-6,
          "rate holds the learned offset, not zero", b.rate_ppm, eng.crystal_ppm());
  }

  printf("\n8. no constant depends on frame rate: 48 kHz behaves the same\n");
  {
    Profile p48 = p;
    p48.frame_rate_hz = 48000;
    Run r44 = simulate(p, 3.35, 600.0);
    Run r48 = simulate(p48, 3.35, 600.0);
    check(std::fabs(r44.final_crystal_ppm - r48.final_crystal_ppm) < 1.0,
          "crystal learned equally at 44.1 and 48 kHz",
          r48.final_crystal_ppm, r44.final_crystal_ppm);
  }

  printf("\n9b. a one-off displacement must not be credited as rate\n");
  {
    // The failure seen on hardware: 1.5 ms of accumulated error corrected in one step implied
    // 156 ppm and wound the crystal to its clamp. A displacement is not a rate.
    Engine eng(p);
    Plant plant;
    plant.plant_ppm = 0.0;                      // no rate error at all
    plant.frame_us = p.frame_us();
    plant.error_us = 1500.0;                    // but 1.5 ms of displacement
    uint64_t pid = 0; int32_t pf = 0; bool live = false; int64_t land = 0;
    for (int64_t t = 0; t < 600000000; t += 25000) {
      if (live && t >= land) { plant.apply_frames(pf); eng.confirm_position_landed(pid, t); live = false; }
      Observation obs{t, static_cast<int64_t>(std::llround(plant.error_us)), true};
      Command c = eng.step(t, obs, GroupEvidence{});
      if (c.frames != 0) { pid = c.correction_id; pf = c.frames; land = t + p.visibility_us; live = true; }
      plant.advance(25000, c.rate_ppm);
    }
    printf("        crystal after a pure 1.5 ms displacement: %.2f ppm\n", eng.crystal_ppm());
    check(std::fabs(eng.crystal_ppm()) < 10.0,
          "displacement does not become rate", eng.crystal_ppm(), 0.0);
  }

  printf("\n9. observation cadence must not retune the loop\n");
  {
    // Same plant, same duration, observations 25x apart in cadence. A fixed filter weight would
    // make these differ; a time-based one must not. This is the property that lets block_n go.
    auto run_at = [&](int64_t tick_us) {
      Engine eng(p);
      Plant plant;
      plant.plant_ppm = 3.35;
      plant.frame_us = p.frame_us();
      for (int64_t t = 0; t < 600000000; t += tick_us) {
        Observation obs{t, static_cast<int64_t>(std::llround(plant.error_us)), true};
        Command c = eng.step(t, obs, GroupEvidence{});
        plant.advance(tick_us, c.rate_ppm);
      }
      return eng.crystal_ppm();
    };
    const float fast = run_at(25000);     // 40 Hz, per tag arrival
    const float slow = run_at(650000);    // ~1.5 Hz, per 64-arrival block
    printf("        crystal at 40 Hz %.2f ppm, at 1.5 Hz %.2f ppm\n", fast, slow);
    check(std::fabs(fast - slow) < 1.0, "crystal learned equally at both cadences", slow, fast);
  }

  printf("\n10. a WRONG restored crystal must not cost position accuracy while it unwinds\n");
  {
    // Board a booted with +112 ppm in NVS against a plant that did not need it, because a limit
    // cycle had been credited as rate. The engine must keep the position error bounded WHILE it
    // unlearns that -- steering is what holds position, so anything that suspends the rate path
    // pays for it in skew. Blind-holding for the visibility horizon passed every other property
    // here and still regressed the bench 9x (median |offset| 25 us -> 224 us), because this case
    // was not tested: with the crystal already correct, coasting costs nothing.
    Profile pw = p;
    pw.visibility_us = 3000000;   // the bench's own horizon
    Engine eng(pw);
    eng.set_crystal_ppm(60.0f);   // wrong by 60 ppm: nothing in the plant asks for it
    Plant plant;
    plant.frame_us = pw.frame_us();
    plant.plant_ppm = 0.0;
    std::deque<std::pair<int64_t, double>> history;
    uint64_t pid = 0; int32_t pf = 0; bool live = false; int64_t land = 0;
    std::vector<double> errs;
    const int64_t tick = 25000;
    for (int64_t t = 0; t < 900000000; t += tick) {
      if (live && t >= land) {
        plant.apply_frames(pf);
        eng.confirm_position_landed(pid, t);
        live = false;
      }
      history.emplace_back(t, plant.error_us);
      double seen = history.front().second;
      while (history.size() > 1 && history.front().first <= t - pw.visibility_us) {
        seen = history.front().second;
        history.pop_front();
      }
      Command c = eng.step(t, Observation{t, static_cast<int64_t>(std::llround(seen)), true},
                           GroupEvidence{});
      if (c.frames != 0 && !live) { pid = c.correction_id; pf = c.frames; land = t + 350000; live = true; }
      plant.advance(tick, c.rate_ppm);
      // WHILE it unwinds, not after: measure from 30 s (past the initial acquisition) to 400 s,
      // which is where the crystal is still wrong and corrections are still firing. Sampling the
      // last 300 s instead measures a settled loop that has stopped correcting, so no blind
      // window ever opens and the property cannot fail -- it passed against the blind-hold build
      // that regressed the bench 9x.
      if (t > 30000000 && t < 400000000) errs.push_back(std::fabs(plant.error_us));
    }
    std::sort(errs.begin(), errs.end());
    const double med = errs[errs.size() / 2];
    const double p90 = errs[static_cast<size_t>(0.9 * errs.size())];
    printf("        crystal %.1f -> %.1f ppm; |error| while unwinding: median %.0f us, p90 %.0f us\n",
           60.0, eng.crystal_ppm(), med, p90);
    check(std::fabs(eng.crystal_ppm()) < 30.0f, "the wrong crystal is unlearned",
          eng.crystal_ppm(), 0.0);
    // 150 us is what this design ACHIEVES here, not what it should: a regression guard, not a
    // target. The bound is set by how fast the crystal can unwind, and the crystal only moves via
    // position credit -- capped at CREDIT_MAX_PPM_PER_WINDOW over CREDIT_BASELINE_HORIZONS, so
    // 60 ppm takes ~6 minutes. There is no true integral in the fine regime: the proportional
    // term is capped at the noise budget (6.7 ppm at a 3 s horizon) and cannot null a 60 ppm
    // plant error on its own. Board a booted at +112 ppm for exactly this reason. Compensation
    // scores 100 us here against the blind hold's 482 us, which is the 4.8x that matches the
    // bench's 25 us vs 224 us -- but neither is 20 us, and closing that is the next change.
    check(med < 150.0, "position error stays bounded while it unwinds", med, 150.0);
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all properties hold", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
