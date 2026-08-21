#include "snapcast_client.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#ifdef SNAPCLIENT_TSF_ACTIVE
#include "esphome/components/json/json_util.h"
#endif

#include <esp_timer.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#ifdef USE_MDNS
#include <mdns.h>
#endif

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome::snapclient {

static const char *const TAG = "snapclient.client";

// Sanity cap on a single message payload; the largest legitimate payloads are FLAC
// wire chunks (a few KB) and codec headers.
static constexpr uint32_t MAX_PAYLOAD_SIZE = 262144;

// Time-sync cadence: a burst on (re)connect for fast filter convergence, then steady
// state. Mirrors the reference web client / embedded snapclient startup behavior.
static constexpr uint32_t TIME_SYNC_BURST_COUNT = 10;
static constexpr uint32_t TIME_SYNC_BURST_INTERVAL_MS = 100;
// While no stream is active, sync only often enough to keep the clock estimate warm
static constexpr uint32_t TIME_SYNC_IDLE_INTERVAL_MS = 2000;

static constexpr uint32_t RECONNECT_DELAY_MS = 2000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;

// Minimum interval between mDNS server scans; each blocks the network task ~3 s, so
// the list refreshes on reconnects rather than on a timer of its own
static constexpr int64_t SERVER_SCAN_MIN_INTERVAL_US = 60000000;

// Deadline slack before the first playback feedback arrives: the first chunk is
// released this far ahead of its deadline so the pipeline has time to spin up, and
// the first feedback-based correction absorbs the remainder.
static constexpr int64_t STARTUP_LEAD_US = 150000;

// Soft-sync correction limit: at most 1/128 of a chunk's frames (~0.2 ms per 26 ms
// chunk) are inserted/dropped per chunk, inaudible but converging ~8 ms per second.
static constexpr uint32_t SOFT_CORRECTION_DIVISOR = 128;

// Above this median error, correct proportionally (post-stall catch-up); below it,
// the steering servo trims single frames. Must sit well above the measurement noise
// and its lag dynamics: a lower threshold put proportional gain inside the loop's
// oscillation amplitude and produced audible warble (~1100 splices/s, observed).
static constexpr int64_t SOFT_CORRECTION_AGGRESSIVE_US = 10000;

// Steering size while muted pre-convergence (~7 ms/s); audibility is not a
// constraint through silence, so lock happens in ~1-2 s instead of up to ~10 s
static constexpr uint32_t STARTUP_STEER_FRAMES = 8;

// Median window for the steering servo's error signal (~0.4 s of chunks)
static constexpr size_t MEDIAN_WINDOW = 15;

#ifdef USE_SNAPCLIENT_RATE_LOCK
// Rate-lock PI gains. The plant is an integrator -- queue depth integrates any
// rate mismatch, so the error's *slope* is the trim -- which is why a stepping
// bang-bang trim (a second integrator) limit-cycled structurally on hardware
// (observed: +-250 ppm / +-3 ms swings, matching the double-integrator prediction
// sqrt(2*e0*slew)). Error in us, trim in ppm (1 ppm = 1 us/s of error slope).
//
// Bandwidth is set by disturbance tracking, not settling: the clock-offset estimate
// (and feedback pivot) wander ~100 us/s with wifi jitter, and the loop trails a
// ramp by rate/KP (KP = 0.1 measured +-1-2 ms excursions on hardware -- pure
// tracking lag, not instability). KP = 0.5 bounds that to ~200 us, with ~50 deg
// phase margin against the ~0.85 s measurement lag (feedback-pivot EWMA + median
// window). KI = KP^2/4: critically damped; the integrator absorbs the crystal
// offset so P holds the error at zero.
static constexpr float TRIM_KP_PPM_PER_US = 0.5f;
static constexpr float TRIM_KI_PPM_PER_US_S = 0.0625f;
static constexpr float TRIM_CLAMP_PPM = 500.0f;
#endif

// A playback-feedback gap this long means the pipeline stopped (and flushed its
// buffers on restart); triggers the frame-accounting re-baseline in
// notify_audio_played()
static constexpr int64_t PIPELINE_FLUSH_GAP_US = 500000;

// At most one hard-resync log line this often; the sync report carries full counts
static constexpr int64_t RESYNC_LOG_INTERVAL_US = 2000000;

#ifdef SNAPCLIENT_TSF_ACTIVE
// TSF unicast roster refresh cadence (only while no stream is active; blocking RPC)
static constexpr int64_t TSF_PEER_REFRESH_US = 60000000;
#endif

// Time-sync RTT gating (see handle_time_reply_)
static constexpr int64_t RTT_GATE_US = 20000;      // reject samples this far above the floor
static constexpr int64_t RTT_FLOOR_LEAK_US = 500;  // floor rises this much per sample (~0.5 ms/s)

static inline int64_t now_us() { return esp_timer_get_time(); }

SnapcastClient::~SnapcastClient() {
  // Components are never destructed in practice; best-effort teardown.
  this->shutdown_.store(true, std::memory_order_relaxed);
  this->close_socket_();
  if (this->network_task_handle_ != nullptr) {
    vTaskDelete(this->network_task_handle_);
  }
  if (this->player_task_handle_ != nullptr) {
    vTaskDelete(this->player_task_handle_);
  }
}

bool SnapcastClient::start() {
  this->pcm_ring_ = ring_buffer::RingBuffer::create(this->config_.buffer_size);
  if (this->pcm_ring_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %zu byte PCM buffer", this->config_.buffer_size);
    return false;
  }
  this->slice_buffer_ = std::make_unique<uint8_t[]>(SLICE_BUFFER_SIZE);

#ifdef USE_SNAPCLIENT_RATE_LOCK
  this->rate_lock_ = std::make_unique<RateLock>(this->config_.rate_lock_i2s_port);
#endif

#ifdef SNAPCLIENT_TSF_ACTIVE
  // Plausibility gate mirrors the hard-resync threshold: a shared mapping that far
  // from our own estimate would hard-resync us -- reject it instead
  this->tsf_sync_ =
      std::make_unique<TsfSync>(static_cast<int64_t>(this->config_.hard_resync_threshold_ms) * 1000);
#endif

  this->control_session_ = std::make_unique<ControlSession>(this->config_.client_id);

  this->event_queue_ = xQueueCreate(8, sizeof(Event));
  this->record_queue_ = xQueueCreate(160, sizeof(ChunkRecord));
  if (this->event_queue_ == nullptr || this->record_queue_ == nullptr) {
    return false;
  }

  // The player outranks the network task so decode bursts cannot starve playout.
  if (xTaskCreate(SnapcastClient::player_task_trampoline, "snap_player", 6144, this, 8, &this->player_task_handle_) !=
      pdPASS) {
    return false;
  }
  if (xTaskCreate(SnapcastClient::network_task_trampoline, "snap_net", 8192, this, 5, &this->network_task_handle_) !=
      pdPASS) {
    return false;
  }
  return true;
}

// THREAD CONTEXT: Main loop (called from the hub's loop())
void SnapcastClient::loop() {
  Event event;
  while (xQueueReceive(this->event_queue_, &event, 0) == pdTRUE) {
    if (this->listener_ == nullptr) {
      continue;
    }
    switch (event.type) {
      case EventType::CONNECTED:
        this->listener_->on_connection_changed(true);
        break;
      case EventType::DISCONNECTED:
        this->listener_->on_connection_changed(false);
        break;
      case EventType::SERVER_SETTINGS:
        this->settings_main_ = event.settings;
        this->listener_->on_server_settings(event.settings);
        break;
      case EventType::STREAM_START:
        this->listener_->on_stream_start(event.params);
        break;
      case EventType::STREAM_END:
        this->listener_->on_stream_end();
        break;
    }
  }

  // Discovered-server list: built on the network task, handed over under the mutex
  // (its strings can't ride the byte-copying event queue)
  this->server_mutex_.lock();
  const bool dirty = this->discovered_dirty_;
  std::vector<ServerCandidate> servers;
  if (dirty) {
    servers = this->discovered_servers_;
    this->discovered_dirty_ = false;
  }
  this->server_mutex_.unlock();
  if (dirty && this->listener_ != nullptr) {
    this->listener_->on_servers_discovered(servers);
  }

  // Stream metadata from the control session (network task -> main loop handoff)
  if (this->control_session_ != nullptr && this->listener_ != nullptr) {
    StreamMetadata metadata;
    if (this->control_session_->take_metadata(metadata)) {
      this->listener_->on_stream_metadata(metadata);
    }
  }
}

// THREAD CONTEXT: Main loop
void SnapcastClient::set_output_active(bool active) {
  if (active && !this->output_active_.load(std::memory_order_relaxed)) {
    // The downstream pipeline restarts when the source is (re)activated, so frames
    // pushed in an earlier session no longer relate to the playback feedback.
    this->playout_mutex_.lock();
    this->playout_valid_ = false;
    this->played_frames_total_ = 0;
    this->pushed_frames_total_ = 0;
    this->fb_samples_ = 0;
    this->playout_mutex_.unlock();
  }
  this->output_active_.store(active, std::memory_order_relaxed);
}

// THREAD CONTEXT: Main loop
void SnapcastClient::set_server_latency(int32_t latency_ms) {
  this->client_info_mutex_.lock();
  this->latency_dirty_ = true;
  this->latency_pending_ms_ = latency_ms;
  this->client_info_mutex_.unlock();
}

// THREAD CONTEXT: Main loop
void SnapcastClient::send_client_info(uint8_t volume_percent, bool muted) {
  this->client_info_mutex_.lock();
  this->client_info_dirty_ = true;
  this->client_info_volume_ = volume_percent;
  this->client_info_muted_ = muted;
  this->client_info_mutex_.unlock();
}

// THREAD CONTEXT: Main loop
void SnapcastClient::set_server_override(const std::string &host, uint16_t port) {
  this->server_mutex_.lock();
  const bool changed = host != this->override_host_ || port != this->override_port_;
  this->override_host_ = host;
  this->override_port_ = port;
  this->server_mutex_.unlock();
  if (changed) {
    // Drop the current session; the network task reconnects to the new target
    // (or back to configured/discovered when the override was cleared)
    this->reconnect_requested_.store(true, std::memory_order_relaxed);
  }
}

// THREAD CONTEXT: Speaker playback callback thread
void SnapcastClient::notify_audio_played(uint32_t frames, int64_t timestamp_us) {
  this->playout_mutex_.lock();
  if (this->playout_valid_) {
    // A gap well beyond the speaker's DMA cadence means the DAC was starved
    // (pipeline underrun); surfaced in the periodic sync report for diagnostics
    const int64_t gap = timestamp_us - this->played_last_ts_us_;
    this->max_feedback_gap_us_ = std::max(this->max_feedback_gap_us_, gap);
    if (gap > PIPELINE_FLUSH_GAP_US) {
      // The pipeline stopped and restarted; the orchestrator recreates its ring on
      // restart, silently DISCARDING pushed-but-unplayed frames. Without this
      // re-baseline, those frames stay counted as queued and the prediction is
      // permanently late by the discarded amount — every chunk hard-drops, which
      // starves the pipeline into another flush: an unrecoverable death spiral.
      // At resume the pipeline is empty, so played == pushed is ground truth.
      this->pushed_frames_total_ = this->played_frames_total_ + frames;
      this->fb_samples_ = 0;
#ifdef USE_SNAPCLIENT_RATE_LOCK
      // The pipeline restart may have reprogrammed the I2S clock divider; re-read
      // the baseline before the next trim (the requested trim itself stays valid --
      // it is the learned crystal offset, a property of the hardware)
      if (this->rate_lock_ != nullptr) {
        this->rate_lock_->invalidate_baseline();
      }
#endif
    }
  }
  // Physical invariant: played can never exceed pushed. During a source starvation
  // the pipeline keeps reporting playback progress (it is playing fill, not our
  // audio) with no feedback gap, so the flush detector cannot fire; counting those
  // phantom frames permanently offsets the accounting by the starvation length
  // (observed: pipeline depth -268 ms during a receive stall, then a clean-looking
  // steady state playing ~230 ms audibly late). Clamp the excess -- the pivot then
  // tracks the stall truthfully and accounting stays exact through recovery.
  const int64_t available_frames = this->pushed_frames_total_ - this->played_frames_total_;
  if (static_cast<int64_t>(frames) > available_frames) {
    if (available_frames <= 0 && frames > 0) {
      // Complete drain: the framework tears its pipeline down and restarts it with
      // an unpredictable buffer fill level between this feedback point and the DAC
      // -- invisible to the accounting, so playback would settle audibly offset
      // (~100-250 ms observed) with clean-looking sync reports. Flag the player to
      // re-baseline from scratch when audio resumes.
      this->pipeline_starved_.store(true, std::memory_order_relaxed);
    }
    frames = static_cast<uint32_t>(std::max<int64_t>(available_frames, 0));
  }
  this->played_frames_total_ += frames;
  this->played_last_ts_us_ = timestamp_us;
  this->playout_valid_ = true;

  // Exponentially-weighted means of (frame index, DAC time); see the member comment.
  // Prediction extrapolates through this pivot along the exact nominal sample rate:
  // fitting a slope too would amplify its estimation noise over the pivot-to-now
  // lever arm into millisecond wobble, while real DAC-vs-esp_timer clock drift only
  // moves the pivot slowly — which the steering servo absorbs by design.
  constexpr double ALPHA = 1.0 / 64.0;
  const double f = static_cast<double>(this->played_frames_total_);
  const double t = static_cast<double>(timestamp_us);
  if (this->fb_samples_ == 0) {
    this->fb_mean_frames_ = f;
    this->fb_mean_ts_ = t;
  } else {
    this->fb_mean_frames_ += ALPHA * (f - this->fb_mean_frames_);
    this->fb_mean_ts_ += ALPHA * (t - this->fb_mean_ts_);
  }
  this->fb_samples_++;
  this->playout_mutex_.unlock();
}

// THREAD CONTEXT: Main loop (diagnostics)
float SnapcastClient::get_clock_offset_ms() {
  this->filter_mutex_.lock();
  float offset = this->time_filter_.has_estimate()
                     ? static_cast<float>(this->time_filter_.get_offset(now_us() / 1000.0))
                     : 0.0f;
  this->filter_mutex_.unlock();
  return offset;
}

// ============================== Network task ==============================

void SnapcastClient::network_task_() {
  while (!this->shutdown_.load(std::memory_order_relaxed)) {
    if (!this->network_ready_.load(std::memory_order_relaxed)) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    this->connection_session_();
    // No backoff when the session ended because the user changed the target
    if (!this->shutdown_.load(std::memory_order_relaxed) &&
        !this->reconnect_requested_.load(std::memory_order_relaxed)) {
      vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
  }
  vTaskDelete(nullptr);
}

void SnapcastClient::connection_session_() {
  this->reconnect_requested_.store(false, std::memory_order_relaxed);

  // Target precedence: override (manual/select entities) > configured server > mDNS
  std::string host;
  uint16_t port = this->config_.server_port;
  this->server_mutex_.lock();
  if (!this->override_host_.empty()) {
    host = this->override_host_;
    if (this->override_port_ != 0) {
      port = this->override_port_;
    }
  }
  this->server_mutex_.unlock();
  if (host.empty()) {
    host = this->config_.server_host;
  }

  // Scan on every attempt while no target is known -- a boot-time scan often comes
  // up empty (mDNS races the network coming up) and must retry on the reconnect
  // cadence, not a scan timer. When a target IS known and the server select wants
  // the list, refresh it opportunistically on reconnects (rate-limited -- each scan
  // blocks ~3 s). An empty scan keeps the previous list (last-known-good fallback).
  const bool need_discovery = host.empty();
  if (need_discovery ||
      (this->discovery_enabled_.load(std::memory_order_relaxed) &&
       (this->last_scan_us_ == 0 || now_us() - this->last_scan_us_ >= SERVER_SCAN_MIN_INTERVAL_US))) {
    this->scan_servers_();
  }
  if (need_discovery) {
    this->server_mutex_.lock();
    if (!this->discovered_servers_.empty()) {
      host = this->discovered_servers_[0].host;
      port = this->discovered_servers_[0].port;
    }
    this->server_mutex_.unlock();
    if (host.empty()) {
      return;  // nothing found; retried after the reconnect delay
    }
    ESP_LOGI(TAG, "Discovered snapserver at %s:%u", host.c_str(), port);
  }
  this->active_host_ = host;
  this->server_id_hash_ = fnv1_hash(host + ":" + std::to_string(port));
#ifdef SNAPCLIENT_TSF_ACTIVE
  this->last_peer_refresh_us_ = 0;  // fresh roster per session
#endif

  if (!this->connect_socket_(host, port)) {
    this->close_socket_();
    return;
  }

  ESP_LOGI(TAG, "Connected to %s:%u", host.c_str(), port);
  this->connected_.store(true, std::memory_order_relaxed);
  this->post_event_(Event{.type = EventType::CONNECTED});

  // Reset per-connection state. The Kalman filter's learned noise estimate survives
  // reset() by design, so re-sync converges quickly.
  this->filter_mutex_.lock();
  this->time_filter_.reset();
  this->filter_mutex_.unlock();
  this->codec_ = Codec::NONE;
  this->stream_params_ = StreamParams{};
  this->time_sync_burst_remaining_ = TIME_SYNC_BURST_COUNT;
  this->next_time_sync_us_ = 0;
  this->last_chunk_us_ = 0;

  std::string hello = build_hello_payload(this->config_.client_id.c_str(), this->config_.hostname);
  bool ok = this->send_message_(MessageType::HELLO, reinterpret_cast<const uint8_t *>(hello.data()), hello.size());

  uint8_t header[BaseMessage::WIRE_SIZE];
  while (ok && !this->shutdown_.load(std::memory_order_relaxed)) {
    if (!this->recv_exact_(header, sizeof(header))) {
      break;
    }
    const int64_t recv_us = now_us();
    BaseMessage base = BaseMessage::deserialize(header);
    if (base.size > MAX_PAYLOAD_SIZE) {
      ESP_LOGE(TAG, "Oversized message: type=%u size=%" PRIu32, base.type, base.size);
      break;
    }
    this->rx_buffer_.resize(base.size);
    if (base.size > 0 && !this->recv_exact_(this->rx_buffer_.data(), base.size)) {
      break;
    }

    switch (static_cast<MessageType>(base.type)) {
      case MessageType::WIRE_CHUNK:
        this->handle_wire_chunk_(this->rx_buffer_.data(), base.size);
        break;
      case MessageType::TIME:
        this->handle_time_reply_(base, this->rx_buffer_.data(), base.size, recv_us);
        break;
      case MessageType::CODEC_HEADER:
        this->handle_codec_header_(this->rx_buffer_.data(), base.size);
        break;
      case MessageType::SERVER_SETTINGS: {
        ServerSettings settings;
        if (ServerSettings::parse(this->rx_buffer_.data(), base.size, settings)) {
          ESP_LOGD(TAG, "Server settings: buffer=%" PRId32 " ms latency=%" PRId32 " ms volume=%u%% muted=%s",
                   settings.buffer_ms, settings.latency, settings.volume, YESNO(settings.muted));
          this->buffer_ms_.store(settings.buffer_ms, std::memory_order_relaxed);
          this->server_latency_ms_.store(settings.latency, std::memory_order_relaxed);
          this->post_event_(Event{.type = EventType::SERVER_SETTINGS, .settings = settings});
        }
        break;
      }
      default:
        ESP_LOGV(TAG, "Unhandled message type %u (%" PRIu32 " bytes)", base.type, base.size);
        break;
    }

    this->service_tx_();
  }

  this->close_socket_();
  if (this->control_session_ != nullptr) {
    this->control_session_->close();  // tied to the stream session's lifecycle
  }
  this->connected_.store(false, std::memory_order_relaxed);
  this->set_stream_active_(false);
  this->post_event_(Event{.type = EventType::DISCONNECTED});
  ESP_LOGW(TAG, "Disconnected from server");
}

// THREAD CONTEXT: Network task. The mDNS query blocks for up to its timeout; the
// espressif mdns library is safe to call from any task once initialized (ESPHome's
// mdns component initializes it when the network comes up, which is before
// network_ready_ lets us get here — a not-yet-ready stack just returns an error and
// we retry after the reconnect delay).
bool SnapcastClient::scan_servers_() {
#ifdef USE_MDNS
  this->last_scan_us_ = now_us();
  mdns_result_t *results = nullptr;
  esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 3000, 8, &results);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS query failed: %d", err);
    return false;
  }
  std::vector<ServerCandidate> servers;
  for (mdns_result_t *r = results; r != nullptr; r = r->next) {
    for (mdns_ip_addr_t *a = r->addr; a != nullptr; a = a->next) {
      if (a->addr.type == ESP_IPADDR_TYPE_V4) {
        char buf[16];
        esp_ip4addr_ntoa(&a->addr.u_addr.ip4, buf, sizeof(buf));
        ServerCandidate c;
        c.host = buf;
        c.port = r->port != 0 ? r->port : this->config_.server_port;
        if (r->instance_name != nullptr && r->instance_name[0] != '\0') {
          c.name = r->instance_name;
        } else if (r->hostname != nullptr && r->hostname[0] != '\0') {
          c.name = r->hostname;
        } else {
          c.name = c.host;
        }
        servers.push_back(std::move(c));
        break;
      }
    }
  }
  mdns_query_results_free(results);
  if (servers.empty()) {
    ESP_LOGW(TAG, "No snapserver found via mDNS (_snapcast._tcp)");
    return false;
  }
  this->server_mutex_.lock();
  // Dirty-flag only real changes so the listener isn't re-notified every reconnect
  if (servers != this->discovered_servers_) {
    this->discovered_servers_ = std::move(servers);
    this->discovered_dirty_ = true;
  }
  this->server_mutex_.unlock();
  return true;
#else
  ESP_LOGE(TAG, "No server configured and mDNS support is not compiled in");
  vTaskDelay(pdMS_TO_TICKS(5000));
  return false;
#endif
}

