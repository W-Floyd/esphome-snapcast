#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace esphome::snapclient {

/// @brief 2-state Kalman filter with Sage-Husa M-estimate adaptive measurement noise.
///
/// Estimates the server-clock minus client-clock offset from Snapcast Time message
/// round trips. State vector: [offset (ms), drift (ms/ms = dimensionless rate)].
///
/// Sage-Husa estimates the measurement noise R̂ from the innovation sequence, so no
/// manual outlier threshold is required. The M-estimate (Mohamed & Schwarz 1999)
/// variant applies a Huber weight to each innovation, preventing outlier spikes
/// (wifi retransmit bursts, buffered TCP segments) from corrupting either the state
/// or the R̂ estimate while still allowing the filter to track real offset changes.
///
/// R̂ is intentionally NOT reset on reset() — the learned network characteristics
/// carry over so re-sync after a reconnect converges faster.
///
/// Ported from the TypeScript KalmanTimeFilter in ImmichFrame-snapweb's snapstream.ts,
/// itself derived from esp32 snapclient's timefilter/TimeFilter.c.
///
/// THREAD CONTEXT: not internally synchronized; callers guard with a mutex.
class KalmanTimeFilter {
 public:
  void reset() {
    this->count_ = 0;
    this->offset_ = 0.0;
    this->drift_ = 0.0;
    this->offset_cov_ = std::numeric_limits<double>::infinity();
    this->offset_drift_cov_ = 0.0;
    this->drift_cov_ = 0.0;
    this->last_update_ = 0.0;
    this->use_drift_ = false;
    // r_hat_ intentionally preserved — learned noise carries over to re-sync
  }

  /// @param measurement Offset measurement in ms: (c2s - s2c) / 2.
  /// @param time_added Client monotonic time of the measurement in ms.
  void insert(double measurement, double time_added) {
    if (this->count_ != 0 && time_added <= this->last_update_) {
      return;
    }

    // Cap dt to 5s — prevents drift_cov*dt² explosion after a long stall
    const double dt = std::min(time_added - this->last_update_, 5000.0);
    const double dt2 = dt * dt;
    this->last_update_ = time_added;

    if (this->count_ == 0) {
      this->count_++;
      this->offset_ = measurement;
      this->offset_cov_ = this->r_hat_;
      return;
    }

    if (this->count_ == 1) {
      this->count_++;
      this->drift_ = (measurement - this->offset_) / dt;
      this->offset_ = measurement;
      this->drift_cov_ = (this->offset_cov_ + this->r_hat_) / dt2;
      this->offset_cov_ = this->r_hat_;
      return;
    }

    // Predict: x = F*x, P = F*P*F^T + Q  (F = [1,dt; 0,1])
    const double pred_offset = this->offset_ + this->drift_ * dt;
    const double new_drift_cov = this->drift_cov_ + dt * DRIFT_PROCESS_VAR;
    const double new_offset_drift_cov = this->offset_drift_cov_ + this->drift_cov_ * dt;
    const double new_offset_cov =
        this->offset_cov_ + 2.0 * this->offset_drift_cov_ * dt + this->drift_cov_ * dt2 + dt * PROCESS_VAR;

    // Innovation
    const double residual = measurement - pred_offset;
    const double innov_std = std::sqrt(new_offset_cov + this->r_hat_);

    // Huber M-weight: downweight outliers beyond HUBER_C·σ without rejecting them
    const double norm_resid = std::abs(residual) / innov_std;
    const double weight = norm_resid <= HUBER_C ? 1.0 : HUBER_C / norm_resid;

    // Effective R for this step — inflated for outliers so the gain is automatically reduced
    const double r_effective = this->r_hat_ / weight;

    // Update: K = P*H^T*(H*P*H^T + R_eff)^-1, H = [1,0]
    const double inv_s = 1.0 / (new_offset_cov + r_effective);
    const double k_offset = new_offset_cov * inv_s;
    const double k_drift = new_offset_drift_cov * inv_s;

    this->offset_ = pred_offset + k_offset * residual;
    this->drift_ += k_drift * residual;
    this->drift_cov_ = new_drift_cov - k_drift * new_offset_drift_cov;
    this->offset_drift_cov_ = new_offset_drift_cov - k_drift * new_offset_cov;
    this->offset_cov_ = new_offset_cov - k_offset * new_offset_cov;

    // Sage-Husa M-estimate: update R̂ from the robustified innovation.
    // d = 1 - b is the fading-memory weight (~0.03 at b=0.97)
    if (this->count_ >= MIN_SAMPLES) {
      const double robust_resid = weight * residual;  // ε̃ = Huber-clipped innovation
      const double d = 1.0 - FORGETTING_FACTOR;
      const double r_hat_raw = (1.0 - d) * this->r_hat_ + d * (robust_resid * robust_resid - new_offset_cov);
      this->r_hat_ = std::max(r_hat_raw, R_MIN);
    } else {
      this->count_++;
    }

    // Only extrapolate with drift once it is statistically significant vs its covariance
    this->use_drift_ = this->drift_ * this->drift_ > DRIFT_SIG_THRESH_SQ * this->drift_cov_;
  }

  /// @brief Estimated clock offset (server - client, ms) at the given client time,
  /// extrapolated forward from the last measurement using drift when significant.
  double get_offset(double client_time_ms) const {
    const double dt = client_time_ms - this->last_update_;
    return this->offset_ + (this->use_drift_ ? this->drift_ : 0.0) * dt;
  }

  /// @brief True once at least one measurement has been inserted.
  bool has_estimate() const { return this->count_ > 0; }

  /// @brief True once enough measurements have been absorbed that the estimate is
  /// past its raw initial phase (the first sample sets the offset directly and can
  /// be off by 100+ ms under congestion).
  bool is_settled() const { return this->count_ >= MIN_SAMPLES; }

  /// @brief Drift estimate (offset ms per client ms, dimensionless — equivalently
  /// ppm×1e-6); 0.0 until statistically significant, matching get_offset()'s gating.
  double get_drift() const { return this->use_drift_ ? this->drift_ : 0.0; }

  /// @brief Current adaptive measurement-noise estimate R̂ in ms² (diagnostic).
  double get_r_hat() const { return this->r_hat_; }

 protected:
  // offset process noise (ms/√ms) squared
  static constexpr double PROCESS_VAR = 0.01 * 0.01;
  // drift process noise squared
  static constexpr double DRIFT_PROCESS_VAR = 1e-7 * 1e-7;
  // b ∈ (0.95, 0.99) — gives ~33-sample memory at 1 Hz
  static constexpr double FORGETTING_FACTOR = 0.97;
  // standard Huber constant — 1.345 → 95% Gaussian efficiency
  static constexpr double HUBER_C = 1.345;
  // hard floor on R̂ (ms²) — 0.1 ms std dev floor
  static constexpr double R_MIN = 0.01;
  // samples before R̂ adaptation begins
  static constexpr int MIN_SAMPLES = 5;
  static constexpr double DRIFT_SIG_THRESH_SQ = 2.0 * 2.0;

  int count_{0};
  double offset_{0.0};
  double drift_{0.0};
  double offset_cov_{std::numeric_limits<double>::infinity()};
  double offset_drift_cov_{0.0};
  double drift_cov_{0.0};
  double last_update_{0.0};
  bool use_drift_{false};

  // Sage-Husa adaptive measurement noise estimate (ms²).
  // Initial 25 ms² — 5 ms std dev, ~10 ms RTT assumed.
  double r_hat_{25.0};
};

}  // namespace esphome::snapclient
