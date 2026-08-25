#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_CLOCK_SYNC_RATE_LOCK)

#include <atomic>
#include <cstdint>

namespace esphome::clock_sync {

/// @brief Hardware sample-rate steering ("rate lock"): trims the I2S output clock by
/// tens of ppm so the sync servo can hold lock with zero waveform splices.
///
/// The interface is chip-agnostic; backends are selected per SoC in rate_lock.cpp:
///  - ESP32-S3: steers the I2S MCLK fractional-N divider (this SoC has no APLL).
///    MCLK = SRC / (N + b/a); with a <= 511, adjacent ratios sit ~0.15 ppm apart.
///  - Other SoCs: stub reporting unavailable, which keeps the caller's frame-splice
///    servo in charge. ESP32-classic could later get an APLL backend here behind the
///    same three methods.
///
/// The baseline divider is read back from what the I2S driver programmed, so this
/// works with whatever rate/mclk_multiple the speaker chose and needs no channel
/// handle -- only the port number.
///
/// THREAD CONTEXT: set_trim_ppm() and applied_ppm() are player-task-only;
/// invalidate_baseline() may be called from any thread.
class RateLock {
 public:
  explicit RateLock(uint8_t i2s_port) : port_(i2s_port) {}

  /// @brief Steers the playback rate by @p ppm (positive = play faster), clamped to
  /// +-500 ppm. Re-reads the driver-programmed divider as the baseline on first use
  /// and after invalidate_baseline().
  /// @return false when steering is unavailable: unsupported SoC, the speaker has
  /// not started (I2S clock inactive), the register round-trip sanity check failed,
  /// or the requested trim would need a different integer divider. The caller must
  /// fall back to splice corrections.
  bool set_trim_ppm(float ppm);

  /// @brief Forgets the baseline: the I2S driver may have reprogrammed the clock
  /// (speaker stop/start, sample-rate change). The next set_trim_ppm() re-reads the
  /// registers and re-applies the full requested trim on top of the new baseline.
  void invalidate_baseline() { this->rebaseline_.store(true, std::memory_order_relaxed); }

  /// @brief Trim actually achieved by the last successful set_trim_ppm(), after
  /// rational quantization (~0.15 ppm steps). Diagnostics only.
  float applied_ppm() const { return this->applied_ppm_; }

  /// @brief Tells the lock which output rate the speaker is running, so the baseline
  /// can be corrected to the exact ideal divider instead of inheriting the I2S
  /// driver's rational approximation of it -- an error of up to ~500 ppm, which the
  /// servo would otherwise have to cancel out of its own authority. Call before the
  /// first trim and whenever the rate changes (alongside invalidate_baseline()).
  /// 0 disables the correction.
  void set_output_rate(uint32_t sample_rate) { this->output_rate_ = sample_rate; }

  /// @brief How far the driver's baseline was off the ideal, ppm; 0 when no
  /// correction was applied. Positive = driver was running slow. Diagnostics.
  float baseline_corrected_ppm() const { return this->baseline_corrected_ppm_; }

 protected:
  bool read_baseline_();

  uint8_t port_;
  std::atomic<bool> rebaseline_{true};
  bool baseline_valid_{false};
  float applied_ppm_{0.0f};

  // Baseline divider as programmed by the I2S driver: MCLK = SRC / (int + num/den)
  uint32_t base_int_{0};
  uint32_t base_num_{0};
  uint32_t base_den_{1};
  // Reference ratio trims are measured against: the exact ideal where it could be
  // established, otherwise the driver's approximation above (legacy behaviour)
  double base_ratio_{0.0};
  uint32_t output_rate_{0};
  float baseline_corrected_ppm_{0.0f};
  // Last written fractional-field register value, to skip redundant writes -- and to
  // recognise our own trim on a re-baseline, so it is not mistaken for a divider the
  // driver chose. ours_valid_ says last_frac_val_ actually came from us.
  uint32_t last_frac_val_{0};
  bool ours_valid_{false};
};

}  // namespace esphome::clock_sync

#endif  // USE_ESP32 && USE_CLOCK_SYNC_RATE_LOCK
