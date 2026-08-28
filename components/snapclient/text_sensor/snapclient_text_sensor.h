#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_TEXT_SENSOR)

#include "esphome/components/snapclient/snapclient_hub.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome::snapclient {

/// @brief Publishes this device's TSF group-sync role: "Leader" / "Follower" /
/// "Inactive" (tsf_sync off or unsupported, no session, or no mapping yet).
/// Polled from loop() with change detection — the underlying read is one atomic.
class SnapclientTsfStateTextSensor final : public SnapclientChild, public text_sensor::TextSensor {
 public:
  void setup() override { this->publish_(); }
  void loop() override { this->publish_(); }
  void dump_config() override;

 protected:
  void publish_();

  TsfState last_state_{TsfState::INACTIVE};
  bool published_{false};
};

/// @brief Publishes the format snapserver is actually sending, as "48000 Hz, 16 bit,
/// 2 ch" — the same wording the codec-header log line uses. Empty while no stream is
/// active.
///
/// The rate is negotiated at runtime rather than fixed by the YAML: the codec header
/// decides it, the speaker is reconfigured to match, and `rate_lock` re-baselines. This
/// makes the value visible, which matters because a rate that disagrees with the
/// `media_pipeline` costs a speaker reconfigure and a dropped buffer at every stream
/// start. It is deliberately NOT wired into the media player's advertised formats:
/// those describe what Home Assistant should ENCODE FOR US, which is unrelated traffic.
class SnapclientStreamFormatTextSensor final : public SnapclientChild, public text_sensor::TextSensor {
 public:
  void setup() override;
  void dump_config() override;

 protected:
  void publish_(bool active, const StreamParams &params);

  std::string last_;
  bool published_{false};
};

/// @brief Publishes one field of the current stream's metadata (title / artist /
/// album / stream name), live from the persistent control session. Empty when the
/// control port is unavailable or the stream carries no metadata.
class SnapclientMetadataTextSensor final : public SnapclientChild, public text_sensor::TextSensor {
 public:
  void setup() override;
  void dump_config() override;

  void set_field(MetadataField field) { this->field_ = field; }

 protected:
  void publish_(const StreamMetadata &metadata);

  MetadataField field_{MetadataField::TITLE};
  std::string last_;
  bool published_{false};
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_TEXT_SENSOR
