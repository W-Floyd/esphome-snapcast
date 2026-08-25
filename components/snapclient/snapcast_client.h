#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "control_session.h"
#include "esphome/components/i2s_rate_lock/rate_lock.h"
#include "snapcast_proto.h"
#include "esphome/components/clock_sync/time_filter.h"
#include "esphome/components/clock_sync/tsf_sync.h"

#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/core/helpers.h"

#ifdef USE_SNAPCLIENT_FLAC
#include <micro_flac/flac_decoder.h>
#endif

#ifdef USE_SNAPCLIENT_OPUS
#include <opus.h>
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

// The timing engine lives in clock_sync and i2s_rate_lock: none of it is
// Snapcast-specific. Imported by name so the call sites below stay short.
using clock_sync::KalmanTimeFilter;
#if defined(USE_ESP32) && defined(USE_I2S_RATE_LOCK)
using i2s_rate_lock::RateLock;
#endif
#ifdef CLOCK_SYNC_TSF_ACTIVE
using clock_sync::TsfSync;
#endif

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
  /// @brief How long a chunk gap is bridged with keepalive silence before the stream is
  /// allowed to end. 0 means NEVER release while the session is up: the pipeline, the
  /// playout phase and the TSF mapping are held indefinitely so the speaker stays
  /// ready to play in sync. Costs a fed DAC, a radio held in high-performance mode and
  /// continuous TSF beaconing -- and note it does NOT make resumption free; see the
  /// stream-idle block in loop().
  uint32_t keepalive_hold_ms{0};
  // Median sync error at which the steering servo engages (disengages at half).
  // Reference esp32 snapclient uses 128 us; single-frame steering splices are ~23 us
  // events, inaudible.
  int64_t sync_deadband_us{128};
  // Coarse->fine handoff for muted convergence, and the boundary that decides
  // whether a hard resync forces a re-lock (see the player loop). Must stay
  // above 2*sync_deadband_us and below hard_resync_threshold_ms; the Python
  // schema enforces both.
  int64_t converge_fine_us{2000};
#ifdef USE_I2S_RATE_LOCK
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

/// @brief What a local PAUSE or STOP does on this client.
///
/// A fixed multiroom speaker and a desk speaker want opposite answers, which is why
/// this is configurable rather than chosen.
enum class PauseBehavior : uint8_t {
  // Honoured. The client stays silent until something plays it again; the media
  // source's re-arm only intervenes when nothing asked for the halt.
  ALLOW = 0,
  // Honoured, then undone once audio is flowing again. Survives a "stop everything"
  // automation without leaving the speaker silent for hours -- but note it means a
  // real audible gap: silence, then playback resumes a few seconds later.
  RESUME = 1,
  // Refused outright, so there is no gap at all. The media player keeps reporting
  // PLAYING, which is the honest state, and the transport button simply does nothing.
  // The trade-off is that a deliberate stop is also refused, so anything relying on
  // stopping this player (an automation, a voice command) will not work.
  IGNORE = 2,
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
  /// @brief Fired when this client's stream metadata changed (control session).
  virtual void on_stream_metadata(const StreamMetadata &metadata) {}
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

  /// @brief Bytes of audio already handed over but not yet played, if the sink can report it.
  /// Lets the playout accounting be anchored to the pipeline's MEASURED fill instead of an
  /// assumption about it; see the re-baseline paths in the player loop.
  /// @return false when unavailable -- distinct from a reported zero.
  virtual bool on_query_buffered(size_t & /*bytes*/) { return false; }
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

  /// @brief Drops the current session immediately (e.g. to free the radio for an
  /// OTA transfer); with network_ready false it stays down until re-enabled.
  void request_disconnect() { this->reconnect_requested_.store(true, std::memory_order_relaxed); }

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
#ifdef CLOCK_SYNC_TSF_ACTIVE
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
  /// @brief True while wire chunks are actually arriving, as opposed to the stream
  /// merely being nominally active.
  ///
  /// The distinction matters because keepalive_hold holds the stream open across a
  /// chunk gap, so "stream active" no longer means "audio is playing". Uses
  /// stream_idle_timeout as the threshold, which is exactly the question that option
  /// was always asking.
  bool audio_flowing() const;

 protected:
  // Median window for the steering servo's error signal. One sample per chunk, so its
  // duration follows the codec block size -- measured 26.2 ms/chunk (FLAC's 1152-frame
  // default at 44.1 kHz), giving ~0.39 s here. That duration is a term in the loop lag
  // that bounds the trim gain, so it is not a free parameter: a codec or rate with a
  // different block size changes it.
  //
  // Lengthened from 15 for stereo-image stability. The per-chunk error is WHITE NOISE
  // (measured: consecutive-difference sigma / sigma = 1.32-1.43 against sqrt(2) = 1.41),
  // so averaging more of it reduces the residual as ~sqrt(n) and costs nothing in
  // tracking -- there is no ramp being trailed. Note this is the opposite of the earlier
  // attempt to SHORTEN the window to buy loop bandwidth: with a noise-dominated error,
  // bandwidth pumps noise into the output and length removes it. Added lag is ~0.2 s
  // (half-window), keeping phase margin comfortable at KP = 0.5.
  static constexpr size_t MEDIAN_WINDOW = 31;

