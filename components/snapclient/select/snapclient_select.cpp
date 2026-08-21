#include "snapclient_select.h"

#if defined(USE_ESP32) && defined(USE_SELECT)

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

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

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_SELECT
