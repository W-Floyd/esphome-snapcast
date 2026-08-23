#include "wifi_bssid_select.h"

#if defined(USE_ESP32) && defined(USE_SELECT)

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace esphome::wifi_bssid_select {

#ifdef WIFI_BSSID_SELECT_ACTIVE

static const char *const TAG = "wifi_bssid_select";
static const char AUTOMATIC_OPTION[] = "Automatic";

// How long to wait for the preferred AP before falling back to any AP on the SSID.
// Long enough to cover a normal associate plus a retry, short enough that a missing AP
// does not strand the device.
static constexpr uint32_t PIN_FALLBACK_MS = 20000;

void DynamicSelect::set_dynamic_options_(std::vector<std::string> &&options) {
  this->option_store_ = std::move(options);
  FixedVector<const char *> ptrs;
  ptrs.init(this->option_store_.size());
  for (const auto &option : this->option_store_) {
    ptrs.push_back(option.c_str());
  }
  this->traits.set_options(ptrs);
}

static std::string bssid_to_string(const uint8_t *b) {
  return str_sprintf("%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

static bool parse_bssid(const std::string &s, wifi::bssid_t &out) {
  unsigned int v[6];
  if (sscanf(s.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; i++) {
    out[i] = static_cast<uint8_t>(v[i]);
  }
  return true;
}

void WifiBssidSelect::setup() {
  auto *w = wifi::global_wifi_component;
  if (w == nullptr) {
    this->mark_failed();
    return;
  }
  w->add_scan_results_listener(this);
  w->add_connect_state_listener(this);

  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<StoredOption>(this->get_object_id_hash());
    StoredOption stored{};
    wifi::bssid_t parsed;
    if (this->pref_.load(&stored)) {
      stored.value[sizeof(stored.value) - 1] = '\0';
      if (stored.value[0] != '\0' && parse_bssid(stored.value, parsed)) {
        this->desired_ = stored.value;
      }
    }
  }
  // The lock itself is applied by on_wifi_connect_state once connected
  this->rebuild_options_();
}

// THREAD CONTEXT: Main loop (invoked by the select framework)
void WifiBssidSelect::control(const std::string &value) {
  wifi::bssid_t parsed;
  this->desired_ = (value != AUTOMATIC_OPTION && parse_bssid(value, parsed)) ? value : "";
  // An explicit choice always gets a fresh attempt, even if a previous one timed out
  this->pin_released_ = false;
  this->pinned_since_ms_ = 0;
  if (this->restore_value_) {
    StoredOption stored{};
    strncpy(stored.value, this->desired_.c_str(), sizeof(stored.value) - 1);
    this->pref_.save(&stored);
  }
  this->apply_lock_();
  this->rebuild_options_();
}


void WifiBssidSelect::release_pin_(const char *reason) {
  auto *w = wifi::global_wifi_component;
  if (w == nullptr) {
    return;
  }
  wifi::WiFiAP sta = w->get_sta();
  if (sta.get_ssid().empty()) {
    return;
  }
  sta.clear_bssid();
  w->set_sta(sta);
  this->pin_released_ = true;
  this->pinned_since_ms_ = 0;
  ESP_LOGW(TAG, "Preferred BSSID %s %s; associating with any AP on the SSID", this->desired_.c_str(), reason);
}

void WifiBssidSelect::loop() {
  if (this->desired_.empty() || this->pin_released_ || this->pinned_since_ms_ == 0) {
    return;
  }
  auto *w = wifi::global_wifi_component;
  if (w != nullptr && w->is_connected()) {
    this->pinned_since_ms_ = 0;  // got there; nothing to fall back from
    return;
  }
  if (millis() - this->pinned_since_ms_ >= PIN_FALLBACK_MS) {
    this->release_pin_("unreachable");
  }
}

void WifiBssidSelect::apply_lock_() {
  auto *w = wifi::global_wifi_component;
  wifi::WiFiAP sta = w->get_sta();
  if (sta.get_ssid().empty()) {
    // Pre-first-connect the selected STA config is empty; stamping a BSSID onto it
    // would wipe the credentials. on_wifi_connect_state re-applies once connected.
    return;
  }
  wifi::bssid_t target;
  if (!this->desired_.empty() && parse_bssid(this->desired_, target) && !this->pin_released_) {
    sta.set_bssid(target);
    w->set_sta(sta);
    if (w->is_connected() && w->wifi_bssid() != target) {
      ESP_LOGI(TAG, "On wrong AP; reconnecting to preferred %s", this->desired_.c_str());
      w->retry_connect();
    }
    if (!w->is_connected() && this->pinned_since_ms_ == 0) {
      this->pinned_since_ms_ = millis();  // start the fallback timer
    }
  } else {
    sta.clear_bssid();
    w->set_sta(sta);
  }
}

void WifiBssidSelect::on_wifi_connect_state(StringRef ssid, std::span<const uint8_t, 6> bssid) {
  if (ssid.empty()) {
    // Disconnect: re-arm the preference. Retrying it here rather than while a fallback
    // link is up means we never drop a working connection to chase the preferred AP --
    // the retry rides a reconnect that was happening anyway.
    if (this->pin_released_) {
      this->pin_released_ = false;
      this->pinned_since_ms_ = 0;
      ESP_LOGI(TAG, "Link dropped; re-arming preferred BSSID %s", this->desired_.c_str());
      this->apply_lock_();
    }
    return;
  }
  this->network_ssid_ = ssid.str();
  this->pinned_since_ms_ = 0;
  this->apply_lock_();
  this->rebuild_options_();
}

void WifiBssidSelect::on_wifi_scan_results(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results) {
  this->scan_snapshot_.clear();
  for (const auto &r : results) {
    this->scan_snapshot_.emplace_back(r.get_ssid().str(), bssid_to_string(r.get_bssid().data()));
  }
  this->rebuild_options_();
}

void WifiBssidSelect::rebuild_options_() {
  std::vector<std::string> options{AUTOMATIC_OPTION};
  auto listed = [&options](const std::string &v) {
    return std::find(options.begin(), options.end(), v) != options.end();
  };
  // Scan results are filtered to the connected SSID (unknown before first connect,
  // so the boot scan's snapshot is re-filtered once on_wifi_connect_state names it)
  for (const auto &entry : this->scan_snapshot_) {
    if (!this->network_ssid_.empty() && entry.first == this->network_ssid_ && !listed(entry.second)) {
      options.push_back(entry.second);
    }
  }
  // The current AP and the pinned choice stay listed even when scans miss them
  auto *w = wifi::global_wifi_component;
  if (w != nullptr && w->is_connected()) {
    const std::string current = bssid_to_string(w->wifi_bssid().data());
    if (!listed(current)) {
      options.push_back(current);
    }
  }
  if (!this->desired_.empty() && !listed(this->desired_)) {
    options.push_back(this->desired_);
  }
  this->set_dynamic_options_(std::move(options));
  this->publish_state(this->desired_.empty() ? AUTOMATIC_OPTION : this->desired_);
}

void WifiBssidSelect::dump_config() { LOG_SELECT("", "WiFi BSSID Select", this); }


#endif  // WIFI_BSSID_SELECT_ACTIVE

}  // namespace esphome::wifi_bssid_select

#endif  // USE_ESP32 && USE_SELECT
