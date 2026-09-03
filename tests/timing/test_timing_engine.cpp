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
  /// Rate of large one-sample excursions, per observation. The bench sees transients of tens of
  /// milliseconds on an otherwise quiet run (census: min -26 ms, max +50 ms), from resyncs,
  /// membership changes and starved chunks. A Gaussian-only model has none, which is what let an
  /// integrator fed raw error reach the 200 ppm clamp on hardware while every test here passed.
  double transient_rate = 0.0;
  double transient_us = 30000.0;

  double observe() {
    double v = error_us;
    if (noise_us > 0.0) {
      std::normal_distribution<double> d(0.0, noise_us);
      v += d(rng);
    }
    if (transient_rate > 0.0) {
      std::uniform_real_distribution<double> u(0.0, 1.0);
      if (u(rng) < transient_rate) {
        std::uniform_real_distribution<double> sgn(-1.0, 1.0);
        v += (sgn(rng) < 0.0 ? -1.0 : 1.0) * transient_us;
      }
    }
    return v;
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
  // no fixed pipe constant: the correction reaches the DAC at position_delay_us, and is then
  // measured measurement_lag_us later. Their sum is exactly compensation_us().
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

    // Record the truth, then report it observation_delay_us late -- the TRANSPORT delay. That is
    // shorter than visibility_us, which also carries the error filter's own lag. Having the two
    // equal here is why over-compensation could not be expressed: the measurement became correct
    // at the same instant compensation stopped.
    history.emplace_back(t, plant.observe());
    double seen = history.front().second;
    while (history.size() > 1 && history.front().first <= t - p.measurement_lag_us) {
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
      pend = {cmd.correction_id, cmd.frames, t + p.position_delay_us, true};
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
  // The bench's own shape: transport (ring + pipe) is what an observation waits for, and
  // visibility adds the filter's lag on top. Keeping the relationship explicit here means a
  // profile in a test cannot accidentally make the two equal again.
  p.measurement_lag_us = 250000;    // pipeline: rate's dead time
  p.position_delay_us = 1250000;    // ring + pipe: position's, measured on the wire
  // Swept: 88-100 ppm is where frame corrections stop, set by the plant's own +-30-40 ppm
  // wander rather than by the actuator's range. The actuator accepts +-2000, but authority
  // that large lets rate accept errors it then overshoots by A*T through the transport delay.
  p.rate_authority_ppm = 100.0f;
  p.target_diff_us = 20;

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
    // Restated. This used to assert that corrections fire early and then taper, which encoded the
    // old division of labour: position did the coarse work and rate was capped at the noise budget
    // (~5 ppm), so a 30 ppm plant HAD to be paid for in frames. With authority separated from the
    // noise budget, rate absorbs 30 ppm outright and the correct property is that position never
    // moves at all -- a frame correction here would be a shortcut, not a necessity.
    Run late = simulate(p, 30.0, 900.0);
    printf("        30 ppm plant: %d corrections over 900 s, crystal %+.2f ppm, final err %+.1f us\n",
           late.position_corrections, late.final_crystal_ppm, late.final_error_us);
    check(late.position_corrections == 0,
          "a 30 ppm plant is absorbed by rate, with no frame corrections",
          late.position_corrections, 0);
    check(std::fabs(late.final_crystal_ppm - 30.0) < 3.0,
          "and the crystal still learns it", late.final_crystal_ppm, 30.0);
  }

  printf("\n4. a step beyond RATE'S AUTHORITY: delivered once, not repeatedly\n");
  // Swept across the WHOLE visibility range travel_horizon_us_() can produce -- it is clamped to
  // [1 s, 5 s] and varies with ring and pipe depth at runtime, so the property has to hold across
  // it, not at one value. Testing only 1 s hid this: there the stale window is 0.65 s and the
  // filter (tau 2 s) cannot climb back over the coarse gate. At 3 s it can.
  for (int64_t vis_us : {1000000, 2000000, 3000000, 4000000, 5000000}) {
    Profile pv = p;
    pv.position_delay_us = vis_us;
    Engine eng(pv);
    Plant plant;
    plant.frame_us = pv.frame_us();
    // 40 frames (~907 us). Rate's authority is 100 ppm, so removing this inside one horizon needs
    // 907/vis_s ppm -- 181 ppm at the longest horizon tested, 907 at the shortest -- and is out of
    // reach at every one of them, which is what makes a frame correction the right answer here.
    // The old 3-frame (68 us) case now needs 14-68 ppm and is absorbed by rate with ZERO
    // corrections; it is kept as its own check below.
    plant.error_us = 40.0 * static_cast<double>(pv.frame_us());
    int corrections = 0;
    uint64_t pend_id = 0;
    int32_t pend_frames = 0;
    bool live = false;
    int64_t land = 0;
    // Same two-timescale model as simulate(): the plant moves at the pipeline depth, the
    // measurement follows a visibility horizon later. Landing and visibility were one time here
    // too, which is why this property passed while the bench re-issued.
    std::deque<std::pair<int64_t, double>> history;
    // Run for a multiple of the profile's own settling time, not a fixed 20 s: with the position
    // delay swept to 5 s, a fixed window measures the residual before rate has finished cleaning
    // up after the step and reports 1-2 frames of "failure" that is just impatience.
    const int64_t run_us = std::max<int64_t>(20000000, 12 * pv.settle_us());
    for (int64_t t = 0; t < run_us; t += 25000) {
      if (live && t >= land) {
        plant.apply_frames(pend_frames);
        eng.confirm_position_landed(pend_id, t);
        live = false;
      }
      history.emplace_back(t, plant.error_us);
      double seen = history.front().second;
      while (history.size() > 1 && history.front().first <= t - pv.measurement_lag_us) {
        seen = history.front().second;
        history.pop_front();
      }
      Observation obs{t, static_cast<int64_t>(std::llround(seen)), true};
      Command cmd = eng.step(t, obs, GroupEvidence{});
      if (cmd.frames != 0) {
        corrections++;
        pend_id = cmd.correction_id;
        pend_frames = cmd.frames;
        land = t + pv.position_delay_us;
        live = true;
      }
      plant.advance(25000, cmd.rate_ppm);
    }
    printf("        position delay %lld ms: %d corrections, residual %+.1f us\n",
           static_cast<long long>(vis_us / 1000), corrections, plant.error_us);
    check(corrections >= 1, "a step out of rate's reach IS delivered", corrections, 1);
    check(corrections <= 2, "and costs at most 2 corrections", corrections, 2);
    check(std::fabs(plant.error_us) < static_cast<double>(pv.frame_us()),
          "settles inside one frame", plant.error_us, 0.0);
  }

  printf("\n4b. and a step WITHIN rate's authority costs no frames at all\n");
  {
    // The complement of case 4, and the property the loop was failing on the bench: a few frames
    // of error is rate's job. Measured there as a correction every 4.0 s -- the entire
    // serialisation window -- at 8-9 frames each, while P contributed 0.4 ppm.
    Profile ps = p;
    ps.position_delay_us = 2000000;
    Run r = simulate(ps, 0.0, 600.0, 80.0);
    printf("        3-frame-scale error, 80 us noise: %d corrections over 600 s\n",
           r.position_corrections);
    check(r.position_corrections == 0, "no frame corrections within rate's reach",
          r.position_corrections, 0);
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
      if (c.frames != 0) { pid = c.correction_id; pf = c.frames; land = t + p.settle_us(); live = true; }
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
    Profile pw = p;   // bench: ring 1724 ms + pipe 247 ms
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
      while (history.size() > 1 && history.front().first <= t - pw.measurement_lag_us) {
        seen = history.front().second;
        history.pop_front();
      }
      Command c = eng.step(t, Observation{t, static_cast<int64_t>(std::llround(seen)), true},
                           GroupEvidence{});
      if (c.frames != 0 && !live) { pid = c.correction_id; pf = c.frames; land = t + pw.position_delay_us; live = true; }
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
    // Bounded in FRAMES, not microseconds. One frame is the quantum the position actuator works
    // in, so "a few frames off while the rate estimate is badly wrong" is the honest statement of
    // the property; an absolute microsecond figure just tracks whatever visibility_us the profile
    // happens to carry, and I relaxed it twice for that reason before saying so.
    //
    // The cost is a deliberate trade. Bounding the integral's input to one frame (test 12) caps
    // its slew at wn^2 * frame_us, so a wrong crystal unwinds slower; the unbounded version is
    // the one that railed both boards to 200 ppm on hardware. A wrong stored crystal is a one-off
    // at boot, measurement transients are continuous, so transient immunity is worth more.
    // This case remains the worst thing the loop does: ~112 us for the ~6 minutes after a boot
    // that restored a bad estimate, against 0.4 us settled (test 11).
    const double bound_us = 6.0 * static_cast<double>(pw.frame_us());
    check(med < bound_us, "position error stays within a few frames while it unwinds", med,
          bound_us);
  }

  printf("\n11. the rate loop must not limit-cycle on a quiet measurement\n");
  {
    // The P term is clamped to the budget, so if Kp is large enough that the clamp is reached
    // inside the error range position cannot cover, P stops being proportional and becomes a
    // relay -- and a relay against a transport delay oscillates. This fired with sigma_e floored
    // at a quarter frame: rate swung -17..+24 ppm on a 20-40 s period with ZERO frame corrections
    // and the crystal never converged. Nothing else in this file caught it, because every other
    // property is satisfied by a loop that oscillates about the right answer.
    Profile pq = p;
    pq.position_delay_us = 1000000;
    Engine eng(pq);
    Plant plant;
    plant.plant_ppm = 3.35;
    plant.frame_us = pq.frame_us();
    std::deque<std::pair<int64_t, double>> history;
    uint64_t pid = 0; int32_t pf = 0; bool live = false; int64_t land = 0;
    std::vector<double> rates, errs;
    const int64_t tick = 25000;
    for (int64_t t = 0; t < 900000000; t += tick) {
      if (live && t >= land) { plant.apply_frames(pf); eng.confirm_position_landed(pid, t); live = false; }
      history.emplace_back(t, plant.error_us);
      double seen = history.front().second;
      while (history.size() > 1 && history.front().first <= t - pq.measurement_lag_us) {
        seen = history.front().second;
        history.pop_front();
      }
      Command c = eng.step(t, Observation{t, static_cast<int64_t>(std::llround(seen)), true},
                           GroupEvidence{});
      if (c.frames != 0 && !live) { pid = c.correction_id; pf = c.frames; land = t + pq.position_delay_us; live = true; }
      plant.advance(tick, c.rate_ppm);
      if (t > 600000000) { rates.push_back(c.rate_ppm); errs.push_back(std::fabs(plant.error_us)); }
    }
    double m = 0.0; for (double v : rates) m += v; m /= static_cast<double>(rates.size());
    double var = 0.0; for (double v : rates) var += (v - m) * (v - m);
    const double sd = std::sqrt(var / static_cast<double>(rates.size()));
    std::sort(errs.begin(), errs.end());
    printf("        settled: rate mean %+.2f ppm sd %.2f ppm, |error| median %.1f us\n",
           m, sd, errs[errs.size() / 2]);
    check(sd < 1.0, "the rate command settles instead of swinging", sd, 1.0);
    check(errs[errs.size() / 2] < 5.0, "and the error settles with it", errs[errs.size() / 2], 5.0);
  }

  printf("\n12. measurement transients must not wind the crystal\n");
  {
    // Tens-of-ms excursions on an otherwise quiet measurement, which is what the bench actually
    // delivers. The estimate must stay a crystal estimate: a real one is tens of ppm, so reaching
    // the CRYSTAL_LIMIT_PPM clamp means it is tracking something else. Measured on hardware
    // railing both boards to 200 ppm inside one boot, while the host suite was green.
    Profile pt = p;
    pt.position_delay_us = 2000000;
    Engine eng(pt);
    Plant plant;
    plant.plant_ppm = 3.35;
    plant.frame_us = pt.frame_us();
    plant.noise_us = 80.0;
    plant.transient_rate = 0.02;   // ~1 in 50 observations
    plant.transient_us = 30000.0;  // 30 ms, mid-range for what the census shows
    std::deque<std::pair<int64_t, double>> history;
    uint64_t pid = 0; int32_t pf = 0; bool live = false; int64_t land = 0;
    double peak_abs_xtal = 0.0;
    const int64_t tick = 25000;
    for (int64_t t = 0; t < 900000000; t += tick) {
      if (live && t >= land) { plant.apply_frames(pf); eng.confirm_position_landed(pid, t); live = false; }
      history.emplace_back(t, plant.observe());
      double seen = history.front().second;
      while (history.size() > 1 && history.front().first <= t - pt.measurement_lag_us) {
        seen = history.front().second;
        history.pop_front();
      }
      Command c = eng.step(t, Observation{t, static_cast<int64_t>(std::llround(seen)), true},
                           GroupEvidence{});
      if (c.frames != 0 && !live) { pid = c.correction_id; pf = c.frames; land = t + pt.position_delay_us; live = true; }
      plant.advance(tick, c.rate_ppm);
      peak_abs_xtal = std::max(peak_abs_xtal, std::fabs((double) eng.crystal_ppm()));
    }
    printf("        crystal peak |%.1f| ppm, final %+.2f ppm (plant %+.2f)\n",
           peak_abs_xtal, eng.crystal_ppm(), 3.35);
    check(peak_abs_xtal < 100.0, "transients do not wind the estimate toward its clamp",
          peak_abs_xtal, 100.0);
  }

  printf("\n13. a WRONG configured position delay must not inject error into a synced board\n");
  {
    // The bench's actual situation. position_delay comes from buffer accounting -- ring fill from
    // available() and pushed-minus-played disagree 7x, and the latter is rebaselined on feedback
    // gaps -- while the wire says the real landing is ~1.25 s. Compensating past the landing
    // subtracts a displacement the observation already contains, inventing an error of -D. On a
    // board that is ALREADY in sync that is not a missed correction, it is an injected one: the
    // wire showed it stepping away from zero and back, every settle window.
    //
    // So: tell the engine a delay that is wrong in both directions and require it to cope.
    for (double factor : {0.5, 2.0, 4.0}) {
      const int64_t true_land = 1250000;                 // what the wire measured
      Profile pb = p;
      pb.position_delay_us = static_cast<int64_t>(true_land * factor);   // what we believe
      Engine eng(pb);
      Plant plant;
      plant.plant_ppm = 0.0;                             // already synced
      plant.frame_us = pb.frame_us();
      plant.noise_us = 80.0;
      plant.error_us = 26.0 * static_cast<double>(pb.frame_us());  // one real step to correct
      std::deque<std::pair<int64_t, double>> history;
      uint64_t pid = 0; int32_t pf = 0; bool live = false; int64_t land = 0;
      int corr = 0; long gross = 0;
      std::vector<double> errs;
      const int64_t tick = 25000;
      for (int64_t t = 0; t < 600000000; t += tick) {
        if (live && t >= land) { plant.apply_frames(pf); eng.confirm_position_landed(pid, t); live = false; }
        history.emplace_back(t, plant.observe());
        double seen = history.front().second;
        while (history.size() > 1 && history.front().first <= t - pb.measurement_lag_us) {
          seen = history.front().second;
          history.pop_front();
        }
        Command c = eng.step(t, Observation{t, static_cast<int64_t>(std::llround(seen)), true},
                             GroupEvidence{});
        if (c.frames != 0 && !live) {
          pid = c.correction_id; pf = c.frames; land = t + true_land; live = true;
          corr++; gross += std::abs(c.frames);
        }
        plant.advance(tick, c.rate_ppm);
        if (t > 120000000) errs.push_back(std::fabs(plant.error_us));
      }
      std::sort(errs.begin(), errs.end());
      const double med = errs[errs.size() / 2];
      printf("        believed %.1fx the true landing: %d corrections, gross %ld frames, "
             "settled |e| median %.0f us\n", factor, corr, gross, med);
      // One real correction for the real step. More than a handful means the loop is correcting
      // its own phantom, which is the failure this guards.
      check(corr <= 3, "a wrong delay does not buy extra corrections", corr, 3);
      check(med < 3.0 * static_cast<double>(pb.frame_us()),
            "and the board stays synced", med, 3.0 * static_cast<double>(pb.frame_us()));
    }
  }

  printf("\n14. a frame drop is SPENT FROM THE BUFFER, and must not drain it\n");
  {
    // The bench's worst failure, and entirely self-inflicted. Dropping frames removes audio from
    // the ring, so a loop that drops to fix a late error drains the thing that lets it play
    // continuously. Board a dropped 54951 frames -- 1.25 s of audio -- from a 1724 ms buffer,
    // took the ring to 26 ms, and starved. A starved board falls further behind, which reads as a
    // LATER error, which buys more drops: err climbed 3 ms per report while the loop commanded
    // 5584 frames at a time. Nothing in this file modelled a buffer, so nothing could see it.
    //
    // Persistent late bias, which is what keeps the drops coming.
    Profile pn = p;
    pn.position_delay_us = 1250000;
    pn.buffer_floor_us = 1724000 / 8;   // as the client derives it, from ring capacity
    Engine eng(pn);
    const double frame_us = static_cast<double>(pn.frame_us());
    double err = 0.0, buffer_us = 1724000.0;
    // Swept, because the two properties here hold over different ranges and conflating them hid
    // that. The BUFFER must survive any drift, including one past the crystal's own 200 ppm clamp
    // where rate cannot ever null the error and position is asked for corrections for ever. The
    // ERROR can only be bounded where rate can actually answer it. Asserting a 100 us error bound
    // at 150 ppm was asserting the second property in the first property's test.
    for (const double deadline_ppm : {50.0, 150.0, 300.0}) {
    std::mt19937 rng(3);
    std::normal_distribution<double> nd(0.0, 80.0);
    std::deque<std::pair<int64_t, double>> obs;
    std::vector<std::pair<int64_t, int32_t>> inflight;
    uint64_t pid = 0; bool live = false;
    const int64_t tick = 25000;
    double min_buffer = buffer_us, max_abs_err = 0.0;
    long gross = 0;
    for (int64_t t = 0; t < 900000000; t += tick) {
      for (auto it = inflight.begin(); it != inflight.end();) {
        if (t >= it->first) { err -= static_cast<double>(it->second) * frame_us; it = inflight.erase(it); }
        else ++it;
      }
      if (live && inflight.empty()) { eng.confirm_position_landed(pid, t); live = false; }
      const bool starved = buffer_us <= 0.0;
      // Starved: playout stalls while the deadline keeps advancing, so the error grows 1:1.
      err += starved ? static_cast<double>(tick) : deadline_ppm * 1e-6 * static_cast<double>(tick);
      obs.emplace_back(t, err + nd(rng));
      double seen = obs.front().second;
      while (obs.size() > 1 && obs.front().first <= t - pn.measurement_lag_us) {
        seen = obs.front().second;
        obs.pop_front();
      }
      Observation o{t, static_cast<int64_t>(std::llround(seen)), true,
                    static_cast<int64_t>(std::max(0.0, buffer_us))};
      Command c = eng.step(t, o, GroupEvidence{});
      if (c.frames != 0 && !live) {
        pid = c.correction_id;
        inflight.push_back({t + pn.position_delay_us, c.frames});
        live = true;
        gross += std::abs(c.frames);
        buffer_us -= static_cast<double>(c.frames) * frame_us;   // drops SPEND the buffer
      }
      // CLOSE THE LOOP. This was missing, so the rate command had no effect on the plant and the
      // error was simply the integral of the drift: 150 ppm x 900 s = 135 ms, which is exactly the
      // "runaway" the test reported. An open-loop harness cannot distinguish a servo that fails
      // from a servo that is never connected, and it made a broken servo the obvious explanation.
      err += (0.0 - static_cast<double>(c.rate_ppm)) * 1e-6 * static_cast<double>(tick);
      buffer_us = std::min(buffer_us, 1724000.0);
      min_buffer = std::min(min_buffer, buffer_us);
      if (t > 120000000) max_abs_err = std::max(max_abs_err, std::fabs(err));
    }
    printf("        %.0f ppm drift: buffer low-water %.0f ms (started 1724); %ld frames spent; "
           "worst |error| %.0f us; crystal %+.1f ppm\n",
           deadline_ppm, min_buffer / 1000.0, gross, max_abs_err, eng.crystal_ppm());
    // Holds at ANY drift: this is what the test is for.
    check(min_buffer > static_cast<double>(pn.buffer_floor_us),
          "the buffer never starves", min_buffer, static_cast<double>(pn.buffer_floor_us));
    // Only where rate can reach it. Past CRYSTAL_LIMIT_PPM the loop cannot null a permanent
    // drift at all, and a bounded error is not a property it can have.
    if (deadline_ppm < 100.0) {
      check(max_abs_err < 100000.0, "and the error does not run away", max_abs_err, 100000.0);
    }
    // Past CRYSTAL_LIMIT_PPM the integral cannot absorb the offset: it rails, a permanent deficit
    // remains, and rate can no longer HOLD a correction even when needed_ppm looks affordable
    // against authority. Position has to take over, and rate_can_fix has to know that -- checking
    // needed_ppm against authority alone let it stand down for work rate could not finish.
    // Measured at 300 ppm: 4912 frames and 38 ms of worst error with the check, against 792
    // frames and 84 ms without it.
    if (std::fabs(eng.crystal_ppm()) >= 199.0f) {
      check(gross > 1000, "position takes over once the crystal is spent",
            static_cast<double>(gross), 1000.0);
    }
    }
  }

  printf("\n15. an observation GAP must not move the crystal by tens of ppm\n");
  {
    // The storm engine. dt_s in the integral is the time since the last observation, so a gap --
    // a resync, a starvation hold, anything that stops the measurement -- was integrated in ONE
    // step as though the loop had been watching throughout. With the horizon collapsed to its
    // floor (a drained ring, wn^2 = 2.95) a 2 s gap moved the estimate 119 ppm instantly.
    //
    // Measured on board a as single steps of -93.8, -105.7 and -110.5 ppm, ending railed at
    // -200 ppm from a +179 peak. Neither credit (5 ppm/window) nor the steady integral
    // (0.59 ppm/s at a healthy horizon) can do that; an unbounded dt can.
    Profile pg = p;
    pg.measurement_lag_us = 47000;
    pg.position_delay_us = 100000;   // the clamp floor: a fully drained ring
    pg.buffer_floor_us = 200000;
    double worst = 0.0;
    for (int64_t gap_us : {25000, 500000, 2000000, 5000000, 15000000}) {
      Engine eng(pg);
      int64_t t = 0;
      for (int i = 0; i < 20; i++) {
        eng.step(t, Observation{t, 20, true, 1724000}, GroupEvidence{});
        t += 25000;
      }
      const float before = eng.crystal_ppm();
      t += gap_us;
      eng.step(t, Observation{t, 20, true, 1724000}, GroupEvidence{});
      const double step = std::fabs(eng.crystal_ppm() - before);
      worst = std::max(worst, step);
    }
    printf("        worst single-step crystal move across gaps up to 15 s: %.2f ppm\n", worst);
    // A crystal is tens of ppm in total, so no single observation may move the ESTIMATE by an
    // appreciable fraction of the whole range, however long the loop was blind.
    check(worst < 20.0, "one observation cannot slew the estimate", worst, 20.0);
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all properties hold", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
