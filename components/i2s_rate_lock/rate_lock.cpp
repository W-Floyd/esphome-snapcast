#include "rate_lock.h"

#include "frac_select.h"

#if defined(USE_ESP32) && defined(USE_I2S_RATE_LOCK)

#include "esphome/core/log.h"

#include <sdkconfig.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>

#if CONFIG_IDF_TARGET_ESP32S3
#include <soc/i2s_struct.h>
#endif

namespace esphome::i2s_rate_lock {

static const char *const TAG = "snapclient.rate_lock";

#if CONFIG_IDF_TARGET_ESP32S3

// ===================== ESP32-S3 backend: MCLK fractional-N divider =====================
//
// The S3's I2S MCLK comes from a fixed PLL through a fractional divider
// (TRM 29.5, hal/esp32s3/i2s_ll.h): MCLK = SRC / (N + b/a), with the fraction
// encoded into dual-modulus counter fields:
//
//   yn1 = (2b > a);  z = yn1 ? a - b : b;  x = a / z - 1;  y = a % z
//
// All four fields live in the single 32-bit tx_clkm_div_conf register, so one store
// swaps the whole fraction atomically -- the divider never passes through an
// intermediate state. The integer part (tx_clkm_conf.tx_clkm_div_num) is NEVER
// written here: live div_num changes are what IDF's "double division" errata
// workaround exists for (i2s_ll_tx_set_raw_clk_div bounces through div_num = 2, a
// ~6.5x MCLK overspeed burst -- unusable on a running channel), and a +-500 ppm trim
// never needs the integer part to move.

// Hardware-side backstop only. The caller's servo derives its own, tighter clamp
// from KP * converge_fine (see trim_clamp_ppm() in snapcast_client.cpp) and must be
// the binding limit -- if this one bit first it would silently truncate that
// derivation, which is exactly what happened while both were a fixed 500 ppm: the
// caller's clamp was raised and nothing changed. Sized instead by what the divider
// can express without touching the integer part: the fraction has ~0.007 of headroom
// before the integer would move, i.e. several thousand ppm, and set_trim_ppm() checks
// that explicitly below. 5000 ppm is 0.5% pitch, at the audibility JND, so this also
// caps a runaway caller at "noticeable" rather than "unlistenable".
static constexpr float TRIM_CLAMP_PPM = 5000.0f;

// Baseline correction. Trims are relative to the baseline divider, so an error in
// the divider the I2S DRIVER chose would be a DC offset the servo must cancel out of
// its own +-500 ppm authority. The ideal ratio IS exactly representable here --
//
//   ideal 160 MHz / (44100 * 256) = 6250/441 = 14 + 76/441
//
// -- so where the driver's approximation is off, recomputing the ideal and using
// THAT as the reference collapses the DC term to the rational quantization floor
// (~0.15 ppm).
//
// CAUTION on how rarely this actually applies. Every observed nonzero "driver error"
// turned out to be OUR OWN last trim read back (see read_baseline_): the only
// genuine driver value seen so far, on four devices at boot, was 76/441 -- already
// exact, nothing to correct. So this path is a safety net for a driver that picks
// badly, not a fix for an observed fault. Do not cite it as the explanation for a
// railed servo: that hypothesis was tested and disproved.
//
// PLL_F160M is the S3's default I2S clock source.
static constexpr double I2S_SRC_HZ = 160000000.0;
// mclk_multiple is the speaker's choice and is not readable back, so it is inferred
// from the divider the driver programmed. Adjacent multiples differ by >=33%, and the
// driver's own error is <1000 ppm, so the match is unambiguous at this tolerance.
static constexpr uint32_t MCLK_MULTIPLES[] = {64, 128, 192, 256, 384, 512, 576, 768, 1024};
static constexpr double MULT_MATCH_TOL = 0.01;

// tx_clkm_div_conf field layout: z[8:0] y[17:9] x[26:18] yn1[27]
static constexpr uint32_t FRAC_FIELDS_MASK = 0x0FFFFFFF;



static i2s_dev_t *i2s_hw(uint8_t port) { return port == 0 ? &I2S0 : &I2S1; }

bool RateLock::read_baseline_() {
  if (this->port_ > 1) {
    ESP_LOGW(TAG, "Invalid I2S port %u", this->port_);
    return false;
  }
  i2s_dev_t *hw = i2s_hw(this->port_);
  if (!hw->tx_clkm_conf.tx_clk_active) {
    ESP_LOGW(TAG, "I2S%u TX clock not active (speaker not started?)", this->port_);
    return false;
  }
  const uint32_t div_num = hw->tx_clkm_conf.tx_clkm_div_num;
  if (div_num < 2) {
    // 0/1 are bypass/reset states the driver never programs for audio
    ESP_LOGW(TAG, "I2S%u divider looks uninitialized (div_num=%" PRIu32 ")", this->port_, div_num);
    return false;
  }
  const uint32_t frac = hw->tx_clkm_div_conf.val & FRAC_FIELDS_MASK;

  // The register may hold OUR OWN last trim rather than anything the driver chose:
  // invalidate_baseline() fires on events (starvation re-baseline, feedback flush
  // gap) that do not necessarily restart the I2S channel. Re-reading then would
  // reinterpret the servo's learned offset as driver error and zero it -- measured
  // across five mid-stream reads on two devices, the "baseline error" came out at
  // exactly minus the trim applied a moment earlier (-267.9 vs +267.87, -350.2 vs
  // +350.17, +39.6 vs -39.63, +27.6 vs -27.64, -8.0 vs +8.00). Correcting that to
  // "ideal" throws away the rate the servo had converged on and dumps the loop on
  // its rail while the integral unwinds. If the register still holds what we wrote,
  // nothing was reprogrammed: keep the reference and the trim exactly as they are.
  const bool ours = frac == this->last_frac_val_.load(std::memory_order_relaxed) ||
                    (this->dither_on_.load(std::memory_order_relaxed) &&
                     (frac == this->dither_lo_.load(std::memory_order_relaxed) ||
                      frac == this->dither_hi_.load(std::memory_order_relaxed)));
  if (this->ours_valid_ && ours && this->base_ratio_ > 0.0) {
    ESP_LOGD(TAG, "I2S%u divider unchanged since our last write; keeping baseline", this->port_);
    return true;
  }

  uint32_t b, a;
  // Round-trip sanity check: decode then re-encode must reproduce the register, or
  // our layout/encoding assumptions do not hold on this chip/IDF -- refuse to steer.
  if (!decode_frac(frac, b, a) || encode_frac(b, a) != frac) {
    ESP_LOGW(TAG, "I2S%u divider round-trip mismatch (0x%08" PRIX32 "), not steering", this->port_, frac);
    return false;
  }
  this->base_int_ = div_num;
  this->base_num_ = b;
  this->base_den_ = a;
  this->last_frac_val_.store(frac, std::memory_order_relaxed);

  const double programmed = div_num + static_cast<double>(b) / a;
  this->base_ratio_ = programmed;
  this->baseline_corrected_ppm_ = 0.0f;

  // Replace the driver's approximation with the exact ideal ratio where we can
  // establish it. Bails out (keeping the driver's baseline, i.e. the old behaviour)
  // on anything unverified: no known output rate, no mclk_multiple that explains the
  // programmed divider, or an ideal whose INTEGER part differs -- tx_clkm_div_num
  // cannot be rewritten glitch-free on a running channel.
  const uint32_t rate = this->output_rate_;
  if (rate == 0) {
    ESP_LOGD(TAG, "I2S%u baseline divider: %" PRIu32 " + %" PRIu32 "/%" PRIu32 " (rate unknown, uncorrected)",
             this->port_, div_num, b, a);
    return true;
  }
  for (const uint32_t mult : MCLK_MULTIPLES) {
    const double ideal = I2S_SRC_HZ / (static_cast<double>(rate) * mult);
    if (std::fabs(ideal - programmed) / programmed > MULT_MATCH_TOL) {
      continue;
    }
    if (static_cast<uint32_t>(ideal) != div_num) {
      ESP_LOGW(TAG, "I2S%u ideal divider %.6f needs integer part %u, driver programmed %" PRIu32 "; uncorrected",
               this->port_, ideal, static_cast<unsigned>(ideal), div_num);
      break;
    }
    this->base_ratio_ = ideal;
    // Positive = the driver was running SLOW and the servo had to push this hard
    this->baseline_corrected_ppm_ = static_cast<float>((programmed / ideal - 1.0) * 1e6);
    ESP_LOGD(TAG,
             "I2S%u baseline divider: %" PRIu32 " + %" PRIu32 "/%" PRIu32
             " (driver) -> ideal %.6f for %" PRIu32 " Hz x%" PRIu32 ", reclaimed %+.1f ppm",
             this->port_, div_num, b, a, ideal, rate, mult, this->baseline_corrected_ppm_);
    return true;
  }
  ESP_LOGD(TAG, "I2S%u baseline divider: %" PRIu32 " + %" PRIu32 "/%" PRIu32 " (no mclk_multiple match, uncorrected)",
           this->port_, div_num, b, a);
  return true;
}

bool RateLock::set_trim_ppm(float ppm) {
  if (this->rebaseline_.exchange(false, std::memory_order_relaxed)) {
    this->baseline_valid_ = false;
    this->applied_ppm_ = 0.0f;
    this->dither_on_.store(false, std::memory_order_relaxed);
  }
  if (!this->baseline_valid_) {
    if (!this->read_baseline_()) {
      return false;
    }
    this->baseline_valid_ = true;
  }

  ppm = std::clamp(ppm, -TRIM_CLAMP_PPM, TRIM_CLAMP_PPM);
  // Ideal ratio where read_baseline_ could establish it, else the driver's own --
  // so ppm is measured against the CORRECT rate, not against the driver's rounding
  const double base_ratio = this->base_ratio_;
  // Positive ppm = play faster = higher MCLK = smaller divider
  const double target = base_ratio * (1.0 - static_cast<double>(ppm) * 1e-6);
  const double frac = target - this->base_int_;
  if (frac < 0.0 || frac >= 1.0) {
    // Reaching this trim needs a different integer divider, which cannot be written
    // glitch-free on a running channel. Cannot occur within the clamp unless the
    // base fraction sits within ~0.007 of 0 or 1.
    ESP_LOGW(TAG, "Trim %+.1f ppm would change the integer divider, not steering", ppm);
    return false;
  }

  // The two achievable ratios that bracket the target fraction: the largest b/a <= frac
  // and the smallest b/a >= frac over all denominators (~1000 float ops, a few times per
  // second). Where one of them IS the target (err < 1e-9) there is nothing to dither.
  // Bracket search and duty live in frac_select.h so they can be tested off-target
  // (tests/rate_lock/run.sh); this file cannot be compiled on the host.
  const FracChoice fc = pick_frac(frac);
  const uint32_t lo_val = encode_frac(fc.lo_b, fc.lo_a);
  const uint32_t hi_val = encode_frac(fc.hi_b, fc.hi_a);
  const double lo_v = fc.lo_v, hi_v = fc.hi_v;
  const double gap = hi_v - lo_v;

  if (fc.exact) {
    // Single value: whichever end coincides with the target
    const bool use_lo = fc.use_lo;
    const uint32_t val = use_lo ? lo_val : hi_val;
    this->dither_on_.store(false, std::memory_order_relaxed);
    if (val != this->last_frac_val_.load(std::memory_order_relaxed)) {
      // Single 32-bit store: the fraction changes atomically
      i2s_hw(this->port_)->tx_clkm_div_conf.val = val;
      this->last_frac_val_.store(val, std::memory_order_relaxed);
    }
    this->dither_gap_ppm_ = 0.0f;
    const double applied_ratio = this->base_int_ + (use_lo ? lo_v : hi_v);
    this->applied_ppm_ = static_cast<float>((1.0 - applied_ratio / base_ratio) * 1e6);
  } else {
    // Publish the bracket for tick(); duty = share of ticks on hi so the mean hits frac.
    // Order: lo/hi/duty first, then dither_on_, so tick() never pairs a new duty with an
    // old bracket. tick() writes the register from here on; nothing is written now, so
    // the current value (one of the previous bracket, or the last single value) stays
    // until the first tick -- at most one 10 ms interval at the old rate.
    const double duty = fc.duty;
    this->dither_lo_.store(lo_val, std::memory_order_relaxed);
    this->dither_hi_.store(hi_val, std::memory_order_relaxed);
    this->dither_duty_.store(static_cast<uint32_t>(std::lround(duty * 65536.0)), std::memory_order_relaxed);
    this->dither_on_.store(true, std::memory_order_release);
    this->dither_gap_ppm_ = static_cast<float>(gap / base_ratio * 1e6);
    // The dithered mean IS the target, to 1/65536 of the gap
    this->applied_ppm_ = static_cast<float>((1.0 - target / base_ratio) * 1e6);
  }
  // Marks the written values as OURS, so a later re-baseline can tell "the driver
  // reprogrammed the clock" from "nothing happened since we wrote this"
  this->ours_valid_ = true;
  return true;
}

void RateLock::tick() {
  if (!this->dither_on_.load(std::memory_order_acquire)) {
    return;
  }
  // First-order sigma-delta in 1/65536 units: the accumulator carries the running
  // shortfall between the requested duty and the hi-ticks delivered, so the long-run
  // share of hi equals duty and the instantaneous error never exceeds one tick.
  const bool hi = sigma_delta_step(this->dither_duty_.load(std::memory_order_relaxed), this->sd_acc_);
  const uint32_t want = hi ? this->dither_hi_.load(std::memory_order_relaxed)
                           : this->dither_lo_.load(std::memory_order_relaxed);
  if (want != this->last_frac_val_.load(std::memory_order_relaxed)) {
    // Single 32-bit store: the fraction changes atomically
    i2s_hw(this->port_)->tx_clkm_div_conf.val = want;
    this->last_frac_val_.store(want, std::memory_order_relaxed);
  }
}

#else  // !CONFIG_IDF_TARGET_ESP32S3

// Stub backend: rate lock unavailable, the caller's splice servo stays in charge.
bool RateLock::read_baseline_() { return false; }
bool RateLock::set_trim_ppm(float) { return false; }
void RateLock::tick() {}

#endif  // CONFIG_IDF_TARGET_ESP32S3

}  // namespace esphome::i2s_rate_lock

#endif  // USE_ESP32 && USE_I2S_RATE_LOCK
