// Host test for the chunk deadline's timebase rule and the filter state that makes it reachable.
// No ESPHome, no hardware. See tests/timebase/run.sh.
//
// The bug this pins: chunk_deadline() used to be written inline as `offset = has_estimate() ?
// get_offset(...) : 0.0`, so a board with no timebase got a deadline built on a zero offset --
// server time compared against the local clock with no domain conversion. It is not a small error;
// it is the entire difference between the two domains.

#include "../../components/snapclient/chunk_deadline.h"
#include "../../components/clock_sync/time_filter.h"
#include "../../components/clock_sync/consensus_math.h"

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <random>
#include <algorithm>

using esphome::clock_sync::KalmanTimeFilter;
using esphome::snapclient::chunk_deadline;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
  printf(ok ? "  ok    %s\n" : "  FAIL  %s\n", what);
  if (!ok) {
    failures++;
  }
}

// The bench observer's numbers, 2026-09-02.
constexpr int64_t BUFFER_US = 1000000;    // 1 s of playout buffer
constexpr int64_t SERVER_TS_US = 252000;  // server timestamp, early in the server's life
constexpr int64_t LOCAL_NOW_US = 162336317156;  // 45.09 h of local/AP uptime
constexpr int64_t TRUE_OFFSET_US = SERVER_TS_US - LOCAL_NOW_US;  // server - local

}  // namespace