bool SnapcastClient::connect_socket_(const std::string &host, uint16_t port) {
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%u", port);

  struct addrinfo *res = nullptr;
  int err = getaddrinfo(host.c_str(), port_str, &hints, &res);
  if (err != 0 || res == nullptr) {
    ESP_LOGW(TAG, "DNS lookup for '%s' failed: %d", host.c_str(), err);
    return false;
  }

  this->sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (this->sock_ < 0) {
    freeaddrinfo(res);
    return false;
  }

  // Non-blocking connect with timeout
  int flags = fcntl(this->sock_, F_GETFL, 0);
  fcntl(this->sock_, F_SETFL, flags | O_NONBLOCK);
  err = connect(this->sock_, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (err != 0 && errno != EINPROGRESS) {
    ESP_LOGW(TAG, "Connect to %s failed: errno %d", host.c_str(), errno);
    return false;
  }
  if (err != 0) {
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(this->sock_, &write_set);
    struct timeval timeout = {.tv_sec = CONNECT_TIMEOUT_MS / 1000, .tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000};
    if (select(this->sock_ + 1, nullptr, &write_set, nullptr, &timeout) <= 0) {
      ESP_LOGW(TAG, "Connect to %s timed out", host.c_str());
      return false;
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(this->sock_, SOL_SOCKET, SO_ERROR, &so_error, &len);
    if (so_error != 0) {
      ESP_LOGW(TAG, "Connect to %s failed: errno %d", host.c_str(), so_error);
      return false;
    }
  }
  fcntl(this->sock_, F_SETFL, flags);

  // Time-sync accuracy benefits from unbatched small messages
  int nodelay = 1;
  setsockopt(this->sock_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  return true;
}

void SnapcastClient::close_socket_() {
  if (this->sock_ >= 0) {
    close(this->sock_);
    this->sock_ = -1;
  }
}

bool SnapcastClient::send_message_(MessageType type, const uint8_t *payload, size_t len, uint16_t refers_to) {
  BaseMessage base;
  base.type = static_cast<uint16_t>(type);
  base.id = ++this->next_message_id_;
  base.refers_to = refers_to;
  base.sent = TimeVal::from_us(now_us());
  base.size = len;

  uint8_t header[BaseMessage::WIRE_SIZE];
  base.serialize(header);

  // Header and payload in one buffer so TCP_NODELAY doesn't split them into two segments
  uint8_t stack_buf[BaseMessage::WIRE_SIZE + TimePayload::WIRE_SIZE];
  const uint8_t *buf;
  std::vector<uint8_t> heap_buf;
  size_t total = sizeof(header) + len;
  if (total <= sizeof(stack_buf)) {
    memcpy(stack_buf, header, sizeof(header));
    if (len > 0) {
      memcpy(stack_buf + sizeof(header), payload, len);
    }
    buf = stack_buf;
  } else {
    heap_buf.resize(total);
    memcpy(heap_buf.data(), header, sizeof(header));
    memcpy(heap_buf.data() + sizeof(header), payload, len);
    buf = heap_buf.data();
  }

  size_t sent = 0;
  while (sent < total) {
    int n = send(this->sock_, buf + sent, total - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += n;
  }
  return true;
}

bool SnapcastClient::recv_exact_(uint8_t *buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    if (this->shutdown_.load(std::memory_order_relaxed) || this->sock_ < 0 ||
        this->reconnect_requested_.load(std::memory_order_relaxed)) {
      return false;
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(this->sock_, &read_set);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    int ready = select(this->sock_ + 1, &read_set, nullptr, nullptr, &timeout);
    if (ready < 0) {
      return false;
    }
    if (ready == 0) {
      // Idle wait: keep time sync and ClientInfo flowing even when no stream is playing
      this->service_tx_();
      continue;
    }
    int n = recv(this->sock_, buf + got, len - got, 0);
    if (n <= 0) {
      return false;
    }
    got += n;
  }
  return true;
}

void SnapcastClient::service_tx_() {
  const int64_t now = now_us();

  // Time sync request. Adaptive cadence: the configured (fast) interval while a
  // stream is active — playout accuracy is being consumed — and a slow interval while
  // idle, which keeps the clock warm for instant stream starts without denying the
  // modem its power-save sleep windows.
  if (now >= this->next_time_sync_us_) {
    uint8_t payload[TimePayload::WIRE_SIZE];
    TimePayload{}.serialize(payload);
    this->send_message_(MessageType::TIME, payload, sizeof(payload));
    uint32_t interval_ms;
    if (this->time_sync_burst_remaining_ > 0) {
      this->time_sync_burst_remaining_--;
      interval_ms = TIME_SYNC_BURST_INTERVAL_MS;
    } else if (this->stream_active_) {
      interval_ms = this->config_.time_sync_interval_ms;
    } else {
      interval_ms = std::max(this->config_.time_sync_interval_ms, TIME_SYNC_IDLE_INTERVAL_MS);
    }
    this->next_time_sync_us_ = now + static_cast<int64_t>(interval_ms) * 1000;
  }

  // Pending ClientInfo (local volume/mute change)
  this->client_info_mutex_.lock();
  bool dirty = this->client_info_dirty_;
  uint8_t volume = this->client_info_volume_;
  bool muted = this->client_info_muted_;
  this->client_info_dirty_ = false;
  this->client_info_mutex_.unlock();
  if (dirty) {
    std::string payload = build_client_info_payload(volume, muted);
    this->send_message_(MessageType::CLIENT_INFO, reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
  }

  // Pending server-latency change (control API)
  this->client_info_mutex_.lock();
  const bool latency_dirty = this->latency_dirty_;
  const int32_t latency_ms = this->latency_pending_ms_;
  this->latency_dirty_ = false;
  this->client_info_mutex_.unlock();
  if (latency_dirty) {
    // Prefer the persistent control session; one-shot socket as the fallback
    // (control port may answer one-shots even when the session is mid-reconnect)
    if (this->control_session_ == nullptr || !this->control_session_->send_set_latency(latency_ms, now)) {
      this->send_set_latency_rpc_(latency_ms);
    }
  }

  // Persistent control session: metadata + roster + control RPCs (non-blocking)
  if (this->control_session_ != nullptr) {
    this->control_session_->service(now, this->active_host_);
#ifdef SNAPCLIENT_TSF_ACTIVE
    if (this->tsf_sync_ != nullptr) {
      std::vector<uint32_t> peers;
      if (this->control_session_->take_peers(peers)) {
        this->tsf_sync_->set_peers(std::move(peers));
      }
    }
#endif
  }

  // Stream idle detection: the server stops sending chunks when the group stream goes idle
  if (this->stream_active_ &&
      now - this->last_chunk_us_ > static_cast<int64_t>(this->config_.stream_idle_timeout_ms) * 1000) {
    ESP_LOGD(TAG, "Stream idle for %" PRIu32 " ms, ending stream", this->config_.stream_idle_timeout_ms);
    this->set_stream_active_(false);
  }

#ifdef SNAPCLIENT_TSF_ACTIVE
  if (this->tsf_sync_ != nullptr) {
    // Unicast roster: the control session feeds it live (non-blocking); the
    // blocking one-shot fetch remains only as the fallback when the control port
    // is unavailable -- once at session start, then refreshed only off-stream
    // (the RPC blocks up to ~2 s, which mid-stream would starve playout)
    const bool session_feeds_roster = this->control_session_ != nullptr && this->control_session_->connected();
    if (!session_feeds_roster &&
        (this->last_peer_refresh_us_ == 0 ||
         (!this->stream_active_ && now - this->last_peer_refresh_us_ >= TSF_PEER_REFRESH_US))) {
      this->refresh_tsf_peers_();
    }
    // Elections/beacons only while a stream is active: that is when deadlines are
    // computed AND when the hub holds high-performance wifi. While idle, modem
    // power save makes TSF reads fail intermittently (observed: sporadic beacons
    // and "TSF unreadable" role flapping on an idle pair). Roles freeze across
    // idle gaps; the leader resumes beaconing on the first active tick, and stale
    // mappings expire into the Kalman fallback on their own.
    if (this->stream_active_) {
      TsfSync::Estimate est;
      this->filter_mutex_.lock();
      est.valid = this->time_filter_.has_estimate();
      if (est.valid) {
        est.offset_ms = this->time_filter_.get_offset(now / 1000.0);
        est.drift = this->time_filter_.get_drift();
      }
      this->filter_mutex_.unlock();
      this->tsf_sync_->service(now, est, this->server_id_hash_);
    }
  }
#endif
}

// THREAD CONTEXT: Network task. A short-lived connection to the JSON-RPC control
// port; failures are logged and dropped (the entity re-syncs from the next
// ServerSettings push either way).
void SnapcastClient::send_set_latency_rpc_(int32_t latency_ms) {
  if (this->active_host_.empty()) {
    return;
  }
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  if (getaddrinfo(this->active_host_.c_str(), "1705", &hints, &res) != 0 || res == nullptr) {
    ESP_LOGW(TAG, "Control API DNS lookup failed");
    return;
  }
  int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock >= 0) {
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) == 0) {
      char req[192];
      const int len = snprintf(req, sizeof(req),
                               "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"Client.SetLatency\","
                               "\"params\":{\"id\":\"%s\",\"latency\":%" PRId32 "}}\r\n",
                               this->config_.client_id.c_str(), latency_ms);
      if (send(sock, req, len, 0) == len) {
        char reply[128];
        recv(sock, reply, sizeof(reply), 0);  // best-effort; drained for TCP hygiene
        ESP_LOGD(TAG, "Requested server latency %" PRId32 " ms", latency_ms);
      } else {
        ESP_LOGW(TAG, "Control API send failed");
      }
    } else {
      ESP_LOGW(TAG, "Control API connect to %s:1705 failed", this->active_host_.c_str());
    }
    close(sock);
  }
  freeaddrinfo(res);
}

#ifdef SNAPCLIENT_TSF_ACTIVE
// THREAD CONTEXT: Network task. Fetches the server's client roster so the TSF
// leader can unicast beacons to every peer -- client-to-client multicast is
// unreliable on many APs (isolation, IGMP snooping, mesh filtering), while unicast
// works wherever snapcast itself does. Blocking (1 s timeouts), so callers only
// invoke it while no stream is active; the session-start call is absorbed by the
// playout buffer.
void SnapcastClient::refresh_tsf_peers_() {
  this->last_peer_refresh_us_ = now_us();  // set even on failure: no hammering
  if (this->active_host_.empty() || this->tsf_sync_ == nullptr) {
    return;
  }
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  if (getaddrinfo(this->active_host_.c_str(), "1705", &hints, &res) != 0 || res == nullptr) {
    return;
  }
  std::string response;
  int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock >= 0) {
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) == 0) {
      static const char REQ[] = "{\"id\":2,\"jsonrpc\":\"2.0\",\"method\":\"Server.GetStatus\"}\r\n";
      if (send(sock, REQ, sizeof(REQ) - 1, 0) == static_cast<ssize_t>(sizeof(REQ) - 1)) {
        // Newline-delimited JSON-RPC; the full status can be several KB
        char buf[512];
        response.reserve(2048);
        while (response.size() < 24576) {
          const int n = recv(sock, buf, sizeof(buf), 0);
          if (n <= 0) {
            break;
          }
          response.append(buf, n);
          if (memchr(buf, '\n', n) != nullptr) {
            break;
          }
        }
      }
    }
    close(sock);
  }
  freeaddrinfo(res);
  if (response.empty()) {
    ESP_LOGD(TAG, "Server.GetStatus fetch failed; TSF peer roster unchanged");
    return;
  }

  // Filtered parse: keep only connected + host.ip of each client
  JsonDocument filter;
  filter["result"]["server"]["groups"][0]["clients"][0]["connected"] = true;
  filter["result"]["server"]["groups"][0]["clients"][0]["host"]["ip"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, response, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    ESP_LOGD(TAG, "Server.GetStatus parse failed; TSF peer roster unchanged");
    return;
  }
  std::vector<uint32_t> peers;
  // Implicit JsonArray/JsonVariant conversions: portable across ArduinoJson 7.x
  // (as<JsonArray>() on nested member proxies fails to compile on some versions);
  // a null array iterates as empty
  JsonArray groups = doc["result"]["server"]["groups"];
  for (JsonVariant group : groups) {
    JsonArray clients = group["clients"];
    for (JsonVariant client : clients) {
      if (!(client["connected"] | false)) {
        continue;
      }
      const char *ip = client["host"]["ip"];
      if (ip == nullptr) {
        continue;
      }
      if (strncmp(ip, "::ffff:", 7) == 0) {
        ip += 7;  // snapserver reports IPv4-mapped IPv6 addresses
      }
      const in_addr_t addr = inet_addr(ip);
      if (addr == INADDR_NONE) {
        continue;  // plain IPv6 or garbage
      }
      // Our own address may be included; the beacon's own-mac check drops it
      peers.push_back(addr);
    }
  }
  this->tsf_sync_->set_peers(std::move(peers));
}
#endif

