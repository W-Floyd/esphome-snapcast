#pragma once

// Preferred-AP picker; nothing here is Snapcast-specific. Pinning an AP removes
// mid-stream roams, which is useful to anything that suffers from roaming.
//
// The pin is a PREFERENCE, not a requirement -- see release_pin_() for why that
// distinction is the whole point of this entity.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_SELECT)

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#if defined(USE_WIFI) && defined(USE_WIFI_SCAN_RESULTS_LISTENERS) && defined(USE_WIFI_CONNECT_STATE_LISTENERS)
#include "esphome/components/wifi/wifi_component.h"
#define WIFI_BSSID_SELECT_ACTIVE
#endif

namespace esphome::wifi_bssid_select {

#ifdef WIFI_BSSID_SELECT_ACTIVE

/// @brief Select with runtime-built options. The traits hold raw const char* pointers
/// (normally codegen string literals), so the entity must own the backing strings for
/// as long as the traits reference them.
///
/// Deliberately duplicated from snapclient's identical helper rather than shared: it is
/// ten lines, and importing it would make this component depend on snapclient, which is
/// exactly the dependency the split removes.
class DynamicSelect : public select::Select {
 protected:
  /// @brief Replaces the option list, taking ownership of the strings.
  void set_dynamic_options_(std::vector<std::string> &&options);

  std::vector<std::string> option_store_;
};

class WifiBssidSelect final : public Component,
                                    public DynamicSelect,
                                    public wifi::WiFiScanResultsListener,
                                    public wifi::WiFiConnectStateListener {
 public:
  // After the wifi component (registers listeners on the wifi global)
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI - 1.0f; }
  void setup() override;
  void dump_config() override;

  void set_restore_value(bool restore) { this->restore_value_ = restore; }

  // THREAD CONTEXT: main loop (wifi component's loop)
  void on_wifi_scan_results(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results) override;
  void on_wifi_connect_state(StringRef ssid, std::span<const uint8_t, 6> bssid) override;
  void loop() override;

 protected:
  void control(const std::string &value) override;
  /// @brief Applies desired_ to the connected STA config (no-op before first connect).
  void apply_lock_();
  void rebuild_options_();

  /// @brief Drops the pin so wifi can associate with any AP on the SSID.
  /// The selection is remembered; only the constraint handed to wifi is released.
  void release_pin_(const char *reason);

  std::string desired_;        // preferred BSSID string; empty = automatic
  std::string network_ssid_;   // SSID of the current connection (connect listener)
  std::vector<std::pair<std::string, std::string>> scan_snapshot_;  // (ssid, bssid)
  struct StoredOption {
    char value[18];
  };
  bool restore_value_{true};
  // A pin is a PREFERENCE, not a requirement: wifi's set_bssid() will refuse to
  // associate with anything else, so an AP that is gone, overloaded, or simply out of
  // range leaves the client unable to connect at all rather than degraded. Track how
  // long we have been pinned-but-disconnected and drop the constraint past a timeout.
  uint32_t pinned_since_ms_{0};  // 0 = not waiting on a pinned connect
  bool pin_released_{false};     // pin dropped after timing out; re-armed on next drop
  ESPPreferenceObject pref_;
};
#endif  // WIFI_BSSID_SELECT_ACTIVE

}  // namespace esphome::wifi_bssid_select

#endif  // USE_ESP32 && USE_SELECT
