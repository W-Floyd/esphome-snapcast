#include "snapclient_text_sensor.h"

#if defined(USE_ESP32) && defined(USE_TEXT_SENSOR)

#include "esphome/core/log.h"

namespace esphome::snapclient {

static const char *const TAG = "snapclient.text_sensor";

void SnapclientTsfRoleTextSensor::publish_() {
  const TsfRole role = this->parent_->get_tsf_role();
  if (this->published_ && role == this->last_role_) {
    return;
  }
  this->published_ = true;
  this->last_role_ = role;
  switch (role) {
    case TsfRole::LEADER:
      this->publish_state("Leader");
      break;
    case TsfRole::FOLLOWER:
      this->publish_state("Follower");
      break;
    default:
      this->publish_state("Inactive");
      break;
  }
}

void SnapclientTsfRoleTextSensor::dump_config() { LOG_TEXT_SENSOR("", "Snapclient TSF Role Text Sensor", this); }

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_TEXT_SENSOR