void SnapcastClient::handle_time_reply_(const BaseMessage &base, const uint8_t *payload, size_t len, int64_t recv_us) {
  TimePayload time_payload;
  if (!TimePayload::parse(payload, len, time_payload)) {
    return;
  }
  // Server reply: payload.latency = client-to-server (server receive - client send),
  // base.sent = server send time. s2c = client receive - server send.
  // The offset measurement (server - client) is (c2s - s2c) / 2.
  const int64_t c2s_us = time_payload.latency.to_us();
  const int64_t s2c_us = recv_us - base.sent.to_us();

  // RTT gate: c2s + s2c is the round trip, with the clock offsets cancelled out.
  // Samples far above the observed minimum are congestion/roam artifacts whose
  // asymmetric delay would walk the offset estimate away from truth — a burst of
  // them (e.g. right after a wifi roam) defeats even the Huber weighting, leaving
  // playback consistently behind until clean samples slowly win back. The floor
  // leaks upward slowly so a genuinely changed network re-baselines within minutes.
  const int64_t rtt_us = c2s_us + s2c_us;
  this->min_rtt_us_ = std::min(rtt_us, this->min_rtt_us_ + RTT_FLOOR_LEAK_US);
  if (rtt_us > this->min_rtt_us_ + RTT_GATE_US) {
    ESP_LOGV(TAG, "Time sync sample rejected: rtt %" PRId64 " us (floor %" PRId64 " us)", rtt_us, this->min_rtt_us_);
    return;
  }

  const double measurement_ms = static_cast<double>(c2s_us - s2c_us) / 2000.0;

  this->filter_mutex_.lock();
  this->time_filter_.insert(measurement_ms, recv_us / 1000.0);
  this->filter_mutex_.unlock();
}

