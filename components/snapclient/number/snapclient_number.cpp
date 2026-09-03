#include "snapclient_number.h"

#include <cmath>

#if defined(USE_ESP32) && defined(USE_NUMBER)

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::snapclient {

static const char *const TAG = "snapclient.number";

void SnapclientServerLatencyNumber::setup() {
  // Mirror the server's belief; every ServerSettings push carries latency
  this->parent_->add_server_settings_callback([this](uint8_t volume, bool muted, int32_t latency_ms) {
    if (!this->has_state() || static_cast<int32_t>(this->state) != latency_ms) {
      this->publish_state(latency_ms);
    }
  });
}

void SnapclientServerLatencyNumber::dump_config() { LOG_NUMBER("", "Snapclient Server Latency Number", this); }

// THREAD CONTEXT: Main loop (invoked by the number framework)
void SnapclientServerLatencyNumber::control(float value) {
  this->parent_->set_server_latency(static_cast<int32_t>(value));
  // Optimistic publish; the server's ServerSettings push confirms (or corrects) it
  this->publish_state(value);
}

void SnapclientServoParamNumber::setup() {
  // Restore and re-apply: a knob moved in the frontend should survive a reboot, and the servo
  // only learns about it by being told.
  this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
  if (this->no_restore_) {
    // Adjustable, but it pushes nothing at boot: the firmware owns and persists this quantity.
    // Publishing the minimum would misreport it, so publish nothing and let the first write or
    // the firmware's own reporting fill it in.
    return;
  }
  float restored = NAN;
  if (this->pref_.load(&restored) && std::isfinite(restored)) {
    this->last_shown_ = restored;   // a stored value is authoritative; loop() must not overwrite it
    this->publish_state(restored);
    if (this->parent_ != nullptr) {
      this->parent_->set_servo_param(this->param_, restored);
    }
  } else {
    // NEVER WRITTEN IS NOT "SET TO THE MINIMUM". This used to publish traits.get_min_value(), so a
    // knob nobody had touched reported the bottom of its range as though it were the setting:
    // timing_target_us read 1 against a compiled 20, tag_stale_ms 100 against 1000, blank_ms 0
    // against 500, gap_blank_ms 0 against 50. The timing_target_us case is the expensive one --
    // it prices the rate gain (Kp = budget / sigma_e), so the frontend understated it twentyfold,
    // and 2026-09-02 an entire analysis of why the loop under-corrects was built on the displayed
    // number before the discrepancy was noticed.
    //
    // Worse than a wrong readout: confirming the shown value WRITES it, so anyone trusting the UI
    // would have set the target to 1 us for real and crippled rate correction.
    //
    // Ask the firmware what it is running. Same reasoning the no_restore branch above already
    // applies to crystal_ppm ("publishing the minimum would misreport it"); it simply was not
    // carried across. If the answer is not available yet, publish NOTHING and let loop() fill it
    // in -- an empty knob is honest, a minimum is not.
    const float live = this->parent_ != nullptr ? this->parent_->servo_param_value(this->param_) : NAN;
    if (std::isfinite(live)) {
      this->last_shown_ = live;
      this->publish_state(live);
    }
  }
}

void SnapclientServoParamNumber::loop() {
  // TRACK, don't just remember. For a quantity the firmware learns, a knob that only ever showed
  // the last typed number would be lying within seconds -- the crystal moves continuously. So the
  // knob follows the live value and stays writable: typing sets the learner, and the learner then
  // owns it again.
  if (this->parent_ == nullptr) {
    return;
  }
  // A no_restore knob TRACKS for ever (the firmware owns and moves the value). A restore knob only
  // needs filling in ONCE, and only if it has never shown anything -- setup() runs before the
  // client is necessarily reachable, and a knob left blank would otherwise stay blank.
  if (!this->no_restore_ && std::isfinite(this->last_shown_)) {
    return;
  }
  const uint32_t now_ms = millis();
  if (now_ms - this->last_track_ms_ < 2000) {
    return;   // the frontend does not need this faster than the crystal moves
  }
  this->last_track_ms_ = now_ms;
  const float v = this->parent_->servo_param_value(this->param_);
  if (!std::isfinite(v)) {
    return;
  }
  // Only on a real move, so a value idling between steps does not generate API traffic for ever.
  const float step = this->traits.get_step() > 0.0f ? this->traits.get_step() : 0.01f;
  if (std::isfinite(this->last_shown_) && std::fabs(v - this->last_shown_) < step) {
    return;
  }
  this->last_shown_ = v;
  this->publish_state(v);
}

void SnapclientServoParamNumber::control(float value) {
  if (this->parent_ != nullptr) {
    this->parent_->set_servo_param(this->param_, value);
  }
  if (!this->no_restore_) {
    this->pref_.save(&value);   // the firmware persists the no-restore ones itself
  }
  this->last_shown_ = value;
  this->publish_state(value);
}

void SnapclientServoParamNumber::dump_config() {
  LOG_NUMBER("", "Snapclient servo parameter", this);
  ESP_LOGCONFIG(TAG, "  Parameter: %s", this->param_.c_str());
}

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_NUMBER
