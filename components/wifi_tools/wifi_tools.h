#pragma once

// Radio-level WiFi utilities, split out of the snapclient example configs where they
// lived as YAML lambdas. Nothing here is Snapcast-specific; what it encodes is
// hard-won knowledge about the esp_wifi driver that is worth keeping as versioned,
// compiled code rather than as comments in three board files.
//
// It also confines <esp_wifi.h> to one translation unit. Previously every board file
// needed `esphome: includes: [<esp_wifi.h>]` purely so a lambda could call into it.

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/core/component.h"

namespace esphome::wifi_tools {

/// @brief Set the radio's maximum TX power and report what the driver ACTUALLY took.
///
/// Never trust the requested value. The driver works in 0.25 dBm units and quantizes
/// the request into bins (esp_wifi.h: [80,84] -> 80 = 20 dBm, [34,43] -> 34 = 8.5 dBm),
/// then clamps to the PHY init data bin and the country's max_tx_power. So the only
/// meaningful number is the readback, which is why this logs it.
///
/// Diagnostic worth remembering: if the readback RISES but the AP's RSSI for this
/// device does not move, the extra power is being reflected by a badly matched antenna
/// rather than radiated. That is the ESP32-S3 SuperMini failure mode -- it misassociates
/// at high power while showing full RX bars.
///
/// @param dbm requested power in dBm
/// @return the power the driver reports, in dBm (negative on error)
float set_max_tx_power(float dbm);

/// @brief Periodic radio forensics at DEBUG: RSSI and the negotiated PHY mode.
///
/// The PHY mode is the useful half -- a fallback from HT20 down to 11G or 11B is the
/// classic marginal-link tell, and it appears next to the consumer's own diagnostics
/// so a stall's cause (retries vs starvation vs roam) can be read off one timeline.
class WifiDiagnostics : public PollingComponent {
 public:
  float get_setup_priority() const override;
  void dump_config() override;
  void update() override;

  /// @brief Also dump the driver's RX/TX and HW counters each tick.
  ///
  /// Off by default, deliberately. It emits a large multi-line block through the
  /// esp-idf logger on every tick, and on hardware its output never appeared in a
  /// serial capture at all -- so it is a live suspect for the device-side log stalls
  /// seen while idle, and enabling it costs bandwidth for something unproven.
  void set_dump_statistics(bool dump) { this->dump_statistics_ = dump; }

 protected:
  bool dump_statistics_{false};
};

}  // namespace esphome::wifi_tools

#endif  // USE_ESP32