void SnapcastClient::handle_codec_header_(const uint8_t *payload, size_t len) {
  CodecHeaderView header;
  if (!CodecHeaderView::parse(payload, len, header)) {
    ESP_LOGE(TAG, "Malformed codec header");
    return;
  }
  ESP_LOGI(TAG, "Codec: %.*s (%zu byte header)", static_cast<int>(header.codec_len), header.codec,
           header.payload_len);

  // A new codec header means a new stream; anything mid-flight belongs to the old one
  this->set_stream_active_(false);
  this->codec_ = Codec::NONE;
  this->stream_params_ = StreamParams{};

  if (header.codec_is("pcm")) {
    // Payload is a RIFF/WAVE header; the PCM format lives in the fmt chunk at fixed offsets
    if (header.payload_len < 44) {
      ESP_LOGE(TAG, "PCM codec header too short");
      return;
    }
    const uint8_t *p = header.payload;
    this->stream_params_.channels = p[22] | (p[23] << 8);
    this->stream_params_.sample_rate =
        static_cast<uint32_t>(p[24]) | (p[25] << 8) | (static_cast<uint32_t>(p[26]) << 16) |
        (static_cast<uint32_t>(p[27]) << 24);
    this->stream_params_.bits_per_sample = p[34] | (p[35] << 8);
    if (!this->stream_params_.valid() || this->stream_params_.bits_per_sample != 16) {
      ESP_LOGE(TAG, "Unsupported PCM format: %" PRIu32 " Hz, %u bit, %u ch", this->stream_params_.sample_rate,
               this->stream_params_.bits_per_sample, this->stream_params_.channels);
      this->stream_params_ = StreamParams{};
      return;
    }
    this->codec_ = Codec::PCM;
  } else if (header.codec_is("flac")) {
#ifdef USE_SNAPCLIENT_FLAC
    this->flac_decoder_ = std::make_unique<micro_flac::FLACDecoder>();
    this->flac_header_done_ = false;
    this->flac_input_.assign(header.payload, header.payload + header.payload_len);
    this->flac_output_.clear();
    this->decode_flac_input_(-1);
    if (this->flac_header_done_) {
      this->codec_ = Codec::FLAC;
    } else {
      ESP_LOGE(TAG, "FLAC stream header did not parse");
      this->flac_decoder_.reset();
    }
#else
    ESP_LOGE(TAG, "FLAC stream received but FLAC support is disabled (set `flac: true` on the snapclient component)");
#endif
  } else {
    ESP_LOGE(TAG, "Unsupported codec '%.*s' — set the snapserver stream codec to flac or pcm",
             static_cast<int>(header.codec_len), header.codec);
  }

  if (this->codec_ != Codec::NONE) {
    ESP_LOGI(TAG, "Stream format: %" PRIu32 " Hz, %u bit, %u ch", this->stream_params_.sample_rate,
             this->stream_params_.bits_per_sample, this->stream_params_.channels);
  }
}

