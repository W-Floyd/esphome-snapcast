#pragma once
//
// The S3 MCLK divider's fraction arithmetic, with no hardware in it, so it can be tested on the
// host. MCLK = SRC / (N + b/a), a <= 511, b < a, the whole fraction in one 32-bit register.
//
// Two pieces:
//   * encode/decode of the x/y/z/yn1 field packing
//   * pick_frac(): the two representable ratios bracketing a target fraction, and the duty that
//     makes their dithered mean equal it
//
// It lives in a header of its own because rate_lock.cpp touches i2s_hw() and cannot be compiled
// off-target, so anything left inside it is untestable by construction.

#include <cmath>
#include <cstdint>

namespace esphome::i2s_rate_lock {

/// x/y/z are 9-bit fields; denominators to 511 give ~0.15 ppm rational spacing.
inline constexpr uint32_t FRAC_MAX_DENOMINATOR = 511;

inline uint32_t encode_frac(uint32_t b, uint32_t a) {
  uint32_t x = 0, y = 0, z = 0, yn1 = 0;
  if (b != 0) {
    yn1 = (2 * b > a) ? 1 : 0;
    z = yn1 ? a - b : b;
    x = a / z - 1;
    y = a % z;
  }
  return (z & 0x1FF) | ((y & 0x1FF) << 9) | ((x & 0x1FF) << 18) | (yn1 << 27);
}

/// Inverts the encoding. z == 0 is a pure integer divider (b = 0).
/// @return false on an undecodable field combination.
inline bool decode_frac(uint32_t val, uint32_t &b, uint32_t &a) {
  const uint32_t z = val & 0x1FF;
  const uint32_t y = (val >> 9) & 0x1FF;
  const uint32_t x = (val >> 18) & 0x1FF;
  const uint32_t yn1 = (val >> 27) & 0x1;
  if (z == 0) {
    b = 0;
    a = 1;
    return true;
  }
  a = (x + 1) * z + y;
  if (a == 0 || a > FRAC_MAX_DENOMINATOR) return false;
  b = yn1 ? a - z : z;
  return b < a;
}

struct FracChoice {
  uint32_t lo_b = 0, lo_a = 1;
  uint32_t hi_b = 1, hi_a = 1;
  double lo_v = 0.0, hi_v = 1.0;
  /// Share of ticks spent on hi so the dithered mean equals the target.
  double duty = 0.0;
  /// True when one end IS the target and there is nothing to dither.
  bool exact = false;
  /// Which end to use when exact.
  bool use_lo = true;
};

/// The representable ratios bracketing `frac`, and the duty between them.
inline FracChoice pick_frac(double frac) {
  FracChoice c;
  for (uint32_t a = 2; a <= FRAC_MAX_DENOMINATOR; a++) {
    const double fa = frac * a;
    const auto bl = static_cast<uint32_t>(std::fmin(std::floor(fa), static_cast<double>(a - 1)));
    const auto bh = static_cast<uint32_t>(std::fmin(std::ceil(fa), static_cast<double>(a - 1)));
    const double vl = static_cast<double>(bl) / a;
    const double vh = static_cast<double>(bh) / a;
    if (vl <= frac && vl > c.lo_v) {
      c.lo_v = vl;
      c.lo_b = bl;
      c.lo_a = a;
    }
    if (vh >= frac && vh < c.hi_v) {
      c.hi_v = vh;
      c.hi_b = bh;
      c.hi_a = a;
    }
  }
  const double gap = c.hi_v - c.lo_v;
  c.exact = gap < 1e-9 || (frac - c.lo_v) < 1e-9 || (c.hi_v - frac) < 1e-9;
  c.use_lo = (frac - c.lo_v) <= (c.hi_v - frac);
  c.duty = c.exact ? 0.0 : (frac - c.lo_v) / gap;
  return c;
}

/// First-order sigma-delta in 1/65536 units, as tick() runs it. Returns true for a hi tick and
/// advances the accumulator: the long-run share of hi equals duty and the instantaneous error
/// never exceeds one tick.
inline bool sigma_delta_step(uint32_t duty_q16, uint32_t &acc) {
  acc += duty_q16;
  if (acc >= 65536u) {
    acc -= 65536u;
    return true;
  }
  return false;
}

}  // namespace esphome::i2s_rate_lock
