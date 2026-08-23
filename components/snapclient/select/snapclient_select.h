#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_SELECT)

#include "esphome/components/select/select.h"
#include "esphome/components/snapclient/snapclient_hub.h"
#include "esphome/core/preferences.h"


namespace esphome::snapclient {

/// @brief Select with runtime-built options. The traits hold raw const char*
/// pointers (normally codegen string literals), so the entity must own the backing
/// strings for as long as the traits reference them.
class SnapclientDynamicSelect : public select::Select {
 protected:
  /// @brief Replaces the option list, taking ownership of the strings.
  void set_dynamic_options_(std::vector<std::string> &&options);

  std::vector<std::string> option_store_;
};

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
  // A pin is a PREFERENCE, not a requirement: wifi's set_bssid() will refuse to
  // associate with anything else, so an AP that is gone, overloaded, or simply out of
  // range leaves the client unable to connect at all rather than degraded. Track how
  // long we have been pinned-but-disconnected and drop the constraint past a timeout.
  uint32_t pinned_since_ms_{0};  // 0 = not waiting on a pinned connect
  bool pin_released_{false};     // pin dropped after timing out; re-armed on next drop
  ESPPreferenceObject pref_;
};

/// @brief Output channel routing (Stereo / Left / Right / Mono).
///
/// The equivalent of esp32 snapclient's web-UI channel mode: switchable at runtime
/// (e.g. two devices sharing a stereo stream as a L/R pair).
///
/// With `mono_dac: true` (MAX98357A-style summing amps) the options collapse to
/// Mix / Left / Right: "Stereo" would just be an analog (L+R)/2 anyway, so the
/// defined digital mix (MONO) stands in for it and the redundant entry disappears.
class SnapclientChannelModeSelect final : public SnapclientIndexedSelect {
 public:
  void dump_config() override;

  void set_mono_dac(bool mono) { this->mono_dac_ = mono; }

 protected:
  void apply_index_(uint8_t index) override {
    ChannelMode mode;
    if (this->mono_dac_) {
      // Options: Mix / Left / Right
      mode = index == 0 ? ChannelMode::MONO : (index == 1 ? ChannelMode::LEFT_ONLY : ChannelMode::RIGHT_ONLY);
    } else {
      mode = static_cast<ChannelMode>(index);
    }
    this->parent_->set_channel_mode(mode);
  }
  uint8_t initial_index_() const override {
    const ChannelMode mode = this->parent_->get_channel_mode();
    if (this->mono_dac_) {
      switch (mode) {
        case ChannelMode::LEFT_ONLY:
          return 1;
        case ChannelMode::RIGHT_ONLY:
          return 2;
        default:
          return 0;  // MONO, and STEREO folds into Mix
      }
    }
    return static_cast<uint8_t>(mode);
  }

  bool mono_dac_{false};
};

/// @brief Output polarity inversion, for correcting an out-of-phase driver in
/// software (an inverted speaker in a synchronized pair cancels bass with its
/// partner).
///
/// Options are None / Left / Right / Both — except with `mono_dac: true`
/// (MAX98357A-style summing amps, which analog-mix L+R: inverting a single side of
/// duplicated content cancels to silence), where they collapse to None / Inverted
/// (mapped to NONE / BOTH).
class SnapclientPhaseSelect final : public SnapclientIndexedSelect {
 public:
  void dump_config() override;

  void set_mono_dac(bool mono) { this->mono_dac_ = mono; }

 protected:
  void apply_index_(uint8_t index) override {
    const PhaseMode mode =
        this->mono_dac_ ? (index == 0 ? PhaseMode::NONE : PhaseMode::BOTH) : static_cast<PhaseMode>(index);
    this->parent_->set_phase_mode(mode);
  }
  uint8_t initial_index_() const override {
    const PhaseMode mode = this->parent_->get_phase_mode();
    if (this->mono_dac_) {
      return mode == PhaseMode::NONE ? 0 : 1;
    }
    return static_cast<uint8_t>(mode);
  }

  bool mono_dac_{false};
};

/// @brief Picker over mDNS-discovered snapservers ("Automatic" + one option per
/// server, labeled "name (host:port)").
///
/// Selecting a server overrides the connection target (below a manual server text
/// entry in precedence; see SnapclientHub::set_server_selection). Options refresh
/// from the periodic discovery scans; note Home Assistant only reloads a select's
/// option list when its API connection to the device is re-established, while the
/// built-in web server always shows the live list.
class SnapclientServerSelect final : public SnapclientChild, public SnapclientDynamicSelect {
 public:
  void setup() override;
  void dump_config() override;

  void set_restore_value(bool restore) { this->restore_value_ = restore; }

 protected:
  void control(const std::string &value) override;
  /// @brief Pushes @p value's target to the hub and publishes the state.
  void apply_(const std::string &value);
  void on_servers_discovered_(const std::vector<ServerCandidate> &servers);

  // Restored option string ("Automatic" persists as empty)
  struct StoredOption {
    char value[65];
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

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_SELECT
