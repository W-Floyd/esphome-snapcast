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

  /// @brief Reports audio buffered downstream (mixer source queue), so the client can anchor its
  /// playout accounting to the measured fill rather than assuming one.
  /// THREAD CONTEXT: the client's player task, same as on_audio_write().
  bool on_query_latency(uint32_t &microseconds) override;

 protected:
  /// @brief Updates the source state and keeps the client's output-active flag in step.
  void set_playback_state_(media_source::MediaSourceState state);

  /// @brief Applies the latest server-pushed volume/mute state, coalesced to once
  /// per loop and volume-gated by the local-drag grace window.
  void apply_server_state_();

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

  // Local-wins-recently: server volume pushes are deferred while a local slider drag
  // is in flight (see the server settings callback).
  static constexpr uint32_t LOCAL_VOLUME_GRACE_MS = 1500;
  uint32_t last_local_volume_ms_{0};

  // Local state mirrored from the orchestrator, reported to the server on change
  uint8_t local_volume_{100};
  bool local_muted_{false};

  bool pending_start_{false};
  uint32_t start_requested_ms_{0};  // when pending_start_ was raised, for a retry timeout
  bool stream_live_{false};         // the client is receiving chunks
  // Any deliberate halt -- PAUSE or STOP, from a person or a Home Assistant
  // automation -- must never be undone by the re-arm below.
  //
  // STOP needs the flag because it sets IDLE, which is precisely what the re-arm
  // looks for. PAUSE looks safe without it (PAUSED is not IDLE) but is not: if the
  // pipeline later times out and the framework drops the source to IDLE, the pause
  // would be forgotten and playback would resume against the automation that stopped
  // it. Cleared only by an explicit PLAY or play_uri.
  bool user_halted_{false};
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_MEDIA_SOURCE
