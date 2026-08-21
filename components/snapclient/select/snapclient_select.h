#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_SELECT)

#include "esphome/components/select/select.h"
#include "esphome/components/snapclient/snapclient_hub.h"
#include "esphome/core/preferences.h"

namespace esphome::snapclient {

/// @brief Select entity for the output channel routing (Stereo / Left / Right / Mono).
///
/// The equivalent of esp32 snapclient's web-UI channel mode: switchable at runtime
/// (e.g. two devices sharing a stereo stream as a L/R pair) and persisted across
/// reboots. The persisted value takes precedence over any YAML default.
class SnapclientChannelModeSelect final : public SnapclientChild, public select::Select {
 public:
  void setup() override;
  void dump_config() override;

  void set_restore_value(bool restore) { this->restore_value_ = restore; }

 protected:
  void control(const std::string &value) override;

  /// @brief Applies @p index to the hub and publishes the matching option.
  void apply_(uint8_t index);

  bool restore_value_{true};
  ESPPreferenceObject pref_;
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_SELECT
