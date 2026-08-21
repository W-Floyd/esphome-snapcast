#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_MEDIA_SOURCE)

#include "esphome/components/media_source/media_source.h"
#include "esphome/components/snapclient/snapclient_hub.h"

namespace esphome::snapclient {

/// @brief Thin adapter media source for Snapcast.
///
/// Implements SnapcastAudioListener to receive synchronized PCM from the client's
/// player task and bridges it to ESPHome's MediaSource output pipeline. Volume and
/// mute round-trip between the orchestrator and the Snapcast server: ServerSettings
/// drive the orchestrator, local changes are reported back via ClientInfo.
class SnapclientMediaSource final : public SnapclientChild,
                                    public media_source::MediaSource,
                                    public SnapcastAudioListener {
 public:
  void setup() override;
  void dump_config() override;

  void set_static_delay_ms(int32_t delay_ms) { this->static_delay_ms_ = delay_ms; }

  // MediaSource interface implementation
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  bool can_handle(const std::string &uri) const override;

  void notify_volume_changed(float volume) override;
  void notify_mute_changed(bool is_muted) override;
  void notify_audio_played(uint32_t frames, int64_t timestamp) override;

  // --- SnapcastAudioListener override ---

  /// @brief Writes synchronized PCM audio to ESPHome's media source output pipeline.
  /// THREAD CONTEXT: the client's player task; may block up to timeout_ms.
  size_t on_audio_write(const uint8_t *data, size_t length, uint32_t timeout_ms, const StreamParams &params) override;

 protected:
  /// @brief Updates the source state and keeps the client's output-active flag in step.
  void set_playback_state_(media_source::MediaSourceState state);

  /// @brief Maps a server volume slider position through the volume curve and
  /// requests the resulting gain from the orchestrator.
  void apply_server_volume_(uint8_t volume_percent);

  int32_t static_delay_ms_{0};

  // Last values the server pushed, used to suppress echoing them straight back
  int last_server_volume_{-1};
  int last_server_muted_{-1};

  // The exact gain we last requested from the orchestrator; a notify with (almost)
  // this value is our own request echoing back, not a user change.
  float last_requested_gain_{-1.0f};

  // Local state mirrored from the orchestrator, reported to the server on change
  uint8_t local_volume_{100};
  bool local_muted_{false};

  bool pending_start_{false};
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_MEDIA_SOURCE
