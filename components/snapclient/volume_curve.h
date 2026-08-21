#pragma once

#include <algorithm>
#include <cmath>

namespace esphome::snapclient {

/// @brief Exponential volume taper, ported from esp32 snapclient's dsp_processor.
///
/// Maps a slider position x ∈ [0,1] to an amplitude gain y ∈ [0,1]:
///   y = a·e^(b·x), a = 10^(-dB_range/20), b = -ln(a)
/// so the slider spans dB_range decibels perceptually evenly. A range of 0 bypasses
/// the curve (linear). Below ROLLOFF_THRESHOLD the curve is replaced by a linear ramp
/// so slider = 0 yields true silence.
class VolumeCurve {
 public:
  void set_db_range(float db_range) {
    this->db_range_ = db_range;
    this->a_ = std::pow(10.0f, -db_range / 20.0f);
    this->b_ = -std::log(this->a_);
    this->threshold_val_ = this->a_ * std::exp(this->b_ * ROLLOFF_THRESHOLD);
  }

  float get_db_range() const { return this->db_range_; }

  /// @brief Slider position [0,1] -> amplitude gain [0,1].
  float apply(float x) const {
    x = std::clamp(x, 0.0f, 1.0f);
    if (this->db_range_ == 0.0f) {
      return x;
    }
    if (x <= ROLLOFF_THRESHOLD) {
      return x / ROLLOFF_THRESHOLD * this->threshold_val_;
    }
    return this->a_ * std::exp(this->b_ * x);
  }

  /// @brief Amplitude gain [0,1] -> slider position [0,1] (inverse of apply()).
  float inverse(float y) const {
    y = std::clamp(y, 0.0f, 1.0f);
    if (this->db_range_ == 0.0f) {
      return y;
    }
    if (y <= this->threshold_val_) {
      return y / this->threshold_val_ * ROLLOFF_THRESHOLD;
    }
    return std::log(y / this->a_) / this->b_;
  }

 protected:
  static constexpr float ROLLOFF_THRESHOLD = 0.1f;

  float db_range_{0.0f};
  float a_{1.0f};
  float b_{0.0f};
  float threshold_val_{ROLLOFF_THRESHOLD};
};

}  // namespace esphome::snapclient
