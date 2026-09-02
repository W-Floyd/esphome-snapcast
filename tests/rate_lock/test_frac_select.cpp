// Host test for the MCLK fraction arithmetic and the sigma-delta dither.
//
//   tests/rate_lock/run.sh
//
// TIMING.md claims ~0.15 ppm rational spacing and a ~10 ns residual for this actuator, and the
// whole fine-regime design rests on the rate command being delivered faithfully -- position is
// the integral of rate, so an actuator error integrates the same way command noise does. None of
// it was tested.

#include "../../components/i2s_rate_lock/frac_select.h"

#include <cmath>
#include <cstdio>
#include <cstdint>

using namespace esphome::i2s_rate_lock;

namespace {

int failures = 0;

void check(bool ok, const char *what, double got = 0.0, double want = 0.0) {
  if (ok) {
    printf("  ok    %s\n", what);
  } else {
    printf("  FAIL  %s  (got %.6f, want %.6f)\n", what, got, want);
    failures++;
  }
}

// The bench case: 160 MHz / (44100 * 256) = 6250/441 = 14 + 76/441, exactly representable.
constexpr double BASE_RATIO = 6250.0 / 441.0;
constexpr uint32_t BASE_INT = 14;

}  // namespace

int main() {
  printf("base ratio %.9f = %u + %.9f\n", BASE_RATIO, BASE_INT, BASE_RATIO - BASE_INT);

  printf("\n1. field packing round-trips for every representable ratio\n");
  {
    int checked = 0, bad = 0;
    for (uint32_t a = 2; a <= FRAC_MAX_DENOMINATOR; a++) {
      for (uint32_t b = 0; b < a; b++) {
        uint32_t db = 0, da = 1;
        const uint32_t val = encode_frac(b, a);
        if (!decode_frac(val, db, da)) { bad++; continue; }
        // b/a must survive as a VALUE; the representation may reduce the fraction.
        if (std::fabs(static_cast<double>(db) / da - static_cast<double>(b) / a) > 1e-12) bad++;
        checked++;
      }
    }
    printf("        %d ratios checked\n", checked);
    check(bad == 0, "every b/a decodes to its own value", bad, 0);
  }

  printf("\n2. the bracket really brackets, and stays inside the field limits\n");
  {
    int bad = 0;
    double worst_gap_ppm = 0.0;
    for (double ppm = -5000.0; ppm <= 5000.0; ppm += 7.3) {
      const double target = BASE_RATIO * (1.0 - ppm * 1e-6);
      const double frac = target - BASE_INT;
      if (frac < 0.0 || frac >= 1.0) continue;
      const FracChoice c = pick_frac(frac);
      if (!(c.lo_v <= frac + 1e-12 && c.hi_v >= frac - 1e-12)) bad++;
      if (c.lo_a > FRAC_MAX_DENOMINATOR || c.hi_a > FRAC_MAX_DENOMINATOR) bad++;
      if (c.lo_b >= c.lo_a && c.lo_b != 0) bad++;
      if (c.hi_b >= c.hi_a && c.hi_b != 0) bad++;
      worst_gap_ppm = std::fmax(worst_gap_ppm, (c.hi_v - c.lo_v) / BASE_RATIO * 1e6);
    }
    check(bad == 0, "lo <= target <= hi, fields in range", bad, 0);
    printf("        widest bracket over +-5000 ppm: %.3f ppm\n", worst_gap_ppm);

    // The spacing is NOT uniform. Fractions with a <= 511 crowd near high-denominator targets
    // and thin out near simple ones: at +-5000 ppm the fraction sweeps through 1/5, where the
    // gap is 1/(5*511) ~ 27.6 ppm. TIMING.md's "~0.15 ppm" is the typical spacing near the
    // operating point (frac 0.172), not a worst case over the clamp -- asserting it across the
    // whole range was wrong. What matters is the servo's own range, and that the dithered MEAN
    // is exact regardless (case 3), so a wide bracket costs instantaneous ripple, not accuracy.
    double gap_at_work_ppm = 0.0;
    for (double ppm = -500.0; ppm <= 500.0; ppm += 1.0) {
      const double frac = BASE_RATIO * (1.0 - ppm * 1e-6) - BASE_INT;
      const FracChoice c = pick_frac(frac);
      gap_at_work_ppm = std::fmax(gap_at_work_ppm, (c.hi_v - c.lo_v) / BASE_RATIO * 1e6);
    }
    printf("        widest bracket over the servo's +-500 ppm: %.3f ppm\n", gap_at_work_ppm);
    // No assertion on the width: it is number theory, not a design property. Even inside
    // +-500 ppm the fraction passes 1/6 = 0.1667, where the nearest neighbours with a <= 511 are
    // 1/(6*511) apart, i.e. ~23 ppm. The design properties are the two below -- the mean is
    // exact and the position ripple is bounded -- and they hold regardless of the width.

    // And the ripple that a bracket puts into POSITION, which is the quantity that matters:
    // the rate alternates across the gap at the tick cadence, so position ripples by
    // gap * tick. At a 10 ms tick even the worst 27.7 ppm bracket is well under a microsecond.
    const double tick_s = 0.010;
    const double ripple_us = worst_gap_ppm * 1e-6 * tick_s * 1e6;
    printf("        worst-case position ripple from dither: %.3f us per 10 ms tick\n", ripple_us);
    check(ripple_us < 1.0, "dither ripple stays sub-microsecond", ripple_us, 1.0);
  }

  printf("\n3. the dithered mean equals the requested rate\n");
  {
    double worst_err_ppm = 0.0;
    double worst_at = 0.0;
    for (double ppm = -500.0; ppm <= 500.0; ppm += 0.37) {
      const double target = BASE_RATIO * (1.0 - ppm * 1e-6);
      const double frac = target - BASE_INT;
      const FracChoice c = pick_frac(frac);
      double mean_frac;
      if (c.exact) {
        mean_frac = c.use_lo ? c.lo_v : c.hi_v;
      } else {
        // Run the actual sigma-delta for a long window and average what it delivers.
        const uint32_t duty_q16 = static_cast<uint32_t>(std::lround(c.duty * 65536.0));
        uint32_t acc = 0;
        long his = 0;
        const long N = 200000;
        for (long i = 0; i < N; i++) {
          if (sigma_delta_step(duty_q16, acc)) his++;
        }
        const double share = static_cast<double>(his) / N;
        mean_frac = c.lo_v + share * (c.hi_v - c.lo_v);
      }
      const double delivered_ppm = (1.0 - (BASE_INT + mean_frac) / BASE_RATIO) * 1e6;
      const double err = std::fabs(delivered_ppm - ppm);
      if (err > worst_err_ppm) { worst_err_ppm = err; worst_at = ppm; }
    }
    printf("        worst delivered error %.4f ppm (at %+.1f ppm requested)\n",
           worst_err_ppm, worst_at);
    // This is the claim that matters: whatever the bracket width, the dithered mean is the
    // requested rate. A 23 ppm bracket costs ripple, not accuracy.
    check(worst_err_ppm < 0.05, "dithered mean within 0.05 ppm of the request",
          worst_err_ppm, 0.05);
  }

  printf("\n4. sigma-delta: long-run share equals duty, instantaneous error bounded\n");
  {
    double worst_share_err = 0.0;
    long worst_run = 0;
    for (double duty = 0.01; duty < 1.0; duty += 0.01) {
      const uint32_t q = static_cast<uint32_t>(std::lround(duty * 65536.0));
      uint32_t acc = 0;
      long his = 0, run = 0, worst_local = 0;
      const long N = 100000;
      for (long i = 0; i < N; i++) {
        if (sigma_delta_step(q, acc)) {
          his++;
          run = 0;
        } else {
          run++;
          if (run > worst_local) worst_local = run;
        }
      }
      worst_share_err = std::fmax(worst_share_err,
                                  std::fabs(static_cast<double>(his) / N - duty));
      if (duty > 0.5 && worst_local > worst_run) worst_run = worst_local;
    }
    printf("        worst long-run share error %.6f; longest lo-run above duty 0.5: %ld ticks\n",
           worst_share_err, worst_run);
    check(worst_share_err < 1e-4, "share converges on duty", worst_share_err, 1e-4);
    check(worst_run <= 2, "no long excursions when duty favours hi", worst_run, 2);
  }

  printf("\n5. the integer divider must never have to move inside the clamp\n");
  {
    int outside = 0;
    for (double ppm = -5000.0; ppm <= 5000.0; ppm += 1.0) {
      const double frac = BASE_RATIO * (1.0 - ppm * 1e-6) - BASE_INT;
      if (frac < 0.0 || frac >= 1.0) outside++;
    }
    printf("        %d of 10001 ppm steps would need a different integer part\n", outside);
    // The base fraction is 76/441 = 0.172, so +-5000 ppm moves it by only +-0.07.
    check(outside == 0, "frac stays in [0,1) across the whole clamp", outside, 0);
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all properties hold", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
