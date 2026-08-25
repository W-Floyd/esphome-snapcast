#include "snapclient_media_source.h"

#if defined(USE_ESP32) && defined(USE_MEDIA_SOURCE)

#include "esphome/components/audio/audio.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>

namespace esphome::snapclient {

static const char *const TAG = "snapclient.media_source";

static constexpr char URI_PREFIX[] = "snapcast://";
static constexpr char URI_CURRENT[] = "snapcast://current";

// THREAD CONTEXT: Main loop. The callbacks registered here also fire on the main loop,
// since SnapclientHub dispatches events from client_->loop().
// How long the source may sit idle while the stream is live before playback is
// re-requested, and how long a request may stay pending before it is retried.
static constexpr uint32_t REARM_INTERVAL_MS = 5000;
static constexpr uint32_t REARM_PENDING_TIMEOUT_MS = 15000;

void SnapclientMediaSource::setup() {
  this->parent_->set_static_delay_ms(this->static_delay_ms_);

  // Re-arm playback when the stream is live but nothing is routing our audio.
  //
  // The request above fires only on the stream's not-active -> active EDGE. With
  // keepalive_hold: never that edge never recurs, because a chunk gap no longer ends
  // the stream -- so a source that lands in IDLE for any reason stays there until a
  // human presses play. Observed after a ~4 h idle: one device resumed on its own
  // while its partner sat silent needing a manual play, with no error logged anywhere.
  //
  // Deliberately not undone: a user STOP. That sets IDLE too, and re-requesting would
  // fight the person holding the remote. A PAUSE needs no special case -- it leaves
  // the state PAUSED, which the IDLE test below already excludes.
  this->set_interval("rearm_playback", REARM_INTERVAL_MS, [this]() {
    // A request that never landed would otherwise latch pending_start_ forever, which
    // is the same class of bug as the missing edge itself
    if (this->pending_start_ && millis() - this->start_requested_ms_ > REARM_PENDING_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Play request went unanswered; retrying");
      this->pending_start_ = false;
    }
    // "Audio flowing" rather than "stream live": keepalive_hold holds the stream open
    // across a chunk gap, so stream_live_ alone stays true through hours of silence.
    // Keying on real chunks is what makes RESUME sane -- a halt taken while the music
    // is stopped is honoured until the music actually comes back.
    if (!this->stream_live_ || !this->parent_->audio_flowing() || this->pending_start_ ||
        this->get_state() != media_source::MediaSourceState::IDLE) {
      return;
    }
    if (this->user_halted_ && this->parent_->pause_behavior() != PauseBehavior::RESUME) {
      return;  // someone asked for silence and the stream is not authoritative
    }
    ESP_LOGW(TAG, "%s; re-requesting playback",
             this->user_halted_ ? "Audio flowing again, overriding the local halt (pause_behavior: resume)"
                                : "Audio is flowing but nothing is routing it");
    this->pending_start_ = true;
    this->start_requested_ms_ = millis();
    this->request_play_uri_(URI_CURRENT);
  });

  this->parent_->add_server_settings_callback([this](uint8_t volume, bool muted, int32_t latency_ms) {
    // Track the server's current belief so notify_volume_changed / notify_mute_changed
    // don't echo the same values straight back as a ClientInfo message.
    this->last_server_volume_ = volume;
    this->last_server_muted_ = muted;
    // Coalesce application: multiple settings events can drain in one main loop, and
    // applying each one separately queues deferred requests that then execute against
    // stale bookkeeping (a superseded mute/volume misread as a genuine local change).
    // Only ever apply the latest state, once.
    this->set_timeout("apply_server_settings", 0, [this]() { this->apply_server_state_(); });
  });


  this->parent_->add_stream_state_callback([this](bool started, const StreamParams &params) {
    if (started) {
      ESP_LOGD(TAG, "Stream started: %" PRIu32 " Hz, %u bit, %u ch", params.sample_rate, params.bits_per_sample,
               params.channels);
      this->stream_live_ = true;
      if (!this->pending_start_ && this->get_state() == media_source::MediaSourceState::IDLE) {
        // Ask the orchestrator to route audio from us
        this->pending_start_ = true;
        this->start_requested_ms_ = millis();
        this->request_play_uri_(URI_CURRENT);
      }
    } else {
      ESP_LOGD(TAG, "Stream ended");
      this->stream_live_ = false;
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
  this->user_halted_ = false;
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
  // pause_behavior: ignore refuses the transport outright. Refusing beats accepting and
  // then undoing it: RESUME leaves a real audible gap (silence, then playback returns a
  // few seconds later) whereas this leaves the audio untouched. The state is not
  // changed, so the media player keeps reporting PLAYING -- which is the truth, and the
  // transport button simply does nothing.
  if (this->parent_->pause_behavior() == PauseBehavior::IGNORE &&
      (command == media_source::MediaSourceCommand::PAUSE || command == media_source::MediaSourceCommand::STOP)) {
    ESP_LOGI(TAG, "Ignoring %s (pause_behavior: ignore)",
             command == media_source::MediaSourceCommand::PAUSE ? "PAUSE" : "STOP");
    return;
  }
  // The Snapcast binary protocol carries no transport control (that lives in the
  // server's JSON-RPC API), so pause/stop act locally. Chunks are still discarded at
  // their deadline while paused, so resuming snaps straight back into sync.
  switch (command) {
    case media_source::MediaSourceCommand::PLAY:
      this->user_halted_ = false;
      this->set_playback_state_(media_source::MediaSourceState::PLAYING);
      break;
    case media_source::MediaSourceCommand::PAUSE:
      // Deliberate: survives a later drop to IDLE, so the re-arm cannot un-pause it
      this->user_halted_ = true;
      this->set_playback_state_(media_source::MediaSourceState::PAUSED);
      break;
    case media_source::MediaSourceCommand::STOP:
      // The user's decision; the re-arm must leave it alone until they play again
      this->user_halted_ = true;
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

// THREAD CONTEXT: Main loop (coalescing timeout). Applies the latest server-pushed
// state — mute immediately, volume subject to the local-wins-recently grace window:
// while the user is actively dragging the local slider, server pushes (group-volume
// recalculations, in-flight echoes of our own reports) would yank it back mid-drag.
// Deferred application retries until the drag has been quiet for the window; the
// latest value still wins, so a genuine concurrent server change is delayed, not lost.
void SnapclientMediaSource::apply_server_state_() {
  if (this->last_server_volume_ < 0) {
    return;
  }
  this->local_muted_ = this->last_server_muted_ == 1;

  const uint32_t since_local = millis() - this->last_local_volume_ms_;
  if (this->last_local_volume_ms_ != 0 && since_local < LOCAL_VOLUME_GRACE_MS) {
    // Volume deferred; mute can apply alone (no volume request, so the orchestrator's
    // unmute-on-volume-set below is not triggered)
    this->request_mute_(this->local_muted_);
    this->set_timeout("apply_server_settings", LOCAL_VOLUME_GRACE_MS - since_local,
                      [this]() { this->apply_server_state_(); });
    return;
  }

  // Order is load-bearing: the orchestrator's set_volume_ force-unmutes on any
  // non-zero volume, so the mute request must be queued after the volume request
  // to end up as the final state.
  this->local_volume_ = static_cast<uint8_t>(this->last_server_volume_);
  this->apply_server_volume_(this->local_volume_);
  this->request_mute_(this->local_muted_);
}

// THREAD CONTEXT: Main loop (server settings)
void SnapclientMediaSource::apply_server_volume_(uint8_t volume_percent) {
  // Straight through. No taper is applied here: every ESPHome output path already maps
  // the slider linearly in DECIBELS -- i2s_audio spans SOFTWARE_VOLUME_MIN_DB (-49 dB)
  // to 0, and audio_dac drivers write dB-scaled registers -- so a second curve on top
  // would double-apply the log law and make the slider bottom-heavy.
  const float gain = volume_percent / 100.0f;
  this->last_requested_gain_ = gain;
  this->request_volume_(gain);
}

// THREAD CONTEXT: Main loop (orchestrator -> source notification)
void SnapclientMediaSource::notify_volume_changed(float volume) {
  if (std::abs(volume - this->last_requested_gain_) < 0.005f) {
    // Our own request echoing back through the orchestrator, not a user change
    return;
  }
  // A local (HA slider) change: report the equivalent slider position to the server
  this->last_local_volume_ms_ = millis();
  const int volume_percent = static_cast<int>(std::roundf(volume * 100.0f));
  this->local_volume_ = static_cast<uint8_t>(std::clamp(volume_percent, 0, 100));
  if (volume_percent != this->last_server_volume_) {
    this->parent_->send_client_volume(this->local_volume_, this->local_muted_);
    // The server does not echo ServerSettings back for ClientInfo updates, so record
    // its new belief here — otherwise the next change back would look like an echo.
    this->last_server_volume_ = this->local_volume_;
  }
}

// THREAD CONTEXT: Main loop (orchestrator -> source notification)
void SnapclientMediaSource::notify_mute_changed(bool is_muted) {
  this->local_muted_ = is_muted;
  const bool report = static_cast<int>(is_muted) != this->last_server_muted_;
  ESP_LOGD(TAG, "notify_mute_changed: muted=%d last_server=%d -> %s", is_muted, this->last_server_muted_,
           report ? "reporting" : "echo, skipping");
  if (report) {
    this->parent_->send_client_volume(this->local_volume_, this->local_muted_);
    // The server does not echo ServerSettings back for ClientInfo updates, so record
    // its new belief here — otherwise unmuting after muting would look like an echo.
    this->last_server_muted_ = static_cast<int>(is_muted);
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

bool SnapclientMediaSource::on_query_latency(uint32_t &microseconds) {
  return this->output_render_latency(microseconds);
}

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_MEDIA_SOURCE
