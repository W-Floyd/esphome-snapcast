#include "snapclient_number.h"

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
  float restored = NAN;
  if (this->pref_.load(&restored) && std::isfinite(restored)) {
    this->publish_state(restored);
    if (this->parent_ != nullptr) {
      this->parent_->set_servo_param(this->param_, restored);
    }
  } else {
    this->publish_state(this->traits.get_min_value());
  }
}

void SnapclientServoParamNumber::control(float value) {
  if (this->parent_ != nullptr) {
    this->parent_->set_servo_param(this->param_, value);
  }
  this->pref_.save(&value);
  this->publish_state(value);
}

void SnapclientServoParamNumber::dump_config() {
  LOG_NUMBER("", "Snapclient servo parameter", this);
  ESP_LOGCONFIG(TAG, "  Parameter: %s", this->param_.c_str());
}

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_NUMBER
