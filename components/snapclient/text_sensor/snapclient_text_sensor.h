#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_TEXT_SENSOR)

#include "esphome/components/snapclient/snapclient_hub.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome::snapclient {

/// @brief Publishes this device's TSF group-sync role: "Leader" / "Follower" /
/// "Inactive" (tsf_sync off or unsupported, no session, or no election yet).
/// Polled from loop() with change detection — the underlying read is one atomic.
class SnapclientTsfRoleTextSensor final : public SnapclientChild, public text_sensor::TextSensor {
 public:
  void setup() override { this->publish_(); }
  void loop() override { this->publish_(); }
  void dump_config() override;

 protected:
  void publish_();

  TsfRole last_role_{TsfRole::INACTIVE};
  bool published_{false};
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_TEXT_SENSOR
