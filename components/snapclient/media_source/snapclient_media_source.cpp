#include "snapclient_media_source.h"

#if defined(USE_ESP32) && defined(USE_MEDIA_SOURCE)

#include "esphome/components/audio/audio.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome::snapclient {

static const char *const TAG = "snapclient.media_source";

static constexpr char URI_PREFIX[] = "snapcast://";
static constexpr char URI_CURRENT[] = "snapcast://current";

// THREAD CONTEXT: Main loop. The callbacks registered here also fire on the main loop,
// since SnapclientHub dispatches events from client_->loop().
void SnapclientMediaSource::setup() {
  this->parent_->set_static_delay_ms(this->static_delay_ms_);

  this->parent_->add_server_settings_callback([this](uint8_t volume, bool muted) {
    // Remember what the server pushed so notify_volume_changed / notify_mute_changed
    // don't echo the same values straight back as a ClientInfo message.
    this->last_server_volume_ = volume;
    this->last_server_muted_ = muted;
    this->request_volume_(volume / 100.0f);
    this->request_mute_(muted);
  });

  this->parent_->add_stream_state_callback([this](bool started, const StreamParams &params) {
    if (started) {
      ESP_LOGD(TAG, "Stream started: %" PRIu32 " Hz, %u bit, %u ch", params.sample_rate, params.bits_per_sample,
               params.channels);
      if (!this->pending_start_ && this->get_state() == media_source::MediaSourceState::IDLE) {
        // Ask the orchestrator to route audio from us
        this->pending_start_ = true;
        this->request_play_uri_(URI_CURRENT);
      }
    } else {
      ESP_LOGD(TAG, "Stream ended");
      this->set_playback_state_(media_source::MediaSourceState::IDLE);
    }
  });
}

void SnapclientMediaSource::dump_config() {
  ESP_LOGCONFIG(TAG, "Snapclient Media Source: static_delay=%" PRId32 " ms", this->static_delay_ms_);
}

// --- MediaSource interface ---

bool SnapclientMediaSource::can_handle(const std::string &uri) const { return uri.starts_with(URI_PREFIX); }

// THREAD CONTEXT: Main loop (media_source.h documents play_uri as main-loop only)
bool SnapclientMediaSource::play_uri(const std::string &uri) {
  this->pending_start_ = false;
  if (!this->is_ready() || this->is_failed() || !this->has_listener()) {
    return false;
  }
  if (!uri.starts_with(URI_PREFIX)) {
    ESP_LOGE(TAG, "Invalid URI: '%s'", uri.c_str());
    return false;
  }
  if (uri != URI_CURRENT) {
    // The server is fixed by the hub's YAML config in this version
    ESP_LOGW(TAG, "Retargeting via URI is not supported; playing the configured server");
  }
  this->set_playback_state_(media_source::MediaSourceState::PLAYING);
  return true;
}

// THREAD CONTEXT: Main loop (media_source.h documents handle_command as main-loop only)
void SnapclientMediaSource::handle_command(media_source::MediaSourceCommand command) {
  // The Snapcast binary protocol carries no transport control (that lives in the
  // server's JSON-RPC API), so pause/stop act locally. Chunks are still discarded at
  // their deadline while paused, so resuming snaps straight back into sync.
  switch (command) {
    case media_source::MediaSourceCommand::PLAY:
      this->set_playback_state_(media_source::MediaSourceState::PLAYING);
      break;
    case media_source::MediaSourceCommand::PAUSE:
      this->set_playback_state_(media_source::MediaSourceState::PAUSED);
      break;
    case media_source::MediaSourceCommand::STOP:
      this->set_playback_state_(media_source::MediaSourceState::IDLE);
      break;
    default:
      break;
  }
}

void SnapclientMediaSource::set_playback_state_(media_source::MediaSourceState state) {
  // Keep the client's output gate in lockstep with the orchestrator routing: the
  // player task only pushes (instead of discarding at deadline) while PLAYING.
  this->parent_->set_output_active(state == media_source::MediaSourceState::PLAYING);
  this->set_state_(state);
}

// THREAD CONTEXT: Main loop (orchestrator -> source notification)
void SnapclientMediaSource::notify_volume_changed(float volume) {
  const int volume_percent = static_cast<int>(std::roundf(volume * 100.0f));
  this->local_volume_ = static_cast<uint8_t>(volume_percent);
  if (volume_percent != this->last_server_volume_) {
    this->parent_->send_client_volume(this->local_volume_, this->local_muted_);
  }
}

// THREAD CONTEXT: Main loop (orchestrator -> source notification)
void SnapclientMediaSource::notify_mute_changed(bool is_muted) {
  this->local_muted_ = is_muted;
  if (static_cast<int>(is_muted) != this->last_server_muted_) {
    this->parent_->send_client_volume(this->local_volume_, this->local_muted_);
  }
}

// THREAD CONTEXT: Speaker playback callback thread (forwarded from the speaker).
// SnapclientHub::notify_audio_played is documented as thread-safe for this use.
void SnapclientMediaSource::notify_audio_played(uint32_t frames, int64_t timestamp) {
  this->parent_->notify_audio_played(frames, timestamp);
}

// --- SnapcastAudioListener override ---

// THREAD CONTEXT: Snapclient player task. May block up to timeout_ms.
size_t SnapclientMediaSource::on_audio_write(const uint8_t *data, size_t length, uint32_t timeout_ms,
                                             const StreamParams &params) {
  if (!this->has_listener()) {
    return 0;
  }
  audio::AudioStreamInfo stream_info(params.bits_per_sample, params.channels, params.sample_rate);
  return this->write_output(data, length, timeout_ms, stream_info);
}

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_MEDIA_SOURCE
