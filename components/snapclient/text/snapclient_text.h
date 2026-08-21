#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_TEXT)

#include "esphome/components/snapclient/snapclient_hub.h"
#include "esphome/components/text/text.h"
#include "esphome/core/preferences.h"

namespace esphome::snapclient {

/// @brief Manual server override: "host" or "host:port"; empty clears.
///
/// Highest-precedence connection target -- above the discovered-server select, the
/// YAML `server:`, and mDNS automatic discovery (see SnapclientHub::set_server_manual).
class SnapclientServerText final : public SnapclientChild, public text::Text {
 public:
  void setup() override;
  void dump_config() override;

  void set_restore_value(bool restore) { this->restore_value_ = restore; }

 protected:
  void control(const std::string &value) override;
  /// @brief Parses @p value, pushes the override to the hub, publishes the state.
  void apply_(const std::string &value);

  struct StoredValue {
    char value[65];
  };
  bool restore_value_{true};
  ESPPreferenceObject pref_;
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_TEXT
