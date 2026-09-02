#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_NUMBER)

#include "esphome/components/number/number.h"
#include "esphome/components/snapclient/snapclient_hub.h"
#include "esphome/core/preferences.h"

namespace esphome::snapclient {

/// @brief Number entity for this client's latency setting on the snapserver.
///
/// Writes go out as Client.SetLatency on the server's control API; state mirrors the
/// latency field the server pushes in ServerSettings, so the server stays the single
/// source of truth (no local persistence, correct even when another controller
/// changes it).
class SnapclientServerLatencyNumber final : public SnapclientChild, public number::Number {
 public:
  void setup() override;
  void dump_config() override;

 protected:
  void control(float value) override;
};

/// @brief Number entity backed by set_servo_param(), one class for every tunable.
///
/// The parameter name is set at codegen time, so adding a knob to the frontend is a schema entry
/// rather than a new class. Values are restored from NVS on boot and re-applied, since a knob the
/// user moved should survive a reboot.
class SnapclientServoParamNumber final : public SnapclientChild, public number::Number {
 public:
  void setup() override;
  void dump_config() override;
  void set_param(const std::string &name) { this->param_ = name; }
  /// A knob that must NOT restore-and-reapply at boot. For a quantity the firmware LEARNS, where
  /// the learned value has its own persistence: restoring here would overwrite it with whatever
  /// was last typed, every boot, and the learner would never keep its own answer.
  void set_no_restore(bool v) { this->no_restore_ = v; }
  void loop() override;

 protected:
  void control(float value) override;

  std::string param_;
  bool no_restore_{false};
  uint32_t last_track_ms_{0};
  float last_shown_{NAN};
  ESPPreferenceObject pref_;
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_NUMBER
