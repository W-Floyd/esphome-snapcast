#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "rate_lock.h"
#include "snapcast_proto.h"
#include "time_filter.h"
#include "tsf_sync.h"

#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/core/helpers.h"

#ifdef USE_SNAPCLIENT_FLAC
#include <micro_flac/flac_decoder.h>
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esphome::snapclient {

/// @brief Compile-time configuration for SnapcastClient, built by the hub's codegen setters.
struct SnapcastClientConfig {
  std::string server_host;  // empty: discover the server via mDNS (_snapcast._tcp)
  uint16_t server_port{1704};
  std::string hostname;   // Hello HostName (display name basis on the server)
  std::string client_id;  // Hello MAC/ID; the pretty MAC address
  size_t buffer_size{524288};
  uint32_t time_sync_interval_ms{1000};
  uint32_t hard_resync_threshold_ms{50};
  uint32_t stream_idle_timeout_ms{3000};
  // Median sync error at which the steering servo engages (disengages at half).
  // Reference esp32 snapclient uses 128 us; single-frame steering splices are ~23 us
  // events, inaudible.
  int64_t sync_deadband_us{128};
#ifdef USE_SNAPCLIENT_RATE_LOCK
  // I2S port whose output clock the hardware rate lock steers (steady-state
  // corrections become clock trims instead of frame splices where supported)
  uint8_t rate_lock_i2s_port{0};
#endif
};

/// @brief Output channel routing, matching esp32 snapclient's dsp_channel_mode_t.
enum class ChannelMode : uint8_t {
  STEREO = 0,      // Normal stereo
  LEFT_ONLY = 1,   // Route the left channel to both outputs
  RIGHT_ONLY = 2,  // Route the right channel to both outputs
  MONO = 3,        // Mix L+R to mono, route to both outputs
};

/// @brief Output polarity inversion, for correcting out-of-phase drivers in software
/// (one inverted speaker in a synchronized pair cancels bass with its partner).
enum class PhaseMode : uint8_t {
  NONE = 0,   // Normal polarity
  LEFT = 1,   // Invert the left channel (any non-NONE inverts a mono stream)
  RIGHT = 2,  // Invert the right channel
  BOTH = 3,   // Invert both channels
};

/// @brief Decoded stream format of the current Snapcast stream.
struct StreamParams {
  uint32_t sample_rate{0};
  uint8_t bits_per_sample{0};
  uint8_t channels{0};

  uint32_t frame_bytes() const { return static_cast<uint32_t>(this->channels) * (this->bits_per_sample / 8); }
  bool valid() const { return this->sample_rate != 0 && this->bits_per_sample != 0 && this->channels != 0; }
};

/// @brief One snapserver found by an mDNS scan (_snapcast._tcp).
struct ServerCandidate {
  std::string name;  // mDNS instance name (hostname or IP when unnamed)
  std::string host;  // IPv4 address string
  uint16_t port;

  bool operator==(const ServerCandidate &o) const {
    return this->port == o.port && this->host == o.host && this->name == o.name;
  }
};

/// @brief TSF group-sync role, for diagnostics entities. INACTIVE covers: feature
/// off/unsupported (no wifi), no session, or no election result yet.
enum class TsfRole : uint8_t { INACTIVE, FOLLOWER, LEADER };

/// @brief Events the client pushes to its listener.
///
/// All callbacks fire on the main loop thread, dispatched from SnapcastClient::loop();
/// the background tasks only enqueue.
class SnapcastClientListener {
 public:
  virtual void on_connection_changed(bool connected) = 0;
  virtual void on_server_settings(const ServerSettings &settings) = 0;
  virtual void on_stream_start(const StreamParams &params) = 0;
  virtual void on_stream_end() = 0;
  /// @brief Fired when an mDNS scan changed the discovered-server list.
  virtual void on_servers_discovered(const std::vector<ServerCandidate> &servers) {}
};

/// @brief Sink for synchronized PCM audio.
///
/// on_audio_write is called from the client's player task and may block up to
/// timeout_ms (downstream backpressure). Return the number of bytes accepted;
/// returning 0 while inactive lets the client discard in-sync instead of stalling.
class SnapcastAudioListener {
 public:
  virtual size_t on_audio_write(const uint8_t *data, size_t length, uint32_t timeout_ms,
                                const StreamParams &params) = 0;
};

/// @brief Native Snapcast client core.
///
/// Structured like sendspin-cpp's SendspinClient: a plain class owned by the ESPHome
/// hub component, with listener interfaces for library -> user events and exposed
/// methods for user -> library calls. Runs two FreeRTOS tasks:
///
///  - Network task: TCP connection, Hello handshake, message framing, time sync
///    (Kalman-filtered clock offset), decode (PCM/FLAC) into a timestamped PCM buffer.
///  - Player task: pops timestamped chunks, computes the local playout deadline from
///    the clock offset and the server's buffer/latency settings, corrects against the
///    DAC feedback from notify_audio_played(), and pushes PCM to the audio listener.
class SnapcastClient {
 public:
  explicit SnapcastClient(SnapcastClientConfig config) : config_(std::move(config)) {}
  ~SnapcastClient();

  void set_listener(SnapcastClientListener *listener) { this->listener_ = listener; }
  void set_audio_listener(SnapcastAudioListener *audio_listener) { this->audio_listener_ = audio_listener; }

  /// @brief Allocates buffers and starts the background tasks.
  /// @return false if an allocation or task creation failed.
  bool start();

  /// @brief Dispatches queued task events to the listener. Call from the main loop.
  void loop();

  // --- Main-loop-thread inputs ---

  /// @brief Mirrors ESPHome's network readiness into the tasks (network::is_connected
  /// is not safe to call off the main loop).
  void set_network_ready(bool ready) { this->network_ready_.store(ready, std::memory_order_relaxed); }

  /// @brief Enables/disables audio output. While disabled, the player task discards
  /// chunks at their deadline so playback resumes in sync when re-enabled.
  void set_output_active(bool active);

  /// @brief Per-device latency trim, subtracted from every chunk deadline.
  void set_static_delay_ms(int32_t delay_ms) { this->static_delay_ms_.store(delay_ms, std::memory_order_relaxed); }

  /// @brief Overrides the connection target, taking precedence over the configured
  /// server and mDNS discovery. Empty @p host clears the override; @p port 0 means
  /// the configured default port. A live session to a different target is dropped
  /// and the network task reconnects to the new one.
  void set_server_override(const std::string &host, uint16_t port);

  /// @brief Keeps the discovered-server list fresh by re-scanning mDNS on reconnects
  /// even when a target is already known (for the server select entity).
  void set_discovery_enabled(bool enabled) { this->discovery_enabled_.store(enabled, std::memory_order_relaxed); }

  /// @brief Output channel routing; applied in-place to stereo 16-bit audio as it is
  /// pushed, so it may be changed at any time without disturbing sync accounting.
  void set_channel_mode(ChannelMode mode) {
    this->channel_mode_.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
  }

  /// @brief Output polarity inversion; same in-place push-path transform as
  /// set_channel_mode, safe to change at any time.
  void set_phase_mode(PhaseMode mode) {
    this->phase_mode_.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
  }

  /// @brief Reports a local volume/mute change to the server via a ClientInfo message.
  void send_client_info(uint8_t volume_percent, bool muted);

  /// @brief Sets this client's server-side latency via the control API (JSON-RPC,
  /// port 1705). The server persists it and pushes the updated ServerSettings back.
  void set_server_latency(int32_t latency_ms);

  // --- Playback feedback ---

  /// @brief Feed DAC-write feedback from the speaker's audio output callback.
  /// THREAD CONTEXT: speaker task; internally synchronized.
  void notify_audio_played(uint32_t frames, int64_t timestamp_us);

  // --- Diagnostics (main loop) ---

  bool is_connected() const { return this->connected_.load(std::memory_order_relaxed); }
  /// @brief Current TSF group-sync role (atomic read; INACTIVE when unavailable).
  TsfRole get_tsf_role() const {
#ifdef SNAPCLIENT_TSF_ACTIVE
    if (this->tsf_sync_ != nullptr) {
      switch (this->tsf_sync_->role()) {
        case TsfSync::Role::LEADER:
          return TsfRole::LEADER;
        case TsfSync::Role::FOLLOWER:
          return TsfRole::FOLLOWER;
        default:
          break;
      }
    }
#endif
    return TsfRole::INACTIVE;
  }
  /// @brief Current server-minus-client clock offset estimate in ms.
  float get_clock_offset_ms();
  const ServerSettings &get_server_settings() const { return this->settings_main_; }

 protected:
  // Fixed-size record describing one decoded chunk resident in the PCM ring buffer.
  // Records are posted to the player task strictly after their PCM bytes are written,
  // so a popped record's bytes are always fully readable.
  struct ChunkRecord {
    int64_t server_ts_us;
    uint32_t bytes;
    StreamParams params;
  };

  enum class EventType : uint8_t { CONNECTED, DISCONNECTED, SERVER_SETTINGS, STREAM_START, STREAM_END };
  struct Event {
    EventType type;
    ServerSettings settings;
    StreamParams params;
  };

  static void network_task_trampoline(void *arg) { static_cast<SnapcastClient *>(arg)->network_task_(); }
  static void player_task_trampoline(void *arg) { static_cast<SnapcastClient *>(arg)->player_task_(); }

  // --- Network task ---
  void network_task_();
  /// One connection lifetime: connect, hello, pump until error/shutdown.
  void connection_session_();
  /// Scans for snapservers via an mDNS PTR query for _snapcast._tcp, storing every
  /// usable result in discovered_servers_ (dirty-flagged for the listener).
  /// @return true if at least one server was found.
  bool scan_servers_();
  bool connect_socket_(const std::string &host, uint16_t port);
  bool send_message_(MessageType type, const uint8_t *payload, size_t len, uint16_t refers_to = 0);
  /// Reads exactly @p len bytes; false on error/close/shutdown. Services periodic
  /// TX (time sync, ClientInfo, idle detection) while waiting for data.
  bool recv_exact_(uint8_t *buf, size_t len);
  /// Sends due time-sync requests / pending ClientInfo and runs the stream idle check.
  void service_tx_();
  /// Sends one Client.SetLatency request on the server's control port (1705).
  void send_set_latency_rpc_(int32_t latency_ms);
#ifdef SNAPCLIENT_TSF_ACTIVE
  /// Fetches the server's client roster (Server.GetStatus, control port) for TSF
  /// unicast beacons. Blocking; only called while no stream is active.
  void refresh_tsf_peers_();
#endif
  void handle_codec_header_(const uint8_t *payload, size_t len);
  void handle_wire_chunk_(const uint8_t *payload, size_t len);
  void handle_time_reply_(const BaseMessage &base, const uint8_t *payload, size_t len, int64_t recv_us);
  /// Writes decoded PCM + its record to the player, blocking on backpressure.
  void emit_pcm_(const uint8_t *data, size_t len, int64_t server_ts_us);
  void post_event_(const Event &event);
  void set_stream_active_(bool active);
  void close_socket_();

#ifdef USE_SNAPCLIENT_FLAC
  /// Runs buffered FLAC input through the decoder, emitting PCM stamped with @p server_ts_us
  /// (or announcing the stream on header completion when @p server_ts_us < 0).
  void decode_flac_input_(int64_t server_ts_us);
#endif

  // --- Player task ---
  void player_task_();
  /// @return the predicted DAC time (µs) of the next frame pushed downstream, or -1 if
  /// no playback feedback has arrived yet.
  int64_t predict_next_play_us_(uint32_t sample_rate);
  /// Computes the local deadline for a chunk record.
  int64_t chunk_deadline_us_(const ChunkRecord &rec);
  /// Reads @p bytes from the PCM ring and discards them.
  void discard_ring_bytes_(size_t bytes);
  /// Pushes silence downstream. @return frames actually pushed.
  uint32_t push_silence_(uint32_t frames, const StreamParams &params);
  /// Pushes one copy of the most recently pushed frame (sample stuffing, like the
  /// reference); a repeated frame is nearly click-free where an inserted silence
  /// frame is a hard amplitude step. Falls back to silence when no frame is cached.
  void push_repeat_frame_(const StreamParams &params);
  /// Pushes @p bytes from the ring downstream, dropping @p drop_frames from the front.
  /// @p silent replaces the audio with zeros (timing preserved): played during sync
  /// convergence so correction splices are inaudible, like the reference's mute-
  /// until-synced.
  void push_chunk_(const ChunkRecord &rec, uint32_t drop_frames, bool silent);
  /// @brief Applies the configured channel routing and polarity inversion in-place
  /// to a frame-aligned slice.
  void apply_channel_mode_(uint8_t *data, size_t len, const StreamParams &params);

  SnapcastClientConfig config_;
  SnapcastClientListener *listener_{nullptr};
  SnapcastAudioListener *audio_listener_{nullptr};

  TaskHandle_t network_task_handle_{nullptr};
  TaskHandle_t player_task_handle_{nullptr};
  std::atomic<bool> shutdown_{false};

  QueueHandle_t event_queue_{nullptr};
  QueueHandle_t record_queue_{nullptr};
  std::unique_ptr<ring_buffer::RingBuffer> pcm_ring_;

  // --- Shared state ---
  std::atomic<bool> network_ready_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> output_active_{false};
  std::atomic<int32_t> static_delay_ms_{0};
  std::atomic<uint8_t> channel_mode_{static_cast<uint8_t>(ChannelMode::STEREO)};
  std::atomic<uint8_t> phase_mode_{static_cast<uint8_t>(PhaseMode::NONE)};

  // Server settings shadow used by the tasks (buffer_ms/latency for deadlines).
  std::atomic<int32_t> buffer_ms_{1000};
  std::atomic<int32_t> server_latency_ms_{0};
  // Main-loop copy for diagnostics.
  ServerSettings settings_main_{};

  // Pending ClientInfo, written by the main loop and consumed by the network task.
  Mutex client_info_mutex_;
  bool client_info_dirty_{false};
  uint8_t client_info_volume_{100};
  bool client_info_muted_{false};
  // Pending Client.SetLatency RPC (same producer/consumer pattern)
  bool latency_dirty_{false};
  int32_t latency_pending_ms_{0};

  // Server discovery + override. Strings can't ride the byte-copying FreeRTOS event
  // queue, so the candidate list is handed to the main loop under this mutex.
  Mutex server_mutex_;
  std::vector<ServerCandidate> discovered_servers_;
  bool discovered_dirty_{false};
  std::string override_host_;  // empty: no override
  uint16_t override_port_{0};  // 0: configured default port
  std::atomic<bool> discovery_enabled_{false};
  // Asks the network task to drop the session (target changed); checked in recv waits
  std::atomic<bool> reconnect_requested_{false};

  // Clock offset filter: fed by the network task, read by the player task + main loop.
  Mutex filter_mutex_;
  KalmanTimeFilter time_filter_;
  // Decaying minimum observed time-sync RTT (network task only); congestion gate.
  int64_t min_rtt_us_{INT64_MAX / 2};

  // Playout feedback: written from the speaker callback thread, read by the player task.
  // In addition to the raw last-callback state, an exponentially-weighted linear
  // regression estimates the DAC clock (frame index -> system time): the speaker
  // reports frames in DMA-sized bursts (~10 ms quantization), and predicting from the
  // raw last callback carries that quantization as sync noise. The fitted line is
  // smooth to microsecond scale, which is what allows reference-grade (~100 us)
  // steering thresholds. Ported concept from esp32 snapclient's sample-accurate age.
  Mutex playout_mutex_;
  bool playout_valid_{false};
  int64_t played_frames_total_{0};
  int64_t played_last_ts_us_{0};
  int64_t pushed_frames_total_{0};
  double fb_mean_frames_{0.0};
  double fb_mean_ts_{0.0};
  uint32_t fb_samples_{0};
  // Longest interval between playback feedback callbacks in the current diagnostics
  // window; read-and-reset by the player task's periodic sync report
  int64_t max_feedback_gap_us_{0};
  // Set by the feedback clamp when the pipeline fully drains (source starvation);
  // consumed by the player task, which re-baselines playout from scratch
  std::atomic<bool> pipeline_starved_{false};

  // --- Network task locals ---
  int sock_{-1};
  int64_t last_scan_us_{0};  // last mDNS scan, rate-limits re-scans on reconnect
  std::string active_host_;  // host of the current session (config or mDNS result)
  // FNV-1a of the active session's "host:port" — identifies which server clock a
  // shared TSF mapping refers to
  uint32_t server_id_hash_{0};
#ifdef SNAPCLIENT_TSF_ACTIVE
  int64_t last_peer_refresh_us_{0};  // TSF unicast roster refresh (network task)
#endif
  uint16_t next_message_id_{0};
  bool stream_active_{false};
  int64_t last_chunk_us_{0};
  int64_t next_time_sync_us_{0};
  uint32_t time_sync_burst_remaining_{0};
  std::vector<uint8_t> rx_buffer_;
  StreamParams stream_params_{};

  enum class Codec : uint8_t { NONE, PCM, FLAC };
  Codec codec_{Codec::NONE};
#ifdef USE_SNAPCLIENT_FLAC
  std::unique_ptr<micro_flac::FLACDecoder> flac_decoder_;
  std::vector<uint8_t> flac_input_;    // undecoded input carry-over across chunks
  std::vector<uint8_t> flac_output_;   // one decoded FLAC frame
  bool flac_header_done_{false};
#endif

#ifdef USE_SNAPCLIENT_RATE_LOCK
  // Hardware clock steering; owned here, driven by the player task's servo. The
  // speaker callback thread only pokes invalidate_baseline() (atomic flag).
  std::unique_ptr<RateLock> rate_lock_;
#endif

#ifdef SNAPCLIENT_TSF_ACTIVE
  // TSF group sync: serviced by the network task, offset queried by the player task
  std::unique_ptr<TsfSync> tsf_sync_;
#endif

  // --- Player task locals ---
  std::unique_ptr<uint8_t[]> slice_buffer_;
  static constexpr size_t SLICE_BUFFER_SIZE = 4096;
  // Most recent pushed frame, for click-free servo insertion (player task only)
  uint8_t last_frame_[8]{};
  uint32_t last_frame_bytes_{0};
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32