  /// @brief Everything the playout servo carries between chunks.
  ///
  /// These were locals of player_task_(), which is why the loop was ~700 lines: the
  /// declarations and their rationale sat 600 lines from the code that used them.
  /// Grouping them changes nothing at runtime -- the servo is still single-threaded in
  /// the player task, and this is still its stack -- but it lets the loop body be split
  /// into named steps that take the state explicitly.
  struct ServoState {
    bool warned_no_sync{false};
    // Rolling sync-error diagnostics, logged once per ~128 chunks (~3 s)
    int64_t err_accum_us{0};
    int64_t err_peak_us{0};
    uint32_t err_count{0};
    // Per-window stutter forensics: how often each correction mechanism fired
    uint32_t soft_dropped_frames{0};
    uint32_t soft_inserted_frames{0};
    uint32_t hard_resyncs{0};
    // Median of recent sync errors (rejects residual feedback spikes better than a
    // mean); the steering servo acts on this, not the raw per-chunk error. Same design
    // as the esp32 snapclient reference (99/19-sample medians on a sample-accurate age).
    int64_t err_window[MEDIAN_WINDOW]{};
    size_t err_window_idx{0};
    size_t err_window_filled{0};
    // Bang-bang steering with hysteresis, ported from the reference: while engaged,
    // trim exactly one frame per chunk (~950 ppm) until the median crosses back inside
    // the disengage threshold. Holds the error near zero continuously instead of
    // letting it random-walk inside a deadband -- a free-walking deadband is exactly
    // what wanders the stereo image between two paired devices.
    int8_t steer_dir{0};
#ifdef USE_I2S_RATE_LOCK
    // Rate lock: once converged, steady-state corrections become hardware clock trims
    // instead of frame splices. The PI integrator (positive = play faster) persists
    // across resyncs, flushes, and rate changes because it converges to the crystal
    // offset, a property of the hardware, not the stream.
    float trim_integral_ppm{0.0f};
    bool rate_lock_ok{false};
    uint32_t rate_lock_rate{0};
    // Trim wander over the report window. The trim IS the loop's estimate of the
    // disturbance it is cancelling, so its spread says whether the loop is tracking a
    // slow crystal offset (narrow, as designed) or chasing something it cannot (wide,
    // or pinned at the rail). Observed on all four devices: swings of hundreds of ppm
    // and repeated +-500 ppm saturation while medians stayed inside a few hundred us
    // -- i.e. running at its authority limit in normal operation. Quantify it before
    // any further gain change.
    float trim_min_ppm{0.0f};
    float trim_max_ppm{0.0f};
    uint32_t trim_samples{0};
    uint32_t trim_railed{0};
#endif
    uint32_t raw_sample_countdown{1};
    // Smoothed accounted-vs-observed disagreement (us); 0 when the accounting is honest
    float fill_corr_us{0.0f};
    bool fill_corr_valid{false};
    uint32_t fill_sample_countdown{0};
    // Mute-until-synced: real audio flows only after a full window of in-band medians
    bool converged{false};
    uint32_t in_band_chunks{0};
    int64_t last_resync_log_us{0};
    // When the stream first went staler than the server's buffer, 0 while it is not
    int64_t stale_since_us{0};
    // Rolling hard-resync count, for telling a one-shot catch-up from a storm
    int64_t storm_window_us{0};
    uint32_t storm_resyncs{0};
    // Learned accounted-vs-measured baseline, and when the excess over it began
    float drift_baseline_ms{0.0f};
    bool drift_baseline_valid{false};
    int64_t drift_excess_since_us{0};
    // Format of the last chunk played, for keepalive silence during a delivery gap
    StreamParams keepalive_params{};
  };

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
#ifdef CLOCK_SYNC_TSF_ACTIVE
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

#ifdef USE_SNAPCLIENT_OPUS
  /// Decodes one raw Opus packet -- a Snapcast wire chunk is exactly one, so unlike
  /// FLAC there is no carry-over buffer -- and emits the PCM stamped @p server_ts_us.
  void decode_opus_packet_(const uint8_t *data, size_t len, int64_t server_ts_us);
#endif