void SnapcastClient::handle_wire_chunk_(const uint8_t *payload, size_t len) {
  WireChunkView chunk;
  if (!WireChunkView::parse(payload, len, chunk)) {
    return;
  }
  if (this->codec_ == Codec::NONE || !this->stream_params_.valid()) {
    return;
  }

  this->last_chunk_us_ = now_us();
  if (!this->stream_active_) {
    this->set_stream_active_(true);
  }

  const int64_t server_ts_us = chunk.timestamp.to_us();
  switch (this->codec_) {
    case Codec::PCM:
      this->emit_pcm_(chunk.payload, chunk.payload_len, server_ts_us);
      break;
#ifdef USE_SNAPCLIENT_FLAC
    case Codec::FLAC:
      this->flac_input_.insert(this->flac_input_.end(), chunk.payload, chunk.payload + chunk.payload_len);
      this->decode_flac_input_(server_ts_us);
      break;
#endif
    default:
      break;
  }
}

#ifdef USE_SNAPCLIENT_FLAC
void SnapcastClient::decode_flac_input_(int64_t server_ts_us) {
  // snapserver flushes the FLAC encoder at every chunk boundary, so in practice each
  // wire chunk decodes to exactly its own samples. The carry-over buffer makes frame
  // spans across chunk boundaries safe anyway; output emitted while processing a chunk
  // is stamped with that chunk's timestamp.
  size_t offset = 0;
  while (offset < this->flac_input_.size()) {
    size_t bytes_consumed = 0;
    size_t samples_decoded = 0;
    micro_flac::FLACDecoderResult result = this->flac_decoder_->decode(
        this->flac_input_.data() + offset, this->flac_input_.size() - offset, this->flac_output_.data(),
        this->flac_output_.size(), bytes_consumed, samples_decoded);

    if (result == micro_flac::FLAC_DECODER_SUCCESS) {
      offset += bytes_consumed;
      if (samples_decoded > 0 && server_ts_us >= 0) {
        const size_t bytes = samples_decoded * (this->stream_params_.bits_per_sample / 8);
        this->emit_pcm_(this->flac_output_.data(), bytes, server_ts_us);
      }
    } else if (result == micro_flac::FLAC_DECODER_HEADER_READY) {
      offset += bytes_consumed;
      const auto &info = this->flac_decoder_->get_stream_info();
      this->stream_params_.sample_rate = info.sample_rate();
      this->stream_params_.bits_per_sample = info.bits_per_sample();
      this->stream_params_.channels = info.num_channels();
      this->flac_output_.resize(this->flac_decoder_->get_output_buffer_size_samples() * info.bytes_per_sample());
      this->flac_header_done_ = true;
    } else if (result == micro_flac::FLAC_DECODER_ERROR_OUTPUT_TOO_SMALL) {
      const auto &info = this->flac_decoder_->get_stream_info();
      this->flac_output_.resize(this->flac_decoder_->get_output_buffer_size_samples() * info.bytes_per_sample());
    } else if (result == micro_flac::FLAC_DECODER_NEED_MORE_DATA) {
      offset += bytes_consumed;
      break;
    } else {
      ESP_LOGW(TAG, "FLAC decode error %d, discarding buffered input", static_cast<int>(result));
      offset = this->flac_input_.size();
      break;
    }
  }
  this->flac_input_.erase(this->flac_input_.begin(), this->flac_input_.begin() + offset);
}
#endif

