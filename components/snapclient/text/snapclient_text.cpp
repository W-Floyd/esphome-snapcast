#include "snapclient_text.h"

#if defined(USE_ESP32) && defined(USE_TEXT)

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cstdlib>
#include <cstring>

namespace esphome::snapclient {

static const char *const TAG = "snapclient.text";

/// Parses "host" or "host:port" (whitespace-trimmed). @p port stays 0 (configured
/// default) without a valid port suffix. @return false for an empty/blank value.
static bool parse_host_port(const std::string &value, std::string &host, uint16_t &port) {
  std::string trimmed = value;
  trimmed.erase(0, trimmed.find_first_not_of(" \t"));
  trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
  port = 0;
  const size_t colon = trimmed.rfind(':');
  if (colon != std::string::npos) {
    char *end = nullptr;
    const long parsed = strtol(trimmed.c_str() + colon + 1, &end, 10);
    if (end != nullptr && *end == '\0' && parsed > 0 && parsed <= 65535) {
      port = static_cast<uint16_t>(parsed);
      trimmed.resize(colon);
    }
  }
  if (trimmed.empty()) {
    return false;
  }
  host = trimmed;
  return true;
}

void SnapclientServerText::setup() {
  std::string initial;
  if (this->restore_value_) {
    this->pref_ = global_preferences->make_preference<StoredValue>(this->get_object_id_hash());
    StoredValue stored{};
    if (this->pref_.load(&stored)) {
      stored.value[sizeof(stored.value) - 1] = '\0';
      initial = stored.value;
    }
  }
  this->apply_(initial);
}

// THREAD CONTEXT: Main loop (invoked by the text framework)
void SnapclientServerText::control(const std::string &value) {
  this->apply_(value);
  if (this->restore_value_) {
    StoredValue stored{};
    strncpy(stored.value, value.c_str(), sizeof(stored.value) - 1);
    this->pref_.save(&stored);
  }
}

void SnapclientServerText::apply_(const std::string &value) {
  std::string host;
  uint16_t port = 0;
  if (!parse_host_port(value, host, port)) {
    host.clear();  // empty/blank: clear the manual override
  }
  this->parent_->set_server_manual(host, port);
  this->publish_state(value);
}

void SnapclientServerText::dump_config() { LOG_TEXT("", "Snapclient Server Text", this); }

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_TEXT