int main() {
  std::mt19937 rng(11);                              // fixed seed: these properties must not flake
  std::normal_distribution<double> noise(0.0, 0.5);  // 0.5 ms of round-trip network noise

  printf("\n1. no timebase must not produce a deadline\n");
  {
    int64_t deadline = INT64_MIN;
    const bool ok = chunk_deadline(SERVER_TS_US, BUFFER_US, /*have_offset=*/false, 0, 0, deadline);
    check(!ok, "reports false when no offset source exists");
    check(deadline == INT64_MIN, "and leaves the caller's deadline untouched");
  }

  printf("\n2. the magnitude of answering with zero instead\n");
  {
    // What the old code did: have_offset was never consulted, offset was 0.
    const int64_t as_if_zero = SERVER_TS_US + BUFFER_US - 0;
    const int64_t error_us = as_if_zero - LOCAL_NOW_US;
    printf("        deadline error if absent were treated as zero: %lld us (%.1f h)\n",
           static_cast<long long>(error_us), static_cast<double>(error_us) / 3.6e9);
    check(std::llabs(error_us) > 100000000LL,
          "a zero offset misplaces the deadline by the whole clock domain, not by a little");
    // The error is -(local_now - server_ts - buffer), so it GROWS WITH UPTIME and has no bound:
    // the same board an hour later is an hour further wrong. That is why the observer's median
    // sat at -45 h (its own AP uptime) and stayed roughly constant while it was up, rather than
    // looking like a drift. Checked at two uptimes rather than pinned to one bench number.
    const int64_t later_now = LOCAL_NOW_US + 3600000000LL;  // one hour later
    const int64_t later_error = (SERVER_TS_US + BUFFER_US) - later_now;
    check(later_error - error_us == -3600000000LL, "and it grows one-for-one with local uptime, unbounded");
  }

  printf("\n3. a real timebase still produces the documented deadline\n");
  {
    int64_t deadline = 0;
    const bool ok = chunk_deadline(SERVER_TS_US, BUFFER_US, /*have_offset=*/true, TRUE_OFFSET_US, 0, deadline);
    check(ok, "reports true when an offset exists");
    check(deadline == SERVER_TS_US + BUFFER_US - TRUE_OFFSET_US, "deadline = server_ts + buffer - offset");
    // With the true offset the deadline lands one buffer ahead of now, which is the whole point.
    check(std::llabs((deadline - LOCAL_NOW_US) - BUFFER_US) < 1000, "and lands one buffer ahead of local now");
  }

  printf("\n4. the render bias applies, and only where it is meaningful\n");
  {
    int64_t with_bias = 0, without = 0;
    chunk_deadline(SERVER_TS_US, BUFFER_US, true, TRUE_OFFSET_US, 250, with_bias);
    chunk_deadline(SERVER_TS_US, BUFFER_US, true, TRUE_OFFSET_US, 0, without);
    check(with_bias - without == 250, "bias shifts the deadline by exactly its own value");
  }

  printf("\n5. the reconnect window that makes case 1 reachable\n");
  {
    KalmanTimeFilter f;
    check(!f.has_estimate(), "a fresh filter has no estimate");
    f.insert(12.0, 1000.0);
    check(f.has_estimate(), "one sample seeds it");
    // connect_socket_() calls reset() on EVERY reconnect: the window reopens every time.
    f.reset();
    check(!f.has_estimate(), "reset() reopens the no-estimate window (every reconnect does this)");
    f.insert(12.0, 2000.0);
    check(f.has_estimate(), "and the first Time reply after the reconnect closes it again");
  }

  // The bound tsf_sync applies to a published drift. Mirrored here (the real one lives in
  // tsf_sync.cpp, which needs ESPHome) so the THRESHOLD's behaviour against real filter output is
  // pinned: what matters is which values it admits, and those come from the filter below.
  constexpr double MAX_PLAUSIBLE_DRIFT_PPM = 200.0;
  const auto published = [&](double kalman_ppm, double tsf_rate_ppm) {
    return tsf_rate_ppm - (std::fabs(kalman_ppm) <= MAX_PLAUSIBLE_DRIFT_PPM ? kalman_ppm : 0.0);
  };

  printf("\n6. a healthy board's drift is never significant, so the bound costs it nothing\n");
  {
    KalmanTimeFilter f;
    double t = 0.0, off = 12.0;
    for (int i = 0; i < 2000; i++) {
      t += 1000.0;
      off += 43e-6 * 1000.0;  // a real +43 ppm crystal
      f.insert(off + noise(rng), t);
    }
    const double ppm = f.get_drift() * 1e6;
    check(ppm == 0.0, "2000 clean samples at +43 ppm never clear the significance gate");
    check(published(ppm, 43.0) == 43.0, "so the bound changes nothing for a healthy board");
  }

  printf("\n7. an offset step produces an implausible drift, and the bound rejects it\n");
  {
    for (double step : {30.0, 180.0}) {
      KalmanTimeFilter f;
      double t = 0.0, off = 12.0;
      for (int i = 0; i < 300; i++) {
        t += 1000.0;
        off += 43e-6 * 1000.0;
        f.insert(off + noise(rng), t);
      }
      off += step;  // reconnect / server restart / stream change
      t += 1000.0;
      f.insert(off + noise(rng), t);
      double peak = 0.0;
      for (int i = 0; i < 60; i++) {
        t += 1000.0;
        off += 43e-6 * 1000.0;
        f.insert(off + noise(rng), t);
        const double d = f.get_drift() * 1e6;
        if (std::fabs(d) > std::fabs(peak)) {
          peak = d;
        }
      }
      printf("        %5.0f ms step -> peak published drift %+9.1f ppm\n", step, peak);
      check(std::fabs(peak) > MAX_PLAUSIBLE_DRIFT_PPM, "the step clears the gate that real drift never does");
      check(published(peak, 43.0) == 43.0, "and the bound drops it to the tsf rate alone, not a clamp");
    }
  }

  printf("\n8. the bound admits the whole physical range\n");
  {
    check(published(100.0, 43.0) == 43.0 - 100.0, "a 100 ppm kalman drift is kept");
    check(published(-200.0, 43.0) == 43.0 + 200.0, "so is one exactly at the limit");
    check(published(201.0, 43.0) == 43.0, "just past the limit is dropped, not clamped to 200");
  }

  printf("\n9. robust_mean is reference-dependent, so the anchor must come from the SET\n");
  {
    // Three boards' published lines, the crystals measured on the bench 2026-09-02, beacons ~1 s
    // apart. Each device evaluates every line at ITS OWN pub_tsf_base_, so the reference instant
    // differs between them -- which the design says is free ("ref_tsf is free: it sets the anchor
    // the line is stored against, not the line"). That holds for a plain mean. robust_mean's
    // weights depend on the spread between lines, and the spread grows with the reference when the
    // drifts differ, so it does NOT hold here.
    struct Line {
      int64_t tsf_base;
      int64_t tms_base;
      double drift_ppm;
    };
    const Line lines[3] = {
        {162000000000LL, 1165100000LL, 46.3},
        {162000500000LL, 1165100200LL, 42.2},
        {162001000000LL, 1165099800LL, 37.9},
    };
    const auto adopt_at = [&](int64_t ref) {
      double dv[3], base = 0.0;
      for (size_t i = 0; i < 3; i++) {
        const double tms =
            static_cast<double>(lines[i].tms_base) +
            lines[i].drift_ppm * 1e-6 * static_cast<double>(ref - lines[i].tsf_base);
        if (i == 0) {
          base = tms;
        }
        dv[i] = tms - base;
      }
      return base + esphome::clock_sync::robust_mean(dv, 3, esphome::clock_sync::CONSENSUS_SCALE_FLOOR_US);
    };
    double dsum = 0.0;
    for (const Line &l : lines) {
      dsum += l.drift_ppm;
    }
    const int64_t ref0 = 162000000000LL;
    const double a0 = adopt_at(ref0);
    const auto disagreement = [&](int64_t skew) {
      // Remove the part that SHOULD change: the group's own drift carrying the line forward.
      return (adopt_at(ref0 + skew) - a0) - (dsum / 3.0) * 1e-6 * static_cast<double>(skew);
    };
    const double d1s = disagreement(1000000LL);
    const double d60s = disagreement(60000000LL);
    const double d1h = disagreement(3600000000LL);
    printf("        one device, reference skewed:  1 s %+.2f us   60 s %+.2f us   1 h %+.2f us\n", d1s, d60s, d1h);
    check(std::fabs(d1s) > 0.1, "the estimator is reference-dependent (robust_mean is not linear)");
    check(std::fabs(d60s) > std::fabs(d1s), "and the dependence grows with reference skew");

    // THE PROPERTY THAT MATTERS is not reference-independence, it is that every device computes
    // the SAME mapping -- that is what makes the error common-mode and cancel on the wire.
    //
    // BROKEN: each device anchors on its own published base, so no two evaluate at one instant.
    double own[3];
    for (size_t i = 0; i < 3; i++) {
      own[i] = adopt_at(lines[i].tsf_base);
    }
    const double own_spread = *std::max_element(own, own + 3) - *std::min_element(own, own + 3);
    // FIXED: anchor on the freshest base in the set. tsf_base is a transmitted field, so `max`
    // over the same set is the same number on every device.
    int64_t canonical = lines[0].tsf_base;
    for (const Line &l : lines) {
      if (l.tsf_base > canonical) {
        canonical = l.tsf_base;
      }
    }
    double fixed[3];
    for (size_t i = 0; i < 3; i++) {
      fixed[i] = adopt_at(canonical);
    }
    const double fixed_spread = *std::max_element(fixed, fixed + 3) - *std::min_element(fixed, fixed + 3);
    printf("        spread ACROSS devices: own-base anchor %.3f us -> set-derived anchor %.3f us\n", own_spread,
           fixed_spread);
    check(own_spread > 10.0, "anchoring on our own base disagrees by tens of us with identical sets");
    check(fixed_spread == 0.0, "anchoring on the freshest base in the set agrees EXACTLY");
    // Equal drifts is the case where gauge invariance DOES hold: the spread stops moving with ref.
    const Line flat[3] = {
        {162000000000LL, 1165100000LL, 42.0},
        {162000500000LL, 1165100200LL, 42.0},
        {162001000000LL, 1165099800LL, 42.0},
    };
    const auto adopt_flat = [&](int64_t ref) {
      double dv[3], base = 0.0;
      for (size_t i = 0; i < 3; i++) {
        const double tms = static_cast<double>(flat[i].tms_base) +
                           flat[i].drift_ppm * 1e-6 * static_cast<double>(ref - flat[i].tsf_base);
        if (i == 0) {
          base = tms;
        }
        dv[i] = tms - base;
      }
      return base + esphome::clock_sync::robust_mean(dv, 3, esphome::clock_sync::CONSENSUS_SCALE_FLOOR_US);
    };
    const double flat_skew =
        (adopt_flat(ref0 + 3600000000LL) - adopt_flat(ref0)) - 42.0 * 1e-6 * 3600000000.0;
    check(std::fabs(flat_skew) < 0.5, "with EQUAL drifts it is gauge-invariant, pinning drift spread as the cause");
  }

  printf(failures == 0 ? "\nall properties hold (0 failures)\n" : "\n%d FAILURES\n", failures);
  return failures == 0 ? 0 : 1;
}