void SnapcastClient::emit_pcm_(const uint8_t *data, size_t len, int64_t server_ts_us) {
  if (len == 0) {
    return;
  }
  // Write the PCM first, then post the record: the player may then rely on a popped
  // record's bytes being fully present in the ring. A full ring blocks here, which
  // backpressures the TCP connection exactly like a desktop snapclient.
  size_t written = 0;
  while (written < len && !this->shutdown_.load(std::memory_order_relaxed)) {
    written += this->pcm_ring_->write_without_replacement(data + written, len - written, pdMS_TO_TICKS(100));
  }
  if (written < len) {
    return;
  }
  ChunkRecord record{.server_ts_us = server_ts_us, .bytes = static_cast<uint32_t>(len), .params = this->stream_params_};
  while (xQueueSend(this->record_queue_, &record, pdMS_TO_TICKS(100)) != pdTRUE) {
    if (this->shutdown_.load(std::memory_order_relaxed)) {
      return;
    }
  }
}

void SnapcastClient::post_event_(const Event &event) {
  // The main loop drains this queue every iteration; drop rather than block if it
  // somehow backs up.
  xQueueSend(this->event_queue_, &event, pdMS_TO_TICKS(50));
}

void SnapcastClient::set_stream_active_(bool active) {
  if (active == this->stream_active_) {
    return;
  }
  this->stream_active_ = active;
  if (active) {
    // Accuracy starts being consumed now: engage the fast cadence immediately with a
    // short burst to refresh the estimate after a possibly long idle stretch
    this->time_sync_burst_remaining_ = std::max<uint32_t>(this->time_sync_burst_remaining_, 3);
    this->next_time_sync_us_ = 0;
    this->post_event_(Event{.type = EventType::STREAM_START, .params = this->stream_params_});
  } else {
    this->post_event_(Event{.type = EventType::STREAM_END});
  }
}

// ============================== Player task ==============================

