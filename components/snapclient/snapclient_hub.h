#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "snapcast_client.h"
#include "volume_curve.h"

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <memory>
#include <string>

namespace esphome::snapclient {

/// @brief Setup priorities for the snapclient hub and its child components.
///
/// Centralized here so every snapclient component orders itself relative to the hub
/// without each subcomponent having to pick a priority independently. Children run
/// one step later than the hub so they can assume the hub's setup() has completed.
namespace snapclient_priority {
// AFTER_WIFI so the hub runs after the wifi/ethernet drivers are up and we can read
// the active interface's MAC for the client ID.
inline constexpr float HUB = esphome::setup_priority::AFTER_WIFI;
inline constexpr float CHILD = HUB - 1.0f;
}  // namespace snapclient_priority

/// @brief Thin adapter over SnapcastClient.
///
/// The hub owns a SnapcastClient instance and bridges its listener interface to
/// ESPHome's CallbackManager for fan-out to child components, mirrors network
/// readiness into the client's tasks, and manages wifi power state while streaming.
///
/// Follows the SendspinHub design:
///  - Configuration is passed at client construction time as a struct, built in setup().
///  - Client -> component communication happens via the listener interface, dispatched
///    on the main loop from client_->loop().
///  - Component -> client communication uses exposed functions on the client object.
class SnapclientHub final : public Component, public SnapcastClientListener {
 public:
  float get_setup_priority() const override { return snapclient_priority::HUB; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  // --- Configuration setters (called from codegen) ---

  void set_server(const std::string &host, uint16_t port) {
    this->server_host_ = host;
    this->server_port_ = port;
  }
  void set_client_name(const std::string &name) { this->client_name_ = name; }
  void set_buffer_size(size_t buffer_size) { this->buffer_size_ = buffer_size; }
  void set_time_sync_interval(uint32_t interval_ms) { this->time_sync_interval_ms_ = interval_ms; }
  void set_hard_resync_threshold(uint32_t threshold_ms) { this->hard_resync_threshold_ms_ = threshold_ms; }
  void set_stream_idle_timeout(uint32_t timeout_ms) { this->stream_idle_timeout_ms_ = timeout_ms; }
  void set_sync_deadband(uint32_t deadband_us) { this->sync_deadband_us_ = deadband_us; }

  // --- Child component API (main loop thread) ---

  /// @brief Registers the single audio sink (the snapclient media source).
  void set_audio_listener(SnapcastAudioListener *audio_listener);

  /// @brief Enables/disables synchronized audio output. While disabled, chunks are
  /// discarded at their deadline so playback resumes in sync when re-enabled.
  void set_output_active(bool active);

  /// @brief Per-device latency trim applied to every chunk deadline.
  void set_static_delay_ms(int32_t delay_ms);

  /// @brief Output channel routing (stereo / left / right / mono). Safe at runtime.
  void set_channel_mode(ChannelMode mode);
  ChannelMode get_channel_mode() const { return this->channel_mode_; }

  /// @brief Output polarity inversion (none / left / right / both). Safe at runtime.
  void set_phase_mode(PhaseMode mode);
  PhaseMode get_phase_mode() const { return this->phase_mode_; }

  /// @brief Volume taper between the Snapcast volume slider and the speaker gain.
  /// 0 dB = linear (off). Fires the volume-curve callbacks so the media source
  /// re-applies the current volume with the new curve.
  void set_volume_curve_db_range(float db_range);
  const VolumeCurve &get_volume_curve() const { return this->volume_curve_; }

  template<typename F> void add_volume_curve_callback(F &&callback) {
    this->volume_curve_callbacks_.add(std::forward<F>(callback));
  }

  /// @brief Reports a local volume/mute change to the server (ClientInfo message).
  void send_client_volume(uint8_t volume_percent, bool muted);

  /// @brief Feed the speaker's audio output callback into the sync engine.
  /// THREAD CONTEXT: speaker task; safe to call from any thread.
  void notify_audio_played(uint32_t frames, int64_t timestamp_us);

  template<typename F> void add_connection_callback(F &&callback) {
    this->connection_callbacks_.add(std::forward<F>(callback));
  }
  /// @brief Fires with (volume_percent, muted) whenever the server pushes settings.
  template<typename F> void add_server_settings_callback(F &&callback) {
    this->server_settings_callbacks_.add(std::forward<F>(callback));
  }
  /// @brief Fires with true and the stream format on stream start, false on stream end.
  template<typename F> void add_stream_state_callback(F &&callback) {
    this->stream_state_callbacks_.add(std::forward<F>(callback));
  }

 protected:
  // --- SnapcastClientListener overrides ---
  // THREAD CONTEXT: Main loop (dispatched from client_->loop())
  void on_connection_changed(bool connected) override;
  void on_server_settings(const ServerSettings &settings) override;
  void on_stream_start(const StreamParams &params) override;
  void on_stream_end() override;

  std::unique_ptr<SnapcastClient> client_;

  // Configuration (from codegen)
  std::string server_host_;
  uint16_t server_port_{1704};
  std::string client_name_;
  size_t buffer_size_{524288};
  uint32_t time_sync_interval_ms_{1000};
  uint32_t hard_resync_threshold_ms_{50};
  uint32_t stream_idle_timeout_ms_{3000};
  uint32_t sync_deadband_us_{2000};

  // Deferred child registrations from before setup() ran
  SnapcastAudioListener *pending_audio_listener_{nullptr};
  int32_t pending_static_delay_ms_{0};

  ChannelMode channel_mode_{ChannelMode::STEREO};
  PhaseMode phase_mode_{PhaseMode::NONE};
  VolumeCurve volume_curve_{};

  // Callback fan-out to child components
  CallbackManager<void(bool)> connection_callbacks_{};
  CallbackManager<void(uint8_t, bool)> server_settings_callbacks_{};
  CallbackManager<void(bool, const StreamParams &)> stream_state_callbacks_{};
  CallbackManager<void()> volume_curve_callbacks_{};
};

/// @brief Base class for all snapclient subcomponents.
///
/// Consolidates the Component + Parented<SnapclientHub> inheritance and pins the setup
/// priority so the hub's setup() always runs before any child. Subcomponents must not
/// override get_setup_priority().
class SnapclientChild : public Component, public Parented<SnapclientHub> {
 public:
  float get_setup_priority() const override { return snapclient_priority::CHILD; }
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32
