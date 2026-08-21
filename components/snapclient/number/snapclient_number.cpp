#include "snapclient_number.h"

#if defined(USE_ESP32) && defined(USE_NUMBER)

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::snapclient {

static const char *const TAG = "snapclient.number";

void SnapclientVolumeCurveNumber::setup() {
  float value = this->initial_value_;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
    float restored;
    if (this->pref_.load(&restored)) {
      value = restored;
    }
  }
  this->parent_->set_volume_curve_db_range(value);
  this->publish_state(value);
}

void SnapclientVolumeCurveNumber::dump_config() { LOG_NUMBER("", "Snapclient Volume Curve Number", this); }

// THREAD CONTEXT: Main loop (invoked by the number framework)
void SnapclientVolumeCurveNumber::control(float value) {
  this->parent_->set_volume_curve_db_range(value);
  this->publish_state(value);
  if (this->restore_value_) {
    this->pref_.save(&value);
  }
}

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

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_NUMBER
