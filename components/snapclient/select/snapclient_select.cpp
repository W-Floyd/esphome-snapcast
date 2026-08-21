#include "snapclient_select.h"

#if defined(USE_ESP32) && defined(USE_SELECT)

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome::snapclient {

static const char *const TAG = "snapclient.select";

void SnapclientIndexedSelect::setup() {
  uint8_t index = this->initial_index_();
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<uint8_t>(this->get_object_id_hash());
    uint8_t restored;
    if (this->pref_.load(&restored) && restored < this->traits.get_options().size()) {
      index = restored;
    }
  }
  this->apply_(index);
}

// THREAD CONTEXT: Main loop (invoked by the select framework)
void SnapclientIndexedSelect::control(const std::string &value) {
  auto index = this->index_of(value);
  if (!index.has_value()) {
    return;
  }
  this->apply_(*index);
  if (this->restore_value_) {
    uint8_t stored = *index;
    this->pref_.save(&stored);
  }
}

void SnapclientIndexedSelect::apply_(uint8_t index) {
  // Option order matches the corresponding client enum
  this->apply_index_(index);
  this->publish_state(this->traits.get_options()[index]);
}

void SnapclientChannelModeSelect::dump_config() { LOG_SELECT("", "Snapclient Channel Mode Select", this); }

void SnapclientPhaseSelect::dump_config() { LOG_SELECT("", "Snapclient Phase Select", this); }

static const char AUTOMATIC_OPTION[] = "Automatic";

/// Extracts host/port from a "name (host:port)" option; false for anything else
/// (including "Automatic"), which means automatic selection.
static bool parse_server_option(const std::string &option, std::string &host, uint16_t &port) {
  const size_t open = option.rfind('(');
  const size_t close = option.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open + 1) {
    return false;
  }
  std::string host_port = option.substr(open + 1, close - open - 1);
  port = 0;
  const size_t colon = host_port.rfind(':');
  if (colon != std::string::npos) {
    char *end = nullptr;
    const long parsed = strtol(host_port.c_str() + colon + 1, &end, 10);
    if (end != nullptr && *end == '\0' && parsed > 0 && parsed <= 65535) {
      port = static_cast<uint16_t>(parsed);
      host_port.resize(colon);
    }
  }
  if (host_port.empty()) {
    return false;
  }
  host = host_port;
  return true;
}

void SnapclientServerSelect::setup() {
  this->parent_->enable_server_discovery();
  this->parent_->add_discovered_servers_callback(
      [this](const std::vector<ServerCandidate> &servers) { this->on_servers_discovered_(servers); });

  std::string initial = AUTOMATIC_OPTION;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<StoredOption>(this->get_object_id_hash());
    StoredOption stored{};
    if (this->pref_.load(&stored) && stored.value[0] != '\0') {
      stored.value[sizeof(stored.value) - 1] = '\0';
      initial = stored.value;
    }
  }
  if (initial != AUTOMATIC_OPTION) {
    // Keep the restored choice selectable until discovery repopulates the list
    this->set_dynamic_options_({AUTOMATIC_OPTION, initial});
  }
  this->apply_(initial);
}

void SnapclientDynamicSelect::set_dynamic_options_(std::vector<std::string> &&options) {
  this->option_store_ = std::move(options);
  FixedVector<const char *> ptrs;
  ptrs.init(this->option_store_.size());
  for (const auto &option : this->option_store_) {
    ptrs.push_back(option.c_str());
  }
  this->traits.set_options(ptrs);
}

// THREAD CONTEXT: Main loop (invoked by the select framework)
void SnapclientServerSelect::control(const std::string &value) {
  this->apply_(value);
  if (this->restore_value_) {
    StoredOption stored{};
    if (value != AUTOMATIC_OPTION) {
      strncpy(stored.value, value.c_str(), sizeof(stored.value) - 1);
    }
    this->pref_.save(&stored);
  }
}