  // --- Player task ---
  void player_task_();
  /// Re-anchors the playout accounting after the pipeline fully drained, to the fill
  /// the sink REPORTS rather than an assumption that it is empty. Consumes the
  /// pipeline_starved_ latch, so it is called unconditionally once per chunk.
  void rebaseline_after_starvation_(ServoState &st, const ChunkRecord &rec, uint32_t frame_bytes);
  /// Emits the periodic sync report every 128 chunks and, on that same cadence,
  /// repairs a sustained split between the accounted queue and the measured fill.
  /// Resets the per-window counters.
  void log_sync_report_(ServoState &st, const ChunkRecord &rec, uint32_t frame_bytes, int64_t median_err_us);
  /// Requests a reconnect once the stream has been unrecoverably late for
  /// STALE_BAILOUT_US -- dropping chunks cannot close a gap the radio is causing.
  void check_stale_bailout_(ServoState &st, int64_t error_us, int64_t stale_us);
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
  // Mean feedback interval over the report window. This is the term that sets the
  // feedback-pivot EWMA time constant (ALPHA * interval) and therefore most of the
  // loop lag that bounds the trim gain -- and it had only ever been INFERRED, from
  // the max-gap line, when a gain increase was derived from it. Measure it.
  int64_t fb_gap_sum_us_{0};
  uint32_t fb_gap_count_{0};
  // Set by the feedback clamp when the pipeline fully drains (source starvation);
  // consumed by the player task, which re-baselines playout from scratch. The latch
  // (playout_mutex_) fires it once per drain, not per zero-clamped callback.
  std::atomic<bool> pipeline_starved_{false};
  bool starved_latched_{false};

  // --- Network task locals ---
  int sock_{-1};
  int64_t last_scan_us_{0};  // last mDNS scan, rate-limits re-scans on reconnect
  std::string active_host_;  // host of the current session (config or mDNS result)
  // FNV-1a of the active session's "host:port" — identifies which server clock a
  // shared TSF mapping refers to
  uint32_t server_id_hash_{0};
#ifdef CLOCK_SYNC_TSF_ACTIVE
  int64_t last_peer_refresh_us_{0};  // TSF unicast roster refresh (network task)
#endif
  uint16_t next_message_id_{0};
  bool stream_active_{false};
  // Written by the network task on every chunk, read from the main loop
  std::atomic<int64_t> last_chunk_us_{0};
  int64_t next_time_sync_us_{0};
  uint32_t time_sync_burst_remaining_{0};
  std::vector<uint8_t> rx_buffer_;
  StreamParams stream_params_{};

  enum class Codec : uint8_t { NONE, PCM, FLAC, OPUS };
  Codec codec_{Codec::NONE};
#ifdef USE_SNAPCLIENT_FLAC
  std::unique_ptr<micro_flac::FLACDecoder> flac_decoder_;
  std::vector<uint8_t> flac_input_;    // undecoded input carry-over across chunks
  std::vector<uint8_t> flac_output_;   // one decoded FLAC frame
  bool flac_header_done_{false};
#endif

#ifdef USE_SNAPCLIENT_OPUS
  struct OpusDecoderDeleter {
    void operator()(::OpusDecoder *decoder) const { opus_decoder_destroy(decoder); }
  };
  std::unique_ptr<::OpusDecoder, OpusDecoderDeleter> opus_decoder_;
  // One decoded packet, sized for Opus' 120 ms maximum. Allocated through
  // RAMAllocator rather than a vector so a short heap -- the realistic case on a
  // PSRAM-less board, where libopus' own state has just taken ~40 KB -- reports a
  // dead stream instead of aborting the device.
  struct HeapFree {
    void operator()(int16_t *buffer) const { free(buffer); }  // NOLINT(cppcoreguidelines-no-malloc)
  };
  std::unique_ptr<int16_t[], HeapFree> opus_output_;
  size_t opus_output_samples_{0};
#endif

#ifdef USE_I2S_RATE_LOCK
  // Hardware clock steering; owned here, driven by the player task's servo. The
  // speaker callback thread only pokes invalidate_baseline() (atomic flag).
  std::unique_ptr<RateLock> rate_lock_;
#endif

#ifdef CLOCK_SYNC_TSF_ACTIVE
  // TSF group sync: serviced by the network task, offset queried by the player task
  std::unique_ptr<TsfSync> tsf_sync_;
#endif

  // Persistent control-port session (metadata, roster, control RPCs); serviced by
  // the network task, metadata handed to the main loop
  std::unique_ptr<ControlSession> control_session_;

  // --- Player task locals ---
#ifdef CLOCK_SYNC_TSF_ACTIVE
  // Whether the last chunk deadline came from the shared TSF mapping (vs the
  // per-device Kalman fallback); gates unmute so joins land on the final timebase
  bool deadline_on_shared_tsf_{false};
#endif
  std::unique_ptr<uint8_t[]> slice_buffer_;
  static constexpr size_t SLICE_BUFFER_SIZE = 4096;
  // Most recent pushed frame, for click-free servo insertion (player task only)
  uint8_t last_frame_[8]{};
  uint32_t last_frame_bytes_{0};
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32
