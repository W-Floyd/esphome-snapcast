#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace esphome::clock_sync {

/// Reweighting constant and scale floor for the consensus mean. Extracted from tsf_sync.cpp so the
/// estimator's properties can be tested on the host; tsf_sync.cpp is the only production caller.
static constexpr double CONSENSUS_REWEIGHT_K = 2.0;
static constexpr double CONSENSUS_SCALE_FLOOR_US = 50.0;

/// @brief Mean with Huber-style reweighting: a broken peer is down-weighted, not excluded.
///
/// NOT GAUGE-INVARIANT, and the consensus design's correctness argument assumes it is. A plain mean
/// is: evaluating every line at a different reference instant shifts them all by mean(drift)*dref,
/// which factors out. This does not, because the WEIGHTS depend on each line's deviation from the
/// mean, and when the lines' drifts differ their spread grows with the reference instant. Two
/// devices holding IDENTICAL sets therefore adopt slightly different mappings if they evaluate at
/// different ref_tsf -- which they always do, since each uses its own pub_tsf_base_.
///
/// Bounded rather than removed: at the 1 Hz beacon rate the references are ~1 s apart and the
/// disagreement is ~1.3 us (tests/timebase, group 9). It reaches 39 us at 60 s of reference skew
/// and 148 us at an hour, so it matters only if a device's published base goes stale. Documented
/// because the design states the opposite, and a future change to beacon cadence or to the
/// reference choice would silently scale it.
inline double robust_mean(const double *vals, size_t n, double scale_floor) {
  double sum = 0.0;
  for (size_t i = 0; i < n; i++) {
    sum += vals[i];
  }
  const double mean0 = sum / static_cast<double>(n);
  if (n < 3) {
    return mean0;  // with two values the weights are equal by construction; skip the arithmetic
  }
  double dev = 0.0;
  for (size_t i = 0; i < n; i++) {
    dev += std::fabs(vals[i] - mean0);
  }
  const double scale = std::max(dev / static_cast<double>(n), scale_floor) * CONSENSUS_REWEIGHT_K;
  double wsum = 0.0, wvsum = 0.0;
  for (size_t i = 0; i < n; i++) {
    const double d = (vals[i] - mean0) / scale;
    const double w = 1.0 / (1.0 + d * d);
    wsum += w;
    wvsum += w * vals[i];
  }
  return wvsum / wsum;
}

}  // namespace esphome::clock_sync
