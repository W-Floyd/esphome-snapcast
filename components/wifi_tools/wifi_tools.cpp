#include "wifi_tools.h"

#ifdef USE_ESP32

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/log.h"

#include <esp_wifi.h>

namespace esphome::wifi_tools {

// Tags kept as they were when this lived in YAML, so existing log filters and the
// analysis scripts keep matching.
static const char *const TAG_TX = "wifi_tx";
static const char *const TAG_DIAG = "wifi_diag";

float set_max_tx_power(float dbm) {
  const int8_t want = static_cast<int8_t>(dbm * 4);  // driver works in 0.25 dBm units
  const esp_err_t err = esp_wifi_set_max_tx_power(want);
  int8_t got = 0;
  esp_wifi_get_max_tx_power(&got);
  ESP_LOGI(TAG_TX, "TX power: requested %.2f dBm -> %s; driver reports %.2f dBm", dbm, esp_err_to_name(err),
           got / 4.0f);
  return err == ESP_OK ? got / 4.0f : -1.0f;
}

float WifiDiagnostics::get_setup_priority() const { return setup_priority::AFTER_WIFI; }

void WifiDiagnostics::dump_config() {
  ESP_LOGCONFIG(TAG_DIAG, "WiFi Diagnostics:");
  ESP_LOGCONFIG(TAG_DIAG, "  Statistics dump: %s", YESNO(this->dump_statistics_));
}

void WifiDiagnostics::update() {
  // Nothing to read while disassociated, and esp_wifi_sta_get_rssi() would fail
  if (wifi::global_wifi_component == nullptr || !wifi::global_wifi_component->is_connected()) {
    return;
  }
  int rssi = 0;
  esp_wifi_sta_get_rssi(&rssi);
  wifi_phy_mode_t phymode;
  static const char *const PHY_NAMES[] = {"LR", "11B", "11G", "11A", "HT20", "HT40", "HE20", "VHT20"};
  const char *phy = esp_wifi_sta_get_negotiated_phymode(&phymode) == ESP_OK && phymode <= WIFI_PHY_MODE_VHT20
                        ? PHY_NAMES[phymode]
                        : "?";
  ESP_LOGD(TAG_DIAG, "rssi=%d dBm, phymode=%s", rssi, phy);
  if (this->dump_statistics_) {
    esp_wifi_statis_dump(WIFI_STATIS_RXTX | WIFI_STATIS_HW);
  }
}

}  // namespace esphome::wifi_tools

#endif  // USE_ESP32
