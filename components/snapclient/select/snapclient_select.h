#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_SELECT)

#include "esphome/components/select/select.h"
#include "esphome/components/snapclient/snapclient_hub.h"
#include "esphome/core/preferences.h"

namespace esphome::snapclient {

/// @brief Base for snapclient's index-mapped select entities: the option index maps
/// 1:1 onto a client enum, restored from flash at setup (the persisted value takes
/// precedence over the hub's YAML default) and persisted on change.
class SnapclientIndexedSelect : public SnapclientChild, public select::Select {
 public:
  void setup() override;

  void set_restore_value(bool restore) { this->restore_value_ = restore; }

 protected:
  void control(const std::string &value) override;

  /// @brief Applies @p index to the hub and publishes the matching option.
  void apply_(uint8_t index);
  /// @brief Pushes @p index into the hub as the concrete enum.
  virtual void apply_index_(uint8_t index) = 0;
  /// @brief The hub's boot-default index for this select.
  virtual uint8_t initial_index_() const = 0;

  bool restore_value_{true};
  ESPPreferenceObject pref_;
};

/// @brief Output channel routing (Stereo / Left / Right / Mono).
///
/// The equivalent of esp32 snapclient's web-UI channel mode: switchable at runtime
/// (e.g. two devices sharing a stereo stream as a L/R pair).
class SnapclientChannelModeSelect final : public SnapclientIndexedSelect {
 public:
  void dump_config() override;

 protected:
  void apply_index_(uint8_t index) override {
    this->parent_->set_channel_mode(static_cast<ChannelMode>(index));
  }
  uint8_t initial_index_() const override { return static_cast<uint8_t>(this->parent_->get_channel_mode()); }
};

/// @brief Output polarity inversion (None / Left / Right / Both), for correcting an
/// out-of-phase driver in software (an inverted speaker in a synchronized pair
/// cancels bass with its partner).
class SnapclientPhaseSelect final : public SnapclientIndexedSelect {
 public:
  void dump_config() override;

 protected:
  void apply_index_(uint8_t index) override { this->parent_->set_phase_mode(static_cast<PhaseMode>(index)); }
  uint8_t initial_index_() const override { return static_cast<uint8_t>(this->parent_->get_phase_mode()); }
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_SELECT