void SnapclientServerSelect::apply_(const std::string &value) {
  std::string host;
  uint16_t port = 0;
  const bool overridden = value != AUTOMATIC_OPTION && parse_server_option(value, host, port);
  if (value != AUTOMATIC_OPTION && !overridden) {
    ESP_LOGW(TAG, "Unparsable server option '%s'; selecting automatic", value.c_str());
  }
  this->parent_->set_server_selection(host, port);  // empty host = automatic
  this->publish_state(overridden ? value : AUTOMATIC_OPTION);
}

// THREAD CONTEXT: Main loop (hub callback)
void SnapclientServerSelect::on_servers_discovered_(const std::vector<ServerCandidate> &servers) {
  // Copy the current option before the store is replaced under it
  const std::string current = this->has_state() ? this->current_option().str() : std::string(AUTOMATIC_OPTION);
  std::vector<std::string> options{AUTOMATIC_OPTION};
  for (const auto &s : servers) {
    options.push_back(str_sprintf("%s (%s:%u)", s.name.c_str(), s.host.c_str(), s.port));
  }
  // Keep the current selection listed even if its server vanished, so the published
  // state stays a valid option (and the user's choice survives a flaky scan)
  if (current != AUTOMATIC_OPTION && std::find(options.begin(), options.end(), current) == options.end()) {
    options.push_back(current);
  }
  this->set_dynamic_options_(std::move(options));
  this->publish_state(current);
}

void SnapclientServerSelect::dump_config() { LOG_SELECT("", "Snapclient Server Select", this); }

#ifdef SNAPCLIENT_BSSID_SELECT

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

void SnapclientBssidSelect::setup() {
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
void SnapclientBssidSelect::control(const std::string &value) {
  wifi::bssid_t parsed;
  this->desired_ = (value != AUTOMATIC_OPTION && parse_bssid(value, parsed)) ? value : "";
  if (this->restore_value_) {
    StoredOption stored{};
    strncpy(stored.value, this->desired_.c_str(), sizeof(stored.value) - 1);
    this->pref_.save(&stored);
  }
  this->apply_lock_();
  this->rebuild_options_();
}

void SnapclientBssidSelect::apply_lock_() {
  auto *w = wifi::global_wifi_component;
  wifi::WiFiAP sta = w->get_sta();
  if (sta.get_ssid().empty()) {
    // Pre-first-connect the selected STA config is empty; stamping a BSSID onto it
    // would wipe the credentials. on_wifi_connect_state re-applies once connected.
    return;
  }
  wifi::bssid_t target;
  if (!this->desired_.empty() && parse_bssid(this->desired_, target)) {
    sta.set_bssid(target);
    w->set_sta(sta);
    if (w->is_connected() && w->wifi_bssid() != target) {
      ESP_LOGI(TAG, "On wrong AP; reconnecting to pinned %s", this->desired_.c_str());
      w->retry_connect();
    }
  } else {
    sta.clear_bssid();
    w->set_sta(sta);
  }
}

void SnapclientBssidSelect::on_wifi_connect_state(StringRef ssid, std::span<const uint8_t, 6> bssid) {
  if (ssid.empty()) {
    return;  // disconnect
  }
  this->network_ssid_ = ssid.str();
  this->apply_lock_();
  this->rebuild_options_();
}

void SnapclientBssidSelect::on_wifi_scan_results(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results) {
  this->scan_snapshot_.clear();
  for (const auto &r : results) {
    this->scan_snapshot_.emplace_back(r.get_ssid().str(), bssid_to_string(r.get_bssid().data()));
  }
  this->rebuild_options_();
}

void SnapclientBssidSelect::rebuild_options_() {
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

void SnapclientBssidSelect::dump_config() { LOG_SELECT("", "Snapclient BSSID Select", this); }

#endif  // SNAPCLIENT_BSSID_SELECT

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_SELECT