void SnapcastClient::player_task_() {
  bool warned_no_sync = false;
  // Rolling sync-error diagnostics, logged once per ~128 chunks (~3 s)
  int64_t err_accum_us = 0;
  int64_t err_peak_us = 0;
  uint32_t err_count = 0;
  // Per-window stutter forensics: how often each correction mechanism fired
  uint32_t soft_dropped_frames = 0;
  uint32_t soft_inserted_frames = 0;
  uint32_t hard_resyncs = 0;
  // Median of recent sync errors (rejects residual feedback spikes better than a
  // mean); the steering servo acts on this, not the raw per-chunk error. Same design
  // as the esp32 snapclient reference (99/19-sample medians on a sample-accurate age).
  int64_t err_window[MEDIAN_WINDOW];
  size_t err_window_idx = 0;
  size_t err_window_filled = 0;
  // Bang-bang steering with hysteresis, ported from the reference: while engaged,
  // trim exactly one frame per chunk (~950 ppm) until the median crosses back inside
  // the disengage threshold. Holds the error near zero continuously instead of
  // letting it random-walk inside a deadband — a free-walking deadband is exactly
  // what wanders the stereo image between two paired devices.
  int8_t steer_dir = 0;
#ifdef USE_SNAPCLIENT_RATE_LOCK
  // Rate lock: once converged, steady-state corrections become hardware clock trims
  // instead of frame splices. The PI integrator (positive = play faster) persists
  // across resyncs, flushes, and rate changes because it converges to the crystal
  // offset, a property of the hardware, not the stream.
  float trim_integral_ppm = 0.0f;
  bool rate_lock_ok = this->rate_lock_ != nullptr;
  uint32_t rate_lock_rate = 0;
#endif
  // Mute-until-synced: real audio flows only after the first in-band median
  bool converged = false;
  int64_t last_resync_log_us = 0;
  while (!this->shutdown_.load(std::memory_order_relaxed)) {
    ChunkRecord rec;
    if (xQueueReceive(this->record_queue_, &rec, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;
    }

    const uint32_t frame_bytes = rec.params.frame_bytes();
    if (frame_bytes == 0) {
      this->discard_ring_bytes_(rec.bytes);
      continue;
    }

#ifdef USE_SNAPCLIENT_RATE_LOCK
    if (rate_lock_ok && rec.params.sample_rate != rate_lock_rate) {
      // The speaker reprograms the I2S clock for a new stream format; re-read the
      // divider baseline once the new clock is running
      this->rate_lock_->invalidate_baseline();
      rate_lock_rate = rec.params.sample_rate;
    }
#endif

    if (!this->output_active_.load(std::memory_order_relaxed)) {
      // No consumer: discard immediately. New chunks arrive continuously, so playback
      // starts in sync as soon as the source is activated.
      this->discard_ring_bytes_(rec.bytes);
      continue;
    }

    if (this->pipeline_starved_.exchange(false, std::memory_order_relaxed)) {
      // The pipeline fully drained (source starvation); the framework restarts its
      // buffers at a fill level the feedback point cannot observe. Reset the playout
      // accounting so playback re-baselines exactly like a fresh start (whose fill
      // is deterministic), at the cost of one muted re-lock -- the automatic version
      // of the manual speaker restart that used to be the fix.
      this->playout_mutex_.lock();
      this->playout_valid_ = false;
      this->played_frames_total_ = 0;
      this->pushed_frames_total_ = 0;
      this->fb_samples_ = 0;
      this->playout_mutex_.unlock();
      err_window_filled = 0;
      steer_dir = 0;
      converged = false;
#ifdef USE_SNAPCLIENT_RATE_LOCK
      if (this->rate_lock_ != nullptr) {
        this->rate_lock_->invalidate_baseline();
      }
#endif
      ESP_LOGI(TAG, "Pipeline drained (source starvation); re-baselining playout");
    }

    const int64_t deadline = this->chunk_deadline_us_(rec);
    const int64_t hard_us = static_cast<int64_t>(this->config_.hard_resync_threshold_ms) * 1000;
    const uint32_t frames = rec.bytes / frame_bytes;
    const int64_t predicted = this->predict_next_play_us_(rec.params.sample_rate);

    if (predicted < 0) {
      // No playback feedback yet: gate the first push by wall clock, leaving the
      // pipeline STARTUP_LEAD_US to spin up; feedback-based correction takes over
      // after the speaker's first output callback.
      if (now_us() > deadline) {
        this->discard_ring_bytes_(rec.bytes);
        continue;
      }
      while (now_us() < deadline - STARTUP_LEAD_US && this->output_active_.load(std::memory_order_relaxed) &&
             !this->shutdown_.load(std::memory_order_relaxed)) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if (!warned_no_sync) {
        this->filter_mutex_.lock();
        warned_no_sync = !this->time_filter_.has_estimate();
        this->filter_mutex_.unlock();
        if (warned_no_sync) {
          ESP_LOGW(TAG, "Starting playback before first time sync; expect a hard resync");
        }
      }
      this->push_chunk_(rec, 0, true);
      continue;
    }

    const int64_t error_us = predicted - deadline;  // >0: this chunk would play late

    err_accum_us += error_us;
    err_peak_us = std::max(err_peak_us, std::abs(error_us));

    err_window[err_window_idx] = error_us;
    err_window_idx = (err_window_idx + 1) % MEDIAN_WINDOW;
    if (err_window_filled < MEDIAN_WINDOW) {
      err_window_filled++;
    }
    int64_t median_err_us = error_us;
    if (err_window_filled == MEDIAN_WINDOW) {
      int64_t sorted[MEDIAN_WINDOW];
      memcpy(sorted, err_window, sizeof(sorted));
      std::nth_element(sorted, sorted + MEDIAN_WINDOW / 2, sorted + MEDIAN_WINDOW);
      median_err_us = sorted[MEDIAN_WINDOW / 2];
    }

    if (++err_count >= 128) {
      int64_t max_gap_us;
      int64_t pipeline_frames;
      this->playout_mutex_.lock();
      max_gap_us = this->max_feedback_gap_us_;
      this->max_feedback_gap_us_ = 0;
      pipeline_frames = this->pushed_frames_total_ - this->played_frames_total_;
      this->playout_mutex_.unlock();
      // Accounted pipeline depth (pushed-but-unplayed). Sane: a stable few hundred
      // ms (mixer + speaker buffers). Divergence from reality is invisible to the
      // servo -- a value outside ~0..500 ms means the accounting has split from the
      // pipeline (e.g. an unnoticed flush) and playback is audibly offset while the
      // report looks clean.
      const int32_t pipeline_ms =
          static_cast<int32_t>(pipeline_frames * 1000 / static_cast<int64_t>(rec.params.sample_rate));
      // Ring occupancy shows how much dropout cushion is actually held client-side
      const uint32_t buffered_ms = static_cast<uint32_t>(
          static_cast<uint64_t>(this->pcm_ring_->available()) * 1000 / (frame_bytes * rec.params.sample_rate));
      char trim_str[32] = "";
#ifdef USE_SNAPCLIENT_RATE_LOCK
      if (rate_lock_ok) {
        snprintf(trim_str, sizeof(trim_str), ", trim %+.2f ppm", this->rate_lock_->applied_ppm());
      }
#endif
      char tsf_str[32] = "";
#ifdef SNAPCLIENT_TSF_ACTIVE
      if (this->tsf_sync_ != nullptr) {
        const TsfSync::Role role = this->tsf_sync_->role();
        if (role == TsfSync::Role::LEADER) {
          snprintf(tsf_str, sizeof(tsf_str), ", tsf=leader(peers %u)", this->tsf_sync_->peer_count());
        } else if (role == TsfSync::Role::FOLLOWER) {
          snprintf(tsf_str, sizeof(tsf_str), ", tsf=follower(%.1fs)", this->tsf_sync_->mapping_age_s(now_us()));
        }
      }
#endif
      ESP_LOGD(TAG,
               "Sync: avg %" PRId64 " us, peak %" PRId64 " us, median %" PRId64
               " us | corrected -%" PRIu32 "/+%" PRIu32 " frames, %" PRIu32 " hard resyncs, max feedback gap %" PRId64
               " ms, buffered %" PRIu32 " ms, pipeline %" PRId32 " ms%s%s over %" PRIu32 " chunks",
               err_accum_us / err_count, err_peak_us, median_err_us, soft_dropped_frames, soft_inserted_frames,
               hard_resyncs, max_gap_us / 1000, buffered_ms, pipeline_ms, trim_str, tsf_str, err_count);
      err_accum_us = 0;
      err_peak_us = 0;
      err_count = 0;
      soft_dropped_frames = 0;
      soft_inserted_frames = 0;
      hard_resyncs = 0;
    }

    // Hard-resync logging is throttled to one line per RESYNC_LOG_INTERVAL_US: during
    // a recovery storm this fires per chunk, and when logs stream over the api the
    // log traffic competes with the audio stream on the already-congested link — a
    // feedback loop that prolongs the outage. The periodic sync report carries the
    // full per-window count either way.
    if (error_us > hard_us) {
      // Hard resync, late: drop whole chunks until we catch back up
      if (now_us() - last_resync_log_us >= RESYNC_LOG_INTERVAL_US) {
        last_resync_log_us = now_us();
        ESP_LOGD(TAG, "Hard resync: %" PRId64 " ms late, dropping chunks (throttled log)", error_us / 1000);
      }
      hard_resyncs++;
      err_window_filled = 0;
      steer_dir = 0;
      converged = false;
      this->discard_ring_bytes_(rec.bytes);
      continue;
    }

    uint32_t drop_frames = 0;
    if (error_us < -hard_us) {
      // Hard resync, early: fill the gap with silence (bounded per chunk so the
      // loop stays responsive), keeping the DAC fed and continuous
      const int64_t gap_frames = (-error_us) * rec.params.sample_rate / 1000000;
      const uint32_t fill = std::min<int64_t>(gap_frames, rec.params.sample_rate / 2);
      if (now_us() - last_resync_log_us >= RESYNC_LOG_INTERVAL_US) {
        last_resync_log_us = now_us();
        ESP_LOGD(TAG, "Hard resync: %" PRId64 " ms early, inserting silence (throttled log)", -error_us / 1000);
      }
      hard_resyncs++;
      err_window_filled = 0;
      steer_dir = 0;
      converged = false;
      this->push_silence_(fill, rec.params);
    } else if (std::abs(median_err_us) > SOFT_CORRECTION_AGGRESSIVE_US) {
      // Post-stall catch-up: frames/32 bursts (~33 ms/s convergence) so a backlog
      // doesn't leave playback audibly behind for long
      const int32_t adjust_frames =
          static_cast<int32_t>(median_err_us * static_cast<int64_t>(rec.params.sample_rate) / 1000000);
      const int32_t max_adjust = std::max<int32_t>(1, frames / (SOFT_CORRECTION_DIVISOR / 4));
      const int32_t adjust = std::clamp(adjust_frames, -max_adjust, max_adjust);
      if (adjust > 0) {
        drop_frames = adjust;
        soft_dropped_frames += adjust;
      } else if (adjust < 0) {
        soft_inserted_frames += -adjust;
        this->push_silence_(-adjust, rec.params);
      }
      steer_dir = 0;
    } else if (err_window_filled == MEDIAN_WINDOW) {
      // Steering servo (reference design): engage when the median error exceeds
      // sync_deadband, then trim exactly one frame (~23 us splice, inaudible) per
      // chunk until it crosses back inside half the threshold. Continuous hold near
      // zero is what keeps a stereo pair's image pinned.
      const int64_t engage_us = this->config_.sync_deadband_us;
      if (steer_dir == 0) {
        if (median_err_us > engage_us) {
          steer_dir = 1;
        } else if (median_err_us < -engage_us) {
          steer_dir = -1;
        }
      } else if ((steer_dir > 0 && median_err_us < engage_us / 2) ||
                 (steer_dir < 0 && median_err_us > -engage_us / 2)) {
        steer_dir = 0;
      }
      bool trim_holds = false;
#ifdef USE_SNAPCLIENT_RATE_LOCK
      // Steady-state rate lock: steer the I2S clock instead of splicing frames.
      // Continuous PI on the median error, no deadband -- trims are inaudible, and
      // gating them through the hysteresis band re-creates the limit cycle. The
      // hysteresis/steer_dir path above still drives the splice fallback.
      // Pre-convergence stays on hard splices: muted convergence at 8 frames/chunk
      // is far faster than clock steering could ever be.
      if (converged && rate_lock_ok) {
        const float dt_s = static_cast<float>(frames) / rec.params.sample_rate;
        trim_integral_ppm = std::clamp(
            trim_integral_ppm + TRIM_KI_PPM_PER_US_S * static_cast<float>(median_err_us) * dt_s, -TRIM_CLAMP_PPM,
            TRIM_CLAMP_PPM);
        const float trim_ppm =
            std::clamp(TRIM_KP_PPM_PER_US * static_cast<float>(median_err_us) + trim_integral_ppm, -TRIM_CLAMP_PPM,
                       TRIM_CLAMP_PPM);
        trim_holds = this->rate_lock_->set_trim_ppm(trim_ppm);
        if (!trim_holds) {
          rate_lock_ok = false;
          ESP_LOGW(TAG, "Rate lock unavailable, falling back to frame-splice corrections");
        }
      }
#endif
      // While muted (pre-convergence) audibility doesn't constrain splice size, so
      // steer hard to reach the band quickly instead of crawling in at ~0.9 ms/s
      if (!trim_holds) {
        const uint32_t steer_frames = converged ? 1 : STARTUP_STEER_FRAMES;
        if (steer_dir > 0) {
          drop_frames = steer_frames;
          soft_dropped_frames += steer_frames;
        } else if (steer_dir < 0) {
          soft_inserted_frames += steer_frames;
          if (converged) {
            this->push_repeat_frame_(rec.params);
          } else {
            this->push_silence_(steer_frames, rec.params);
          }
        }
      }
    }

    // Mute-until-synced (reference behavior): convergence corrections are chunky and
    // audible (drops of 14 frames/chunk in the proportional band), so the audio is
    // replaced with silence until the median error first lands inside the servo
    // band. Hard resyncs re-mute, turning recovery storms into silent gaps.
    if (!converged && err_window_filled == MEDIAN_WINDOW &&
        std::abs(median_err_us) <= this->config_.sync_deadband_us) {
      converged = true;
      ESP_LOGI(TAG, "Sync locked (median %" PRId64 " us), unmuting", median_err_us);
    }

    this->push_chunk_(rec, drop_frames, !converged);
  }
  vTaskDelete(nullptr);
}

int64_t SnapcastClient::predict_next_play_us_(uint32_t sample_rate) {
  this->playout_mutex_.lock();
  int64_t predicted = -1;
  if (this->playout_valid_) {
    const double nominal_slope = 1e6 / static_cast<double>(sample_rate);
    if (this->fb_samples_ >= 8) {
      // Smoothed pivot + exact nominal slope: averages away feedback quantization
      // without a fitted slope's lever-arm instability (see notify_audio_played)
      predicted = static_cast<int64_t>(
          this->fb_mean_ts_ +
          nominal_slope * (static_cast<double>(this->pushed_frames_total_) - this->fb_mean_frames_));
    } else {
      const int64_t queued_frames = this->pushed_frames_total_ - this->played_frames_total_;
      predicted = this->played_last_ts_us_ + queued_frames * 1000000 / static_cast<int64_t>(sample_rate);
    }
  }
  this->playout_mutex_.unlock();
  return predicted;
}

int64_t SnapcastClient::chunk_deadline_us_(const ChunkRecord &rec) {
  // Effective playout buffer, matching the reference client (controller.cpp):
  // max(0, bufferMs - serverLatency - localLatency)
  const int64_t buffer_us =
      std::max<int64_t>(0, static_cast<int64_t>(this->buffer_ms_.load(std::memory_order_relaxed)) -
                               this->server_latency_ms_.load(std::memory_order_relaxed) -
                               this->static_delay_ms_.load(std::memory_order_relaxed)) *
      1000;

#ifdef SNAPCLIENT_TSF_ACTIVE
  // TSF group sync: prefer the AP-shared server->TSF mapping so every same-AP
  // client derives identical deadlines (estimate wander becomes common-mode)
  if (this->tsf_sync_ != nullptr) {
    int64_t shared_offset_us;
    if (this->tsf_sync_->shared_server_offset_us(now_us(), shared_offset_us)) {
      return rec.server_ts_us + buffer_us - shared_offset_us;
    }
  }
#endif

  this->filter_mutex_.lock();
  const double offset_ms = this->time_filter_.has_estimate() ? this->time_filter_.get_offset(now_us() / 1000.0) : 0.0;
  this->filter_mutex_.unlock();
  return rec.server_ts_us + buffer_us - static_cast<int64_t>(offset_ms * 1000.0);
}

void SnapcastClient::discard_ring_bytes_(size_t bytes) {
  while (bytes > 0 && !this->shutdown_.load(std::memory_order_relaxed)) {
    size_t n = this->pcm_ring_->read(this->slice_buffer_.get(), std::min(bytes, SLICE_BUFFER_SIZE),
                                     pdMS_TO_TICKS(100));
    bytes -= n;
  }
}

uint32_t SnapcastClient::push_silence_(uint32_t frames, const StreamParams &params) {
  if (this->audio_listener_ == nullptr) {
    return 0;
  }
  const uint32_t frame_bytes = params.frame_bytes();
  memset(this->slice_buffer_.get(), 0, SLICE_BUFFER_SIZE);
  uint32_t pushed = 0;
  while (pushed < frames && this->output_active_.load(std::memory_order_relaxed) &&
         !this->shutdown_.load(std::memory_order_relaxed)) {
    const uint32_t batch = std::min<uint32_t>(frames - pushed, SLICE_BUFFER_SIZE / frame_bytes);
    size_t written = this->audio_listener_->on_audio_write(this->slice_buffer_.get(), batch * frame_bytes, 100, params);
    if (written == 0) {
      break;
    }
    const uint32_t written_frames = written / frame_bytes;
    pushed += written_frames;
    this->playout_mutex_.lock();
    this->pushed_frames_total_ += written_frames;
    this->playout_mutex_.unlock();
  }
  return pushed;
}

// Polarity inversion with INT16_MIN clamp (negating INT16_MIN overflows)
static inline int16_t invert_sample(int16_t s) { return s == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-s); }

