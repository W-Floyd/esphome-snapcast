#include "snapclient_hub.h"

#ifdef USE_ESP32

#include "esphome/components/network/util.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::snapclient {

static const char *const TAG = "snapclient.hub";

void SnapclientHub::setup() {
  SnapcastClientConfig config;
  config.server_host = this->server_host_;
  config.server_port = this->server_port_;
  // Hello HostName, which the server uses as the client's default display name:
  // explicit `name:` config > the device's friendly_name > the node name.
  if (!this->client_name_.empty()) {
    config.hostname = this->client_name_;
  } else if (!App.get_friendly_name().empty()) {
    config.hostname = App.get_friendly_name();
  } else {
    config.hostname = App.get_name();
  }
  config.client_id = get_mac_address_pretty();
  config.buffer_size = this->buffer_size_;
  config.time_sync_interval_ms = this->time_sync_interval_ms_;
  config.hard_resync_threshold_ms = this->hard_resync_threshold_ms_;
  config.stream_idle_timeout_ms = this->stream_idle_timeout_ms_;
  config.keepalive_hold_ms = this->keepalive_hold_ms_;
  config.reanchor_after_reconnect = this->reanchor_after_reconnect_;
  config.fast_splice_threshold_us = this->fast_splice_threshold_us_;
  config.render_align_max_us = this->render_align_max_us_;
  config.tsf_observer = this->tsf_observer_;
  config.sync_deadband_us = this->sync_deadband_us_;
  config.converge_fine_us = this->converge_fine_us_;
#ifdef USE_I2S_RATE_LOCK
  config.rate_lock_i2s_port = this->rate_lock_port_;
#endif

  this->client_ = std::make_unique<SnapcastClient>(std::move(config));
  this->client_->set_listener(this);
  if (this->pending_audio_listener_ != nullptr) {
    this->client_->set_audio_listener(this->pending_audio_listener_);
  }
  this->client_->set_static_delay_ms(this->pending_static_delay_ms_);
  this->client_->set_channel_mode(this->channel_mode_);
  this->client_->set_phase_mode(this->phase_mode_);
  this->client_->set_sync_resilience(this->sync_resilience_);

  if (!this->client_->start()) {
    ESP_LOGE(TAG, "Failed to start Snapcast client");
    this->mark_failed();
    return;
  }

#ifdef USE_OTA_STATE_LISTENER
  ota::get_global_ota_callback()->add_global_state_listener(this);
#endif
}

#ifdef USE_OTA_STATE_LISTENER
void SnapclientHub::on_ota_global_state(ota::OTAState state, float progress, uint8_t error,
                                        ota::OTAComponent *component) {
  if (state == ota::OTA_STARTED) {
    ESP_LOGI(TAG, "OTA started; pausing snapclient to free the radio");
    this->ota_paused_ = true;
    if (this->client_ != nullptr) {
      this->client_->set_network_ready(false);
      this->client_->request_disconnect();
    }
  } else if (state == ota::OTA_ERROR || state == ota::OTA_ABORT) {
    ESP_LOGI(TAG, "OTA ended without reboot; resuming snapclient");
    this->ota_paused_ = false;  // loop() re-mirrors network readiness
  }
  // OTA_COMPLETED reboots the device; nothing to restore
}
#endif

void SnapclientHub::loop() {
  // network::is_connected() is main-loop-only; mirror it into the client's tasks
  bool ready = network::is_connected();
#ifdef USE_OTA_STATE_LISTENER
  ready = ready && !this->ota_paused_;
#endif
  this->client_->set_network_ready(ready);
  this->client_->loop();
}

void SnapclientHub::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Snapclient Hub:\n"
                "  Server: %s:%u\n"
                "  Discovery: %s\n"
                "  Client ID: %s\n"
                "  Buffer size: %zu bytes\n"
                "  Time sync interval: %" PRIu32 " ms\n"
                "  Hard resync threshold: %" PRIu32 " ms\n"
                "  Stream idle timeout: %" PRIu32 " ms",
                this->server_host_.empty() ? "(mDNS)" : this->server_host_.c_str(), this->server_port_,
                this->server_host_.empty() ? "mDNS (_snapcast._tcp)" : "static host", get_mac_address_pretty().c_str(),
                this->buffer_size_, this->time_sync_interval_ms_, this->hard_resync_threshold_ms_,
                this->stream_idle_timeout_ms_);
#ifdef USE_I2S_RATE_LOCK
  ESP_LOGCONFIG(TAG, "  Rate lock: I2S%u clock steering", this->rate_lock_port_);
#endif
  if (this->client_ != nullptr && this->client_->is_connected()) {
    ESP_LOGCONFIG(TAG, "  Connected, clock offset: %.2f ms", this->client_->get_clock_offset_ms());
  }
}

// --- Child component API ---
// THREAD CONTEXT: Main loop (invoked from snapclient components)

void SnapclientHub::set_audio_listener(SnapcastAudioListener *audio_listener) {
  this->pending_audio_listener_ = audio_listener;
  if (this->client_ != nullptr) {
    this->client_->set_audio_listener(audio_listener);
  }
}

void SnapclientHub::set_output_active(bool active) {
  if (this->client_ != nullptr) {
    this->client_->set_output_active(active);
  }
}

bool SnapclientHub::output_active() const {
  return this->client_ != nullptr && this->client_->output_active();
}

void SnapclientHub::set_static_delay_ms(int32_t delay_ms) {
  this->pending_static_delay_ms_ = delay_ms;
  if (this->client_ != nullptr) {
    this->client_->set_static_delay_ms(delay_ms);
  }
}

void SnapclientHub::set_channel_mode(ChannelMode mode) {
  this->channel_mode_ = mode;
  if (this->client_ != nullptr) {
    this->client_->set_channel_mode(mode);
  }
}

void SnapclientHub::set_sync_resilience(SyncResilience level) {
  this->sync_resilience_ = level;
  if (this->client_ != nullptr) {
    this->client_->set_sync_resilience(level);
  }
}

void SnapclientHub::set_phase_mode(PhaseMode mode) {
  this->phase_mode_ = mode;
  if (this->client_ != nullptr) {
    this->client_->set_phase_mode(mode);
  }
}

void SnapclientHub::send_client_volume(uint8_t volume_percent, bool muted) {
  if (this->client_ != nullptr) {
    this->client_->send_client_info(volume_percent, muted);
  }
}

// THREAD CONTEXT: Speaker playback callback thread (forwarded from the media source);
// SnapcastClient::notify_audio_played is internally synchronized.
void SnapclientHub::inject_starvation(uint32_t ms) {
  if (this->client_ != nullptr) {
    ESP_LOGW(TAG, "Injecting starvation: discarding audio for %" PRIu32 " ms", ms);
    this->client_->inject_starvation(ms);
  }
}

void SnapclientHub::inject_split(int32_t us) {
  if (this->client_ != nullptr) {
    ESP_LOGW(TAG, "Injecting accounting split: %+" PRId32 " us (audio untouched)", us);
    this->client_->inject_split(us);
  }
}

void SnapclientHub::on_shutdown() {
  if (this->client_ != nullptr) {
    this->client_->persist_now();
  }
}

void SnapclientHub::set_servo_param(const std::string &name, float value) {
  if (this->client_ != nullptr) {
    if (!this->client_->set_servo_param(name, value)) {
      ESP_LOGW(TAG, "servo_param refused: %s=%.3f (unknown name or out of bounds)", name.c_str(), value);
    }
  }
}

void SnapclientHub::notify_audio_played(uint32_t frames, int64_t timestamp_us) {
  if (this->client_ != nullptr) {
    this->client_->notify_audio_played(frames, timestamp_us);
  }
}

void SnapclientHub::notify_audio_played_tagged(uint32_t frames, int64_t adjusted_ts, const audio::RenderTag &tag) {
  if (this->client_ != nullptr) {
    this->client_->notify_audio_played_tagged(frames, adjusted_ts, tag);
  }
}

void SnapclientHub::set_server_manual(const std::string &host, uint16_t port) {
  this->manual_host_ = host;
  this->manual_port_ = port;
  this->apply_server_override_();
}

void SnapclientHub::set_server_selection(const std::string &host, uint16_t port) {
  this->selected_host_ = host;
  this->selected_port_ = port;
  this->apply_server_override_();
}

void SnapclientHub::apply_server_override_() {
  if (this->client_ == nullptr) {
    return;
  }
  if (!this->manual_host_.empty()) {
    this->client_->set_server_override(this->manual_host_, this->manual_port_);
  } else {
    this->client_->set_server_override(this->selected_host_, this->selected_port_);
  }
}

void SnapclientHub::enable_server_discovery() {
  if (this->client_ != nullptr) {
    this->client_->set_discovery_enabled(true);
  }
}

// --- SnapcastClientListener overrides ---
// THREAD CONTEXT: Main loop (dispatched from client_->loop())

void SnapclientHub::on_connection_changed(bool connected) { this->connection_callbacks_.call(connected); }

void SnapclientHub::on_server_settings(const ServerSettings &settings) {
  this->server_settings_callbacks_.call(settings.volume, settings.muted, settings.latency);
}

void SnapclientHub::on_servers_discovered(const std::vector<ServerCandidate> &servers) {
  this->discovered_servers_callbacks_.call(servers);
}

void SnapclientHub::on_stream_metadata(const StreamMetadata &metadata) {
  // Scope TSF leadership to the stream: render_phase is only comparable between devices playing
  // the same one, so a group spanning two streams produces deltas that are not playout offsets.
  if (this->client_ != nullptr) {
    this->client_->set_stream_identity(metadata.stream_name);
  }
  this->metadata_callbacks_.call(metadata);
}

void SnapclientHub::set_server_latency(int32_t latency_ms) {
  if (this->client_ != nullptr) {
    this->client_->set_server_latency(latency_ms);
  }
}

void SnapclientHub::on_stream_start(const StreamParams &params) {
#ifdef USE_WIFI
  // Streaming needs low-latency wifi: modem power save adds tens of ms of jitter,
  // and a mid-stream roaming scan takes the radio off-channel for hundreds of ms —
  // long enough to starve the DAC (same pairing sendspin uses).
  if (wifi::global_wifi_component != nullptr) {
    wifi::global_wifi_component->request_high_performance();
    wifi::global_wifi_component->request_roaming_suppression();
  }
#endif
  this->stream_state_callbacks_.call(true, params);
}

void SnapclientHub::on_stream_end() {
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    wifi::global_wifi_component->release_high_performance();
    wifi::global_wifi_component->release_roaming_suppression();
  }
#endif
  StreamParams empty{};
  this->stream_state_callbacks_.call(false, empty);
}

}  // namespace esphome::snapclient

#endif  // USE_ESP32