// THREAD CONTEXT: Player task. Slices are always frame-aligned: chunk sizes and
// SLICE_BUFFER_SIZE are multiples of the frame size, and reads are sequential.
void SnapcastClient::apply_channel_mode_(uint8_t *data, size_t len, const StreamParams &params) {
  if (params.bits_per_sample != 16) {
    return;
  }
  auto *samples = reinterpret_cast<int16_t *>(data);

  // Channel routing first: phase inversion below refers to the *output* channels
  const auto mode = static_cast<ChannelMode>(this->channel_mode_.load(std::memory_order_relaxed));
  if (mode != ChannelMode::STEREO && params.channels == 2) {
    const size_t frames = len / 4;
    switch (mode) {
      case ChannelMode::LEFT_ONLY:
        for (size_t i = 0; i < frames; i++) {
          samples[2 * i + 1] = samples[2 * i];
        }
        break;
      case ChannelMode::RIGHT_ONLY:
        for (size_t i = 0; i < frames; i++) {
          samples[2 * i] = samples[2 * i + 1];
        }
        break;
      case ChannelMode::MONO:
        for (size_t i = 0; i < frames; i++) {
          const int16_t mixed = static_cast<int16_t>((static_cast<int32_t>(samples[2 * i]) + samples[2 * i + 1]) / 2);
          samples[2 * i] = mixed;
          samples[2 * i + 1] = mixed;
        }
        break;
      default:
        break;
    }
  }

  const auto phase = static_cast<PhaseMode>(this->phase_mode_.load(std::memory_order_relaxed));
  if (phase != PhaseMode::NONE) {
    if (params.channels == 2) {
      const size_t frames = len / 4;
      const bool left = phase != PhaseMode::RIGHT;
      const bool right = phase != PhaseMode::LEFT;
      for (size_t i = 0; i < frames; i++) {
        if (left) {
          samples[2 * i] = invert_sample(samples[2 * i]);
        }
        if (right) {
          samples[2 * i + 1] = invert_sample(samples[2 * i + 1]);
        }
      }
    } else if (params.channels == 1) {
      // Mono has no L/R distinction; any inversion setting inverts the one channel
      const size_t n = len / 2;
      for (size_t i = 0; i < n; i++) {
        samples[i] = invert_sample(samples[i]);
      }
    }
  }
}

// THREAD CONTEXT: Player task
void SnapcastClient::push_repeat_frame_(const StreamParams &params) {
  const uint32_t frame_bytes = params.frame_bytes();
  if (this->audio_listener_ == nullptr || frame_bytes == 0 || frame_bytes > sizeof(this->last_frame_)) {
    return;
  }
  if (this->last_frame_bytes_ != frame_bytes) {
    // No cached frame in this format yet
    this->push_silence_(1, params);
    return;
  }
  size_t offset = 0;
  while (offset < frame_bytes && this->output_active_.load(std::memory_order_relaxed) &&
         !this->shutdown_.load(std::memory_order_relaxed)) {
    const size_t written =
        this->audio_listener_->on_audio_write(this->last_frame_ + offset, frame_bytes - offset, 100, params);
    if (written == 0) {
      return;
    }
    offset += written;
  }
  this->playout_mutex_.lock();
  this->pushed_frames_total_ += 1;
  this->playout_mutex_.unlock();
}

void SnapcastClient::push_chunk_(const ChunkRecord &rec, uint32_t drop_frames, bool silent) {
  const uint32_t frame_bytes = rec.params.frame_bytes();
  size_t remaining = rec.bytes;
  size_t skip = std::min<size_t>(static_cast<size_t>(drop_frames) * frame_bytes, remaining);

  while (remaining > 0 && !this->shutdown_.load(std::memory_order_relaxed)) {
    const size_t want = std::min(remaining, SLICE_BUFFER_SIZE);
    size_t got = 0;
    while (got < want && !this->shutdown_.load(std::memory_order_relaxed)) {
      got += this->pcm_ring_->read(this->slice_buffer_.get() + got, want - got, pdMS_TO_TICKS(100));
    }
    remaining -= got;

    if (silent) {
      memset(this->slice_buffer_.get(), 0, got);
    }
    this->apply_channel_mode_(this->slice_buffer_.get(), got, rec.params);

    // Cache the slice's final frame (post-transform) for click-free servo insertion
    if (got >= frame_bytes && frame_bytes <= sizeof(this->last_frame_)) {
      memcpy(this->last_frame_, this->slice_buffer_.get() + got - frame_bytes, frame_bytes);
      this->last_frame_bytes_ = frame_bytes;
    }

    size_t offset = 0;
    if (skip > 0) {
      offset = std::min(skip, got);
      skip -= offset;
    }

    while (offset < got) {
      if (this->audio_listener_ == nullptr || !this->output_active_.load(std::memory_order_relaxed)) {
        // Consumer went away mid-chunk: discard the rest, deadlines keep us honest
        this->discard_ring_bytes_(remaining);
        return;
      }
      size_t written = this->audio_listener_->on_audio_write(this->slice_buffer_.get() + offset, got - offset, 100,
                                                             rec.params);
      offset += written;
      if (written > 0) {
        this->playout_mutex_.lock();
        this->pushed_frames_total_ += written / frame_bytes;
        this->playout_mutex_.unlock();
      }
    }
  }
}

}  // namespace esphome::snapclient

#endif  // USE_ESP32
