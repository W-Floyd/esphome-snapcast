#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "control_session.h"
#include "esphome/components/i2s_rate_lock/rate_lock.h"
#include "snapcast_proto.h"
#include "esphome/components/clock_sync/time_filter.h"
#include "esphome/components/clock_sync/tsf_sync.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

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
#include <cmath>
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
  // Engage threshold (us) for fast POSITION correction while converged; 0 disables it.
  // See FAST_SPLICE_RELEASE_US and fast_splice_(). Off by default: the splice path has a
  // limit-cycle history and this puts it back in the loop while unmuted.
  uint32_t fast_splice_threshold_us{0};
  // Cap on the correction of the INTER-DEVICE offset; 0 disables it (default).
  // The servo nulls each device against server time, so nothing nulls the difference between
  // two of them and a hard resync's residual persists forever. render_group_delta_us() measures
  // that difference -- measured 2026-08-27 across forced resyncs, it tracks the true displacement
  // to within a roughly constant 8.7 +- 3.4 us. Off by default because it is a second loop acting
  // on the same audio.
  uint32_t render_align_max_us{0};
  // TSF OBSERVER MODE: log the group's phase inputs (own phase, each peer's with its age, the
  // resulting delta). For a device that drives no DAC and exists to instrument the others.
  bool tsf_observer{false};
  // Force one accounting repair cycle after each session start. OFF by default: the effect is
  // measured (n=12) but its mechanism is not, so this is opt-in until a lone reconnect has been
  // graded with it. See reanchor_after_session_() and REANCHOR_BIAS_US.
  bool reanchor_after_reconnect{false};  // named for the config key; arms on any re-lock
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

/// @brief How much AUDIBLE DISRUPTION to accept in exchange for not going silent.
///
/// A hard resync is answered two ways: mute until the servo re-locks (silence, clean when it
/// returns) or correct audibly while playing (skips and inserted silence, but sound never stops).
/// The current default mutes once a storm is established, because a storm means many corrections
/// and each one is audible.
///
/// That is the right default and the wrong answer for some rooms. The measured trigger is a ~2 s
/// SUPPLY OUTAGE upstream of this firmware -- during which there is nothing to play anyway -- and
/// what follows is a re-lock the listener hears as a hole. A speaker carrying speech, or one whose
/// owner would rather hear a glitch than a gap, wants to ride it out. Hence a runtime setting: the
/// answer belongs to the room, not the build.
///
/// The levels are ordered by increasing tolerance of artifacts. NONE of them changes the FIRST
/// lock after a start or reconnect: before it there is no established timeline at all, so playing
/// is not "tolerating an artifact", it is emitting audio at an unknown offset -- which is the boot
/// warble this codebase has chased twice.
enum class SyncResilience : uint8_t {
  // Mute once a storm is established (RESYNC_STORM_COUNT resyncs in the window) or the error
  // passes the server's own buffer. The historical behaviour, and still the default: a storm is
  // many audible corrections in a row, and silence is usually the lesser artifact.
  MUTE_ON_STORM = 0,
  // Ride out storms; mute only when the error passes the server's buffer, where the DEADLINE is
  // implausible rather than the clock, and playing toward it is meaningless by definition. Trades
  // a run of audible skips for never going quiet during an ordinary supply outage.
  PLAY_THROUGH_STORMS = 1,
  // Never mute after the first lock, whatever the error. The most tolerant setting and the one
  // with real teeth: a wrong DEADLINE (a group-wide pause/resume was measured at ~2.1 s) will be
  // chased audibly rather than in silence. Choose it when a gap is worse than a mess.
  NEVER_MUTE = 2,
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

/// @brief TSF group-sync state, for diagnostics entities. There is no leader and no role: every
/// device publishes its own server<->TSF estimate and adopts the average of everyone's.
///   INACTIVE  feature off/unsupported (no wifi), no session, or no mapping held
///   SOLO      a mapping, but consensed from our own estimate alone -- nobody else is audible
///   CONSENSUS a mapping averaged over two or more devices, i.e. a genuinely shared timebase
enum class TsfState : uint8_t { INACTIVE, SOLO, CONSENSUS };

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

  /// @brief How long from now until audio handed over now would be rendered, if the sink can report
  /// it. Lets the playout accounting be anchored to the pipeline's MEASURED depth instead of an
  /// assumption about it; see the re-baseline paths in the player loop.
  ///
  /// A DURATION, because the chain crosses format boundaries -- a mixer may widen mono to stereo, a
  /// resampler changes the frame rate, the i2s slot width may be narrower than the stream -- so byte
  /// counts from different stages cannot be added. It is also LATENCY, not audio remaining: it
  /// includes the i2s DMA's silence padding, which holds none of our audio but still takes time to
  /// clock out, so it does not fall to zero as the queue empties.
  ///
  /// @return false when unavailable -- distinct from a reported zero.
  virtual bool on_query_latency(audio::AudioDepth & /*depth*/) { return false; }

  /// @brief How much of OUR OWN audio the sink still holds, as a duration. Excludes padding and
  /// anything we did not write, so this -- not on_query_latency() -- is what the accounting
  /// cross-check compares against `pushed - played`. Differencing against the latency instead
  /// yields the DMA's silence padding as a phantom split.
  ///
  /// The reading describes `depth.as_of_us`, which is in the past by up to one iteration of whichever
  /// task published it. Compare it against what `pushed - played` was AT THAT INSTANT, not against
  /// their current values -- see accounted_at_() and the drift column.
  /// @return false when unavailable -- distinct from a reported zero.
  virtual bool on_query_audio(audio::AudioDepth & /*depth*/) { return false; }

  /// @brief Attach an identity to the audio the NEXT on_audio_write() hands over, so the sink can say
  /// when THAT audio rendered rather than merely how much did.
  ///
  /// This is what makes render phase an observation instead of an inference. The inferred form,
  ///   (played_ts + tsf_offset) - (chunk_ts - (pushed - played)/rate),
  /// consumes our own frame ledger, and a device cannot detect that its own counter is biased by
  /// consulting that counter: once the servo repairs a ledger bias, the bias and the audio
  /// displacement it caused are equal and opposite inside that subtraction and cancel exactly.
  /// Measured 2026-08-28 against a known displacement -- inject_split(+1000 us) moved it by 0.003 of
  /// the truth, a re-baseline residual by 0.13, while an externally planted latency step moved it by
  /// 1.000. No gain or filter setting fixes an identity problem with arithmetic on quantities.
  ///
  /// @param tag Identity of the first frame of the next payload. See audio::RenderTag.
  virtual void on_set_render_tag(const audio::RenderTag & /*tag*/) {}

  /// @brief Whether tags actually survive to the DAC and come back. False through a resampler, and
  /// false while a mixer is blending a second source, both of which change or destroy the identity.
  /// Changes at runtime -- an announcement starting is exactly such a change -- so it must be read
  /// each time, never cached.
  virtual bool on_supports_render_tags() const { return false; }
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
  /// @brief Whether the player is currently routing audio rather than discarding it at the
  /// deadline. The re-arm watchdog keys on this instead of on the media source's state enum:
  /// "not routing while chunks are arriving" is the condition that is actually wrong, and it
  /// stays true however the source got there.
  bool output_active() const { return this->output_active_.load(std::memory_order_relaxed); }

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

  /// @brief How much audible disruption to accept rather than mute; see SyncResilience.
  /// Read by the player task at the mute decision, so a change takes effect on the next
  /// excursion and never mid-recovery.
  void set_sync_resilience(SyncResilience level) {
    this->sync_resilience_.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
  }
  SyncResilience sync_resilience() const {
    return static_cast<SyncResilience>(this->sync_resilience_.load(std::memory_order_relaxed));
  }

  /// @brief Reports a local volume/mute change to the server via a ClientInfo message.
  void send_client_info(uint8_t volume_percent, bool muted);

  /// @brief Sets this client's server-side latency via the control API (JSON-RPC,
  /// port 1705). The server persists it and pushes the updated ServerSettings back.
  void set_server_latency(int32_t latency_ms);
  /// @brief Tell the client which snapcast stream it is playing, for TSF leadership scoping.
  void set_stream_identity(const std::string &stream_name);

  // --- Playback feedback ---

  /// @brief Feed DAC-write feedback from the speaker's audio output callback.
  /// THREAD CONTEXT: speaker task; internally synchronized.
  void notify_audio_played(uint32_t frames, int64_t timestamp_us);

  /// @brief Feed a TAGGED render: audio we identified on the way down, handed back with the instant
  /// it rendered. Fires alongside notify_audio_played(), never instead of it, and only for audio
  /// whose identity survived the whole path -- so it is silent through a resampler, while a mixer is
  /// blending an announcement, and for the silence and splices we insert ourselves.
  ///
  /// This is the ONLY input from which render phase can be measured rather than inferred. See
  /// SnapcastAudioListener::on_set_render_tag() for why the inferred form cannot work.
  ///
  /// THREAD CONTEXT: speaker task; internally synchronized.
  void notify_audio_played_tagged(uint32_t frames, int64_t adjusted_ts, const audio::RenderTag &tag);

  /// @brief TEST HOOK: stop handing audio downstream for `ms`, so the pipeline drains and starves
  /// exactly as it does under an upstream stall.
  ///
  /// Faithful rather than synthetic: it discards at the push point, which is the same thing the
  /// client already does to chunks whose deadline has passed, so the drain, the feedback clamp and
  /// the starvation latch all run through their real paths. Setting pipeline_starved_ directly would
  /// skip all three and prove nothing.
  ///
  /// Exists because the event under investigation is upstream and arrives group-wide, roughly every
  /// 6-30 minutes, which makes it useless to wait for -- and because the case that plants
  /// inter-device skew is ONE device starving while the other does not, which never happens by
  /// chance when the cause is shared. THREAD CONTEXT: any (atomic).
  void inject_starvation(uint32_t ms) {
    this->starve_until_us_.store(now_us_public() + static_cast<int64_t>(ms) * 1000, std::memory_order_relaxed);
  }
  /// @brief TEST HOOK: perturb ONLY the playout accounting, by `us` of audio, leaving the real
  /// audio untouched.
  ///
  /// Exists because the interesting event -- an accounted-vs-measured split that holds long enough
  /// for the self-repair to fire -- cannot be waited for (it arrives on its own schedule) and
  /// cannot be provoked with inject_starvation, which reproduces it only by wrecking everything
  /// else: an injected starvation puts the wire's fit floor at 162-1291 us, against 0.71 us when
  /// quiet, and the step being measured is a few hundred us. The measurement needs the split
  /// WITHOUT the chaos.
  ///
  /// This reproduces the real mechanism faithfully rather than simulating it: shifting `pushed`
  /// makes the prediction wrong by exactly that much, the servo immediately begins steering real
  /// audio against it, and DRIFT_REPAIR_HOLD_US later the repair reconciles the accounting while
  /// the audio stays where the servo put it. That is the sequence a natural split follows; the only
  /// difference is that the size is known, which is what makes a step-versus-split curve possible.
  ///
  /// RAMPED, not stepped, and that distinction is the whole experiment.
  ///
  /// A natural split develops over seconds. The servo's error is computed against the prediction
  /// `pushed` feeds, so a slowly-biased `pushed` simply holds the audio at a biased position while
  /// the error reads ~0 -- no disturbance, just a static offset -- and the only step is at the
  /// REPAIR, when `pushed` is corrected. That is the +491 us measured on a natural repair against a
  /// 0.71 us floor.
  ///
  /// Stepping `pushed` instead hands the servo a large INSTANTANEOUS error, which it corrects
  /// violently: an injected +10 ms produced a -3.9 ms offset excursion and left the fit floor at
  /// 822-1204 us, swamping the very step being measured. So the injection contributes two steps
  /// where nature contributes one, and the injection's is the bigger. No magnitude fixes this --
  /// DRIFT_REPAIR_US is 2000, so even a minimal 2 ms step disturbs by ~2 ms against a sub-us floor.
  ///
  /// So the request is a TARGET, applied a little per chunk at SPLIT_RAMP_US_PER_S -- slow enough
  /// that the servo tracks it as ordinary drift, which is what makes the audio arrive at the biased
  /// position smoothly and leaves the repair as the only step on the wire. THREAD CONTEXT: any
  /// (atomic); the ramp itself is player-task-only.
  void inject_split(int32_t us) { this->inject_split_us_.store(us, std::memory_order_relaxed); }
  static int64_t now_us_public();

  // --- Diagnostics (main loop) ---

  bool is_connected() const { return this->connected_.load(std::memory_order_relaxed); }
  /// @brief Current TSF group-sync state (atomic read; INACTIVE when unavailable).
  TsfState get_tsf_state() const {
#ifdef CLOCK_SYNC_TSF_ACTIVE
    if (this->tsf_sync_ != nullptr) {
      const uint8_t n = this->tsf_sync_->consensus_n();
      if (n >= 2) {
        return TsfState::CONSENSUS;
      }
      if (n == 1) {
        return TsfState::SOLO;
      }
    }
#endif
    return TsfState::INACTIVE;
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

  /// @brief One chunk of the resync trace's PRE-TRIGGER history.
  ///
  /// Deliberately 16 bytes: 80 of these is 1.3 KB, which is affordable as a permanent member
  /// but would not be on the player task's 6 KB stack. dt_us is the gap from the previous
  /// recorded chunk -- the cadence of arrivals is itself part of the question ("does the ring
  /// empty because supply stopped?"), and a delta fits where an absolute timestamp does not.
  struct PreSample {
    uint32_t dt_us;
    int32_t err_us;
    int32_t med_us;
    uint16_t ring_ms;
    uint16_t drops;
  };
  // Chunks of history kept before an arm, matched to the 80 the live burst covers after it, so
  // an episode reads as one continuous ~160-chunk record. ~2.1 s of lead-in at the 26 ms
  // cadence, and much less during a storm -- which is the case the lead-in is for.
  static constexpr uint16_t RESYNC_PRE_CHUNKS = 80;
  // Samples packed into each dumped line. The dump is paced at one line per chunk, so this is
  // the ratio between the history's log cost and the live burst's: at 6, replaying 80 chunks of
  // history adds ~25% to a burst that already runs at the documented flood rate. Sized so the
  // formatted line stays clear of the 256-byte ceiling the Sync line hit.
  static constexpr uint16_t RESYNC_PRE_PER_LINE = 6;

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
    // Last trim handed to the hardware, and the point the slew limiter ramps from. Part
    // of the control path, not diagnostics -- it must exist in every build, and it must
    // track what is actually programmed (including the nominal-rate fallback) or the
    // limiter ramps away from a value the clock is not running at.
    float trim_applied_ppm{0.0f};
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
    /// @brief Why the steering gate refused, sampled on the LAST chunk that it refused.
    ///
    /// Recorded rather than inferred, after three plausible explanations for a frozen clock were
    /// each falsified by the log: rate_lock_ok never went false, the reported error sat an order of
    /// magnitude inside converge_fine, and the split-pending hold never fired. The gate tests a
    /// PER-CHUNK median error while the report prints a per-report summary, so the number that
    /// actually decided this was never in the log at all.
    bool gate_seen{false};
    bool gate_rate_lock_ok{false};
    bool gate_converged{false};
    int32_t gate_median_err_us{0};
    /// @brief The most recent chunk's target and the server time it belongs to.
    ///
    /// Kept so the SHADOW error can be evaluated at the report without recomputing a deadline --
    /// and without CALLING chunk_deadline_us_() again, which has side effects. The deadline is
    /// linear in server time for a fixed buffer and clock offset, so
    /// deadline(T) == last_deadline_us + (T - last_deadline_server_ts) exactly, for any T inside
    /// the same report. That identity is what makes the shadow free rather than a second model.
    int64_t last_deadline_us{0};
    int64_t last_deadline_server_ts{0};
    uint32_t trim_railed{0};
    // TIME-INTEGRAL of the applied trim over the report window, its audio time, and the
    // audio time actually covered by a programmed trim.
    //
    // The wire offset between two devices is the integral of their differential achieved
    // rate and nothing else (slope -1.0, sub-us residual, 99-100% explained by the
    // analyser's own rate columns). The trim is this device's best on-board proxy for that
    // rate, but the END-OF-WINDOW SNAPSHOT above cannot deliver it: one sample per 3.3 s of
    // a continuously moving quantity is an ALIAS, and integrating those snapshots explained
    // only 13-19% of the measured offset. The window's time-MEAN is the quantity that has a
    // chance, and the integral needs the mean and the DURATION together, which is why the
    // time is published beside it rather than assumed to be one report interval.
    //
    // Accumulated in EVERY build, not just under timing diagnostics: the snapshot's
    // invisibility in a plain build is the same failure the note at the report site
    // describes, where "(idle)" was printed for a loop steering by tens of ppm. Accumulated
    // once per chunk AFTER the servo has run, on a path every servo branch reaches, so the
    // hard-resync and catch-up branches -- which leave the previously programmed trim on the
    // hardware and never enter the PI -- contribute their audio time instead of silently
    // dropping out of the integral.
    //
    // covered < window means part of the window had no trim programmed at all (rate lock
    // unavailable); the report prints the ratio rather than folding the hole into the mean,
    // because a mean over an unknown fraction of the window is exactly the kind of number
    // that reads as data and is not.
    double trim_integral_ppm_s{0.0};
    float trim_covered_s{0.0f};
    float trim_window_s{0.0f};
    // Time throttle for the line above, independent of the 128-chunk report -- that boundary
    // also gates the accounting-split repair, so it must not be moved for a diagnostic.
    int64_t trim_log_us{0};
#endif
    // Per-chunk resync trace: bounded burst, armed by an excursion and rate-limited so a long
    // storm cannot re-arm indefinitely. drops counts discards within the current episode, which
    // is what makes the error's response to discarding readable line by line.
    uint16_t resync_trace_left{0};
    uint16_t resync_trace_idx{0};
    int64_t resync_trace_arm_us{0};
    uint32_t resync_drops{0};
    // FAST POSITION CORRECTION. splice_hist is a ring of the frames spliced on each recent chunk:
    // a splice changes the rendered audio immediately but reaches the error SIGNAL only after its
    // measurement lag, so without subtracting what is already in flight the loop would keep
    // correcting an error it has already fixed and overshoot -- which is the limit cycle this path
    // is on record for.
    //
    // The lag is a property of the SIGNAL, so the window over this ring is a parameter, not the
    // ring size: on the demoted prediction it is half the median window (15 chunks, the historical
    // value); on err_tag it is one pipeline depth (a splice is invisible until the spliced audio
    // renders) plus half the averaging block, derived per episode from the measured depth.
    // Inheriting 15 for both was right only by coincidence -- ~870 us/s of correction against a
    // ~300 ms blind spot hides ~260 us, most of the 300 us release band.
    static constexpr size_t SPLICE_HIST = 32;
    int8_t splice_hist[SPLICE_HIST]{};
    size_t splice_hist_idx{0};
    // First instant the standing error was seen above the engage threshold, so a TRANSIENT cannot
    // arm position correction. See FAST_SPLICE_PERSIST_US. 0 = not currently above it.
    int64_t fast_splice_seen_us{0};
    bool fast_splice_active{false};
    uint32_t fast_splice_frames{0};  // frames spliced in the current episode, for the log
    // How long the record queue has been empty; see the no-record branch in player_task_.
    int64_t no_record_since_us{0};
    // r_push VALIDITY, counted per report window. r_push = pushed - src_received is the term a
    // frames-based pivot would rest on, and measured over one session it is OUT OF RANGE 31-35% of
    // the time -- stably so (-55817720 frames, i.e. -1265 s, repeating), which is the dangerous
    // kind: a counter pair that does not share an origin after a pipeline restart, reading like
    // data. Counted so the fraction is visible BEFORE anything is built on it.
    uint32_t rpush_samples{0};
    uint32_t rpush_bad{0};      // raw pushed - src_received out of range
    uint32_t rpush_bad_reb{0};  // and after re-basing, which is the fix being verified
    int64_t rpush_log_us{0};
    // EPOCH ORIGIN for r_push. Our pushed counter restarts at zero on a pipeline restart while the
    // mixer's src_received keeps counting from its own start, so their raw difference is
    // meaningless until the mixer happens to restart too -- measured as stable garbage for minutes
    // to hours (05:47 -> 07:57 in one case). Differencing both against their values at the first
    // sample of the epoch gives them a common origin, which is all conservation needs: the
    // quantity that must hold still is the CHANGE in what is in flight, not its absolute value.
    int64_t rpush_base_pushed{0};
    int64_t rpush_base_src{0};
    bool rpush_base_valid{false};
    uint32_t rpush_epoch{0};
    // ACHIEVED RATE against server time: incremental least-squares of played_frames_total_ on
    // server time, in non-overlapping windows. See accumulate_achieved_rate_().
    //
    // Least squares over the WHOLE window, never a two-endpoint baseline: credit-adjacent
    // timestamps carry ~300 us of jitter, so 30 s of baseline resolves only ~+-10 ppm, and the
    // spec this has to hit is 0.04 ppm (the offset is the integral of the rate, so a constant
    // error of e ppm costs e us per second of run).
    ///
    /// Two fits are kept, on the SAME samples: one against server time (the quantity wanted) and
    /// one against local time (the control). The first windows read +100 ppm on BOTH boards, which
    /// cannot be a true achieved rate -- a locked device renders 44100 frames per second of server
    /// time or its buffer drains, and 100 ppm is 3 ms per window. A common-mode bias like that is
    /// either the local->server mapping or the frame counter itself, and the local fit separates
    /// them in one window: ~ the programmed trim means the machinery is sound and the mapping is
    /// at fault; ~ +100 ppm too means the counter grows faster than real time (padding, say),
    /// which is a different and larger finding.
    struct RateFit {
      double n{0.0}, sx{0.0}, sy{0.0}, sxx{0.0}, sxy{0.0}, syy{0.0};
      void reset() { this->n = this->sx = this->sy = this->sxx = this->sxy = this->syy = 0.0; }
      void add(double x, double y) {
        this->n += 1.0;
        this->sx += x;
        this->sy += y;
        this->sxx += x * x;
        this->sxy += x * y;
        this->syy += y * y;
      }
      /// @return frames per microsecond, with @p sd_frames set to the fit's residual sd.
      double slope(double &sd_frames) const {
        sd_frames = 0.0;
        const double sxx_c = this->sxx - this->sx * this->sx / this->n;
        const double sxy_c = this->sxy - this->sx * this->sy / this->n;
        const double syy_c = this->syy - this->sy * this->sy / this->n;
        if (!(sxx_c > 0.0) || this->n < 3.0) {
          return 0.0;
        }
        const double m = sxy_c / sxx_c;
        // RESIDUAL sum of squares, not a covariance term -- the first version printed the latter
        // and read 3e15, which says nothing about whether the fit found a rate or fitted jitter.
        const double rss = syy_c - m * sxy_c;
        sd_frames = std::sqrt(std::max(rss, 0.0) / (this->n - 2.0));
        return m;
      }
    };
    RateFit rate_server;
    RateFit rate_local;
    int64_t rate_x0{0};        // first sample's server time, subtracted to keep the sums conditioned
    int64_t rate_x0_local{0};  // and its local time, for the control fit
    int64_t rate_y0{0};  // first sample's frame count, same reason
    int64_t rate_last_fb_ts{0};  // feedback timestamp already accumulated; skips duplicates
    int64_t rate_window_start_us{0};
    uint32_t rate_epoch{0};  // playout epoch the fit belongs to; a seed invalidates it

    // Last computed median of the accounting split, carried so the UNMUTE GATE can see it: the
    // median is computed further down the loop than the gate, and one chunk of staleness (26 ms)
    // is nothing against a window that spans ~3.3 s.
    int32_t drift_med_last_us{INT32_MIN};
    // When the median-error gate first came good while the anchor test was still failing, so the
    // wait can be bounded. 0 = not waiting.
    int64_t unmute_anchor_wait_us{0};
    // Instant of the last accounting repair. A repair STEPS the prediction, so the median error
    // jumps by the size of the split with no audio having moved -- see FAST_SPLICE_REPAIR_HOLDOFF_US.
    int64_t last_repair_us{0};

    // Re-anchor after a RE-LOCK: armed by whatever dropped convergence (a session start, a mute,
    // a starvation re-baseline), fired once convergence returns and settles. See
    // reanchor_after_relock_().
    uint32_t reanchor_epoch{0};
    bool reanchor_armed{false};
    int64_t reanchor_due_us{0};
    int64_t reanchor_last_us{0};
    // Gain schedule: the instant of the last DISTURBANCE EVENT, from which KP decays ACQUIRE ->
    // RUN. Not the instant of the last error -- see TRIM_KP_DECAY_TAU_S for why that distinction
    // is the whole safety argument. Set to the player task's start so a boot counts as an event.
    int64_t kp_event_us{0};
    // Instant of the last event that was LOGGED, so a resync storm re-arms the schedule on every
    // chunk (which is correct) without logging on every chunk (which is not).
    int64_t kp_event_log_us{0};
    // What the schedule last handed the PI, for the report. Carried rather than recomputed because
    // the report runs on a different chunk from the PI and would otherwise print a gain that was
    // never applied.
    float kp_active{0.0f};
#ifdef CLOCK_SYNC_TSF_ACTIVE
    // Last observed TSF timebase epoch, for detecting a re-anchor: the timebase the servo measures
    // itself against actually stepped, so whatever it had converged to is now referenced to a
    // different clock. Under leader election this was the ROLE, and it fired on every handover --
    // six in seventeen minutes. Consensus makes joins and departures slew instead, so this fires
    // only on a real discontinuity. UINT32_MAX = not yet observed, so the first chunk records the
    // epoch rather than reporting a change that never happened.
    uint32_t kp_last_epoch{UINT32_MAX};
#endif
    // PRE-TRIGGER history cursors for that trace. The burst above is armed BY the threshold
    // crossing, so its first line already shows the ring empty and it structurally cannot show
    // the emptying. These carry the rolling window of the chunks BEFORE the arm; the samples
    // themselves live in pre_trace_ on the client object, because this struct is the player
    // task's stack and that task has 6 KB.
    //
    // pre_last_t_us is the timestamp of the newest recorded sample: samples store only the
    // DELTA from their predecessor (which is the arrival-cadence data the drain question wants,
    // and fits in 4 bytes), so absolute time is reconstructed backwards from this at dump.
    uint16_t pre_idx{0};     // write cursor into pre_trace_
    uint16_t pre_filled{0};  // samples valid, <= RESYNC_PRE_CHUNKS
    int64_t pre_last_t_us{0};
    // Dump in progress: paced at one packed line per chunk so the history costs a fraction of
    // the live burst's line rate rather than doubling it. Recording is frozen while it runs, so
    // the dump cannot read entries it is racing against; the frozen span is exactly the span the
    // live burst is covering anyway.
    uint16_t pre_dump_left{0};   // samples still to emit
    uint16_t pre_dump_pos{0};    // ring index of the next sample to emit
    uint16_t pre_dump_label{0};  // printed as -pre_dump_label, so the newest sample is -1
    int64_t pre_dump_t_us{0};    // reconstructed absolute time of that sample
#ifdef USE_I2S_RATE_LOCK
#endif
    uint32_t raw_sample_countdown{1};
    // Smoothed accounted-vs-observed disagreement (us); 0 when the accounting is honest
    float fill_corr_us{0.0f};
    bool fill_corr_valid{false};
    uint32_t fill_sample_countdown{0};
    // Mute-until-synced: real audio flows only after a full window of in-band medians
    bool converged{false};
    // Frames of DMA SILENCE PADDING baked into the starvation re-baseline's seed, owed back once
    // that padding drains. The seed is deliberately padding-inclusive because the prediction
    // extrapolates "frame N renders at frame M's time plus (N-M)/rate", which is exact only while
    // our frames render contiguously -- padding inserts gaps and would make it early by exactly the
    // padding. But `pushed - played` is an OWN-AUDIO count, so the same padding leaves it
    // permanently over-stated: measured as a standing +70 ms after an injected starvation, which the
    // self-repair then walked back audibly ten seconds later, after unmute. That walk-back is the
    // warble heard on every boot. Repaying the debt as soon as the padding actually drains -- while
    // the device is still muted and re-locking -- makes the same correction inaudible.
    int64_t padding_debt_frames{0};
    /// @brief Local instant at which the padding the seed was given has certainly drained: the seed
    /// instant plus the whole span that was resident then. The silence sits BEHIND the real audio
    /// inside each DMA descriptor, so waiting for the span is what guarantees it, and the DAC plays
    /// at real time so the deadline needs no query to confirm. 0 = no debt outstanding.
    int64_t padding_repay_at_us{0};
    // Running mean of the accounted queue over the report window, for the TSF group cross-check.
    // The instantaneous depth is useless there: it sawtooths by a chunk as the source ring fills in
    // 26 ms steps and drains continuously, so two devices sampled out of phase differ by up to
    // +-50 ms of pure phase. Measured across a pair whose depths agreed to 0.3 ms on average, the
    // instantaneous difference spanned 98 ms. That noise floor is why the divergence alarm sits at
    // 100 ms and cannot see the millisecond-scale offsets that actually matter.
    // Drift sampled ACROSS the window rather than once at its end. fill_drift_us is one
    // snapshot pair per report, and the quantity sawtooths -- with a mixer in the chain it
    // swings ~0 to -100 ms as a source ring fills and drains -- so a single sample is an
    // arbitrary point on that wave. That is the same aliasing that made the published depth
    // useless until it became a window mean, and it is why the repair needs a 10 s hold to
    // see through it. Accumulate min/max/mean so the wave can be characterised before
    // deciding whether the mean is a fit input for the repair.
    // Timebase contribution to the error, isolated. The median error is predicted - deadline, and
    // a +-500 us COMMON-MODE swing (both boards together) is what drives the differential residue
    // that lands on the wire. Which of the two terms moves decides the fix entirely: a wandering
    // shared offset needs smoothing in clock_sync, a wandering prediction needs the feedback pivot.
    //
    // deadline = server_ts + buffer - shared_offset, and buffer is constant, so (deadline -
    // server_ts) isolates the timebase term exactly. Tracked as a spread across the report window:
    // steady means the timebase is innocent and the pivot is the source.
    // DE-TRENDED. The raw span of (deadline - server_ts) is dominated by the local-versus-server
    // crystal drift -- measured -48..-52 ppm, so ~165 us across a 3.3 s report -- which buries the
    // noise riding on it. It read ~170 us whether the offset filter smoothed 8x or 16x, and so
    // failed to detect a change the wire showed as a halving of the skew floor.
    //
    // Tracking the spread of CONSECUTIVE DIFFERENCES de-trends by construction: a pure ramp has
    // constant differences and therefore ~zero spread, while a glitch appears at its full size.
    // Cheap -- one subtraction per sample, no fit, no history.
    int64_t dl_off_prev_us{0};
    int64_t dl_step_min_us{0};
    int64_t dl_step_max_us{0};
    bool dl_off_valid{false};
    bool dl_step_valid{false};
    // A MEDIAN, not a mean. The wave this was built to average through turned out not to be a
    // wave: measured, 31 samples of a window read ~0 and one reads a fixed large negative spike,
    // so the means came out quantised in exact multiples of spike/32 (-1320, -2640, -3960 for
    // one, two and three spikes). Averaging carries the artefact straight into the answer; a
    // median discards it outright and reads the true split. Kept alongside min/max/mean so the
    // spike stays visible rather than merely rejected -- it is a fixed-size recurring artefact
    // worth explaining, not just filtering.
    static constexpr size_t DRIFT_WINDOW = 33;
    int32_t drift_window_us[DRIFT_WINDOW]{};
    size_t drift_window_idx{0};
    int64_t drift_accum_us{0};
    uint32_t drift_samples{0};
    int32_t drift_min_us{0};
    int32_t drift_max_us{0};
    uint32_t drift_sample_countdown{1};
    int64_t depth_accum_frames{0};
    uint32_t depth_samples{0};
    uint32_t in_band_chunks{0};
    int64_t last_resync_log_us{0};
    // When the stream first went staler than the server's buffer, 0 while it is not
    int64_t stale_since_us{0};
    // Rolling hard-resync count, for telling a one-shot catch-up from a storm
    int64_t storm_window_us{0};
    uint32_t storm_resyncs{0};
    // When the drift first exceeded the repair threshold, 0 while it has not, plus the
    // range it has covered since -- a split is steady, an artefact moves.
    int32_t drift_excess_min_us{0};
    int32_t drift_excess_max_us{0};
    int64_t drift_excess_since_us{0};
    /// @brief DELAY LOOP state: the PI that steers the I2S rate on the MEASURED tag error
    /// (err_tag), with the ledger nowhere in its loop. See delay_loop_update_.
    ///
    /// dl_active latches once the loop has seeded its integral from the trim applied at handoff;
    /// it drops on tag loss (the trim is then HELD, not zeroed) and re-seeds on the next fresh
    /// block. dl_err_us is the last completed block mean, consumed by fast_splice_ so position
    /// correction runs on the measured error too -- while it is fresh, a ledger bias cannot move
    /// audio through the splice path, which is the property the inject_split test grades.
    bool dl_active{false};
    bool dl_have_err{false};
    int64_t dl_err_us{0};
    /// Local time dl_err_us was computed; stale (> DL_ERR_STALE_US) hands fast_splice_ back to
    /// the demoted prediction, where the split-pending guard applies again.
    int64_t dl_err_at_us{0};
    /// Gain the last block was conditioned under, for bumpless transfer across the decay schedule.
    /// 0 = never conditioned.
    float dl_kp_last{0.0f};
    /// Last integral value persisted to NVS, and when -- the crystal offset survives boots so a
    /// cold start engages at the learned rate instead of re-learning over ~90 s (iteration cost,
    /// and one fewer boot transient). Saved only on real change at a slow cadence: NVS writes
    /// block the player task briefly and wear flash.
    float dl_saved_integral_ppm{0.0f};
    int64_t dl_saved_at_us{0};
    /// When the loop last (re-)engaged; a save needs DL_PERSIST_SETTLE_US of continuous engagement
    /// first, or every boot overwrites the learned crystal with the first block's guess (measured:
    /// restored +3.30, re-saved +0.63 within a second of engaging).
    int64_t dl_engaged_since_us{0};
    /// Slow average of the integral (time constant DL_PERSIST_EMA_S); THIS is what gets persisted.
    /// The instantaneous integral swings tens of ppm with the common-mode timebase excursions
    /// (measured: +98.73 saved while the loop sat at +46 a minute later), so a settle gate alone
    /// still samples peaks. Seeded from the restored value, so a boot with no history is honest.
    float dl_integral_ema_ppm{0.0f};
    bool dl_integral_ema_valid{false};
    /// In-range HOLD (tag loss, mapping flap): the P term at hold entry and when it began. While
    /// holding, the programmed trim is integral + P*exp(-t/tau): a ~1 s mapping flap keeps P
    /// intact (holding the integral alone stepped one board by -P ~ -25 ppm while its peer kept
    /// applying P to the same common-mode error -- a differential step measured as ~130 us of
    /// skew per flap), while a minutes-long outage still ends at the crystal offset.
    float dl_hold_p_ppm{0.0f};
    int64_t dl_hold_since_us{0};
    /// AUTOTUNE accumulators: lag-1 autocorrelation of the block-error series over a 64-block
    /// (~21 s) window -- a decade slower than the loop, which is the timescale separation that
    /// keeps an adapter wrapped around a controller from becoming a second oscillator. Ringing
    /// (r1 strongly negative) slows tau; a standing mean with r1 near +1 speeds it, inside hard
    /// bounds. Player task only; reset whenever the toggle is off or the loop disengages.
    /// The window itself is stored (64 floats) so r1 can be computed properly -- demeaned, one
    /// normalisation -- rather than from running sums, which reported r1 = +1.07 on the first
    /// live window (mixed n / n-1 denominators plus a trending mean).
    static constexpr size_t AT_WINDOW = 64;
    float at_win[AT_WINDOW]{};
    uint32_t at_n{0};
    /// Completed PI updates this report window, so the report can tell "loop running" from
    /// "loop holding" -- the exact ambiguity the old "(idle)" trim snapshot had.
    uint32_t dl_updates{0};
    /// Newest accounted pipeline depth (pushed - played), sampled per chunk under playout_mutex_.
    /// Sizes the splice in-flight horizon; a ledger bias of even a few ms is a fraction of a
    /// chunk here, so this use survives ledger perturbation.
    int64_t pipe_depth_frames{0};
    int64_t dl_log_us{0};
    /// When a coarse correction (hard resync / aggressive catch-up) last acted on the MEASURED
    /// error; another tag-driven one waits until the tags post-date it by a blank interval.
    int64_t coarse_act_us{0};
    /// The measured error that correction acted on, so the first block that post-dates it can be
    /// judged: did the correction move the measurement it was based on? 0 = nothing to judge.
    int64_t coarse_act_err_us{0};
    /// Consecutive tag-driven corrections that left |err_tag| essentially where it was. Three
    /// declare the tag path faulted (see tag_fault_until_us). A measurement that corrections cannot
    /// move is not measuring the audio: 2026-08-29 12:38-13:17, B hard-resynced every ~20 s on a
    /// constant +97 ms err_tag and A's splice gave up on -19.5 ms over and over, for 40 minutes,
    /// while SHADOW showed the tag and ledger errors apart by exactly the RECON drift.
    uint8_t tag_miss{0};
    /// While now < this, err_tag is not trusted: coarse decisions, the measured-error splice and
    /// the split-repair disarm all fall back to the ledger, as if tags were stale.
    int64_t tag_fault_until_us{0};
    /// Consecutive TAGFAULTs with no split repair between them; two escalate to a reconnect.
    uint8_t tag_fault_streak{0};
    uint8_t tag_agree_streak{0};        // consecutive blocks of tag/ledger agreement while tags are distrusted
    int64_t tag_agree_block_us{0};      // block the agreement streak last counted (one vote per block)
    /// RESYNC WINDOW: until this instant the fast splice arms at resync_splice_us with no persistence
    /// wait. Set at engage, at every mark_kp_event_ (hard resync, re-anchor, split repair) and at a
    /// reconnect. Measured 2026-08-29 without it: A's 21:05 reconnect took 25 s to |wire| < 100 us
    /// (from +204 us at PI pace, below the 1 ms splice threshold); B's 22:13 boot > 75 s from +490.
    int64_t post_event_until_us{0};
    int64_t resync_inside_since_us{0};  // when |err| last went inside the arm threshold (window close timer)
    int64_t resync_last_block_us{0};    // dl_err_at_us of the block the last in-window step used (one block, one step)
    int64_t rskip_log_at_us{0};         // block the last RSKIP line described (one line per block)
    int64_t resync_step_at_us{0};       // last in-window position step of ANY source (ledger steps wait out the blank too)
    int64_t phase_transient_until_us{0};  // my render phase does not describe my audio until then (steps, hard resyncs, deadline source changes)
    int64_t ledger_prev_err_us{0};      // previous chunk's ledger error (stability test for the first window step)
    uint8_t ledger_stable_streak{0};    // consecutive chunks with a consistent ledger reading
    float align_kick_us{0.0f};  // render_align bias change not yet delivered as position (ALIGN KICK)
    /// In-window position steps still on their way to the DAC. A drop is applied at PUSH time and the
    /// ring holds ~1.7 s of audio ahead of the DAC, so a step is invisible to the tags for ring depth
    /// + pipeline; the coarse target subtracts what is pending instead of waiting for it.
    static constexpr size_t WIN_STEPS = 8;
    int64_t win_step_us[WIN_STEPS]{};
    int64_t win_step_at_us[WIN_STEPS]{};
    int64_t win_step_land_frame[WIN_STEPS]{};  // pushed_frames_total_ the step was applied at (landed when played passes it)
    size_t win_step_idx{0};
    /// No NVS integral was restored at boot: seed from the TSF crystal estimate at first engage
    /// and run the fast boot Ti; a fresh board otherwise winds ~56 ppm through Ki over 10+ min.
    bool dl_cold_start{true};
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

  /// @brief Correction for the inter-device offset the servo cannot see.
  ///
  /// The per-device servo nulls its OWN error against server time, so nothing nulls the
  /// difference between two devices; a hard resync leaves a residual that then persists
  /// forever. render_group_delta_us() measures that difference against the group average.
  /// Measured 2026-08-27 across forced resyncs, it tracks the true displacement with a roughly
  /// constant 8.7 +- 3.4 us residual, which is what makes closing the loop defensible.
  ///
  /// Applied to the DEADLINE rather than by splicing audio: shifting the target lets the
  /// existing servo do the work, instead of a second controller fighting it for the same
  /// frames.
  std::atomic<int32_t> render_bias_us_{0};
  /// Report counter for rate-limiting the alignment correction; see RENDER_ALIGN_EVERY_N_REPORTS.
  uint32_t render_align_tick_{0};
  float render_align_frac_{0.0f};  // sub-microsecond remainder of align steps (see RALIGN)
  std::atomic<float> bias_kick_request_us_{0.0f};  // bench hook: bias change to deliver as a kick (see align_bias_kick_us)
  std::atomic<int64_t> pipe_depth_us_{0};   // pushed-minus-played, us; mirrored per block for travel_horizon_us_()
  std::atomic<int64_t> ring_depth_us_{0};   // pcm ring fill, us; mirrored per chunk for travel_horizon_us_()
  std::atomic<int64_t> write_begin_us_{0};  // last on_audio_write() entry (player task); see the fill-drift comparison
  std::atomic<int64_t> write_end_us_{0};    // its return; end < begin while a write is in progress
  // Ring of the last write windows: the depth snapshot can be 50-60 ms old and fall inside a window
  // that had already ENDED by the time the comparison ran, which the single begin/end pair missed
  // (17 % of comparable reports still carried the two-chunk artefact, 2026-08-30 16:38).
  static constexpr size_t WRITE_WIN_RING = 8;
  int64_t write_win_begin_[WRITE_WIN_RING]{};
  int64_t write_win_end_[WRITE_WIN_RING]{};
  size_t write_win_idx_{0};
  /// Reads @p bytes from the PCM ring and discards them.
  void discard_ring_bytes_(size_t bytes);

  /// Accumulates one sample into the achieved-rate fit and reports a completed window.
  /// THREAD CONTEXT: player task.
  void accumulate_achieved_rate_(ServoState &st, const ChunkRecord &rec);

  /// Fast POSITION correction while converged: one frame per chunk against a standing offset the
  /// rate loop would take too long to integrate away. Returns the frames to splice this chunk
  /// (>0 drop, <0 insert, 0 none). See fast_splice_threshold_us.
  ///
  /// @p err_us is whichever error signal is live -- the measured tag error when fresh, the demoted
  /// prediction otherwise. @p hold gates the whole path (the split-pending guard, applied by the
  /// caller ONLY on the prediction: a ledger bias is invisible to err_tag, so guarding it there
  /// would disarm the mechanism against exactly the errors it can safely correct).
  /// @p horizon_chunks is the signal's measurement lag in chunks -- how far back a splice is still
  /// invisible to @p err_us -- over which in-flight splices are subtracted before the threshold
  /// test. @p measured lifts the converged gate: a measured error cannot fight the muted coarse
  /// convergence (which acts on the prediction), and gating it left an unconverged 1-2 ms dead
  /// zone nothing corrected.
  int32_t fast_splice_(ServoState &st, int64_t err_us, uint32_t sample_rate, bool hold,
                       uint32_t horizon_chunks, bool measured);

#ifdef USE_I2S_RATE_LOCK
  /// @brief THE DELAY LOOP: PI rate steering on the measured tag error, the ledger nowhere in it.
  ///
  /// Pulls a completed block of DL_BLOCK_N tagged arrivals (accumulated on the speaker callback in
  /// notify_audio_played_tagged), runs one PI update, and leaves the demand in st.trim_applied_ppm
  /// for the caller to program. Between blocks, and through tag loss, the demand is simply not
  /// changed -- the loop HOLDS ITS LAST TRIM, never its last error. Preconditions for an update:
  /// tags fresh, and the deadline on the SHARED TSF mapping (on the local-Kalman fallback the
  /// clock-offset wander is per-device, so steering on it misaligns the group; hold instead).
  /// THREAD CONTEXT: player task.
  void delay_loop_update_(ServoState &st);
  void publish_render_phase_(bool steady);  // per-block render phase to the group; UNKNOWN while in transient
  void publish_render_phase_sample_();      // measure + store one phase sample (no broadcast-flag change)
  int64_t travel_horizon_us_() const;  // ring + pipeline + two blocks: how long a position change takes to reach the tags
#endif

  /// Forces one repair cycle after a re-lock, if configured; a no-op otherwise. Called once per
  /// chunk from the player loop, after the convergence gate.
  void reanchor_after_relock_(ServoState &st);

  /// The PI's proportional gain for this chunk: ACQUIRE while unconverged, otherwise decaying
  /// ACQUIRE -> RUN with time since the last disturbance event. Open-loop in the error by
  /// construction; see TRIM_KP_DECAY_TAU_S.
  float trim_kp_(const ServoState &st) const;
  /// Re-arms that decay. @p why is logged, throttled, so a schedule change is never invisible.
  void mark_kp_event_(ServoState &st, const char *why);

  /// Appends one chunk to the resync trace's rolling pre-trigger history. Called on EVERY chunk
  /// the servo sees; a no-op while a dump is replaying. Costs five stores.
  void record_pre_trace_(ServoState &st, int64_t error_us, int64_t median_err_us, uint32_t ring_ms);
  /// Arms the replay of that history, oldest first, and reconstructs the oldest sample's
  /// absolute timestamp from the stored deltas.
  void arm_pre_trace_dump_(ServoState &st);
  /// Emits at most one packed RPRE line, if a dump is armed. Called once per chunk.
  void emit_pre_trace_line_(ServoState &st);

  /// One recorded value of a playout counter and the instant it took effect. See the histories
  /// themselves, below, for why the accounting has to be evaluable at a past instant at all.
  struct PlayoutMark {
    int64_t ts_us;  // 0 marks an unused slot; no real sample can land there
    int64_t total;  // the counter's value as of ts_us
  };
  /// Sized against the worst staleness seen in the field (~165 ms) and the fastest either counter
  /// moves. Pushes are the faster of the two: a chunk is 4608 bytes at 44.1 kHz stereo against a
  /// 4096-byte slice buffer, so two or three marks per chunk at ~38 chunks/s, and more when the
  /// pipeline back-pressures into partial writes. Playback credits arrive per DMA buffer, ~100/s.
  /// 64 slots covers ~600 ms of either at 2 KB across both rings; an older reading is refused
  /// rather than answered wrongly, so being generous here only costs RAM.
  static constexpr size_t PLAYOUT_HISTORY = 64;

  /// @brief Records a counter's new value against the instant it took effect. Call under
  /// playout_mutex_, from the thread that owns the counter.
  static void mark_playout_(PlayoutMark *history, size_t &next, int64_t ts_us, int64_t total);
  /// @brief The newest recorded level at or before `as_of_us`. False when the ring holds nothing that
  /// old, which is the caller's cue to discard the sample.
  static bool playout_level_at_(const PlayoutMark *history, int64_t as_of_us, int64_t &total);
  /// @brief Clears both histories. Call under playout_mutex_ whenever the counters are reset or
  /// re-baselined: a level recorded before a re-baseline says nothing about the one after it.
  void clear_playout_history_();
  /// TEMPORARY DIAGNOSTIC: see the definition.
  void dbg_early_recon_(const ChunkRecord &rec, const char *phase);
  /// @brief The accounted queue (`pushed - played`) as it stood at `as_of_us`, in frames.
  ///
  /// This is the whole point of the histories: it makes the accounting comparable to a sink reading
  /// that describes a moment already past. Call under playout_mutex_.
  ///
  /// @return false when `as_of_us` predates what the histories still hold, in which case the sample
  /// must be discarded rather than compared -- a wrong answer here manufactures the split it is
  /// meant to detect.
  bool accounted_at_(int64_t as_of_us, int64_t &frames) const;
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
  std::atomic<uint8_t> sync_resilience_{static_cast<uint8_t>(SyncResilience::MUTE_ON_STORM)};
  /// Bumped by the network task when a stream starts, so the player task can tell "this is a new
  /// session" from "the same session re-locked after an excursion" -- which the servo state alone
  /// cannot, since both clear st.converged.
  std::atomic<uint32_t> session_epoch_{0};
  /// Bumped whenever the playout accounting is re-baselined (a seed, or a fresh session), so any
  /// fit or integral over the counters can tell "my window straddles a discontinuity" from "my
  /// window is fine". r_push has been measured at -180 s of span across one of these.
  std::atomic<uint32_t> playout_epoch_{0};

  /// PLAYER-TASK WATCHDOG, reported by the MAIN LOOP.
  ///
  /// Every attempt so far to have the player task report its own stall has failed for the same
  /// reason: the counters reset on any progress, so a trickle keeps them quiet, and a task stuck
  /// in a loop that occasionally succeeds says nothing at all. Three wedges today ended with the
  /// player silent and no diagnostic firing.
  ///
  /// So the report comes from a thread that is provably alive -- the main loop, which keeps
  /// logging wifi_diag throughout every wedge. The player only has to stamp where it is; it does
  /// not have to be well enough to complain.
  enum class PlayerPhase : uint8_t {
    IDLE = 0,        // waiting on the record queue
    KEEPALIVE = 1,   // pushing silence to hold the pipeline
    RING_READ = 2,   // waiting for a popped record's PCM
    SERVO = 3,       // in the servo/deadline arithmetic
    WRITE = 4,       // writing audio downstream
    DISCARD = 5,     // draining a chunk it will not play
  };
  std::atomic<uint8_t> player_phase_{static_cast<uint8_t>(PlayerPhase::IDLE)};
  /// Bumped every time the player completes a chunk. The main loop watches it for movement, so a
  /// stall is visible even when the stuck loop is making partial progress forever.
  std::atomic<uint32_t> player_progress_{0};
  /// Bumped at the TOP of every player-loop iteration, whether or not the chunk is played.
  /// Separates "the task is blocked" from "the task is spinning through a path that never
  /// completes a chunk" -- opposite problems that look identical in the phase stamp, because every
  /// discard path returns to the top and re-stamps IDLE on its way past.
  std::atomic<uint32_t> player_iters_{0};
  uint32_t player_iters_seen_{0};
  uint32_t player_progress_seen_{0};
  int64_t player_progress_at_us_{0};
  int64_t player_stall_log_us_{0};
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
  /// Local time of the last byte received on the stream socket; 0 = fresh session. Network task only.
  int64_t last_rx_us_{0};

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
  /// @brief The most recent CAPTURED render observation: a descriptor's real audio, the instant it
  /// finished rendering, and the identity of its first real frame.
  ///
  /// Held under playout_mutex_ with the ledger it exists to replace, so the report path can take both
  /// in one lock and never mix an observation with counters from a different instant.
  struct TaggedRender {
    /// Local (esp_timer) time the descriptor's real audio FINISHED. 0 until the first observation.
    int64_t adjusted_ts_us{0};
    /// Real frames in that descriptor, i.e. how far back its FIRST real frame was.
    uint32_t frames{0};
    /// Server audio time of the frame the tag names, already advanced to the descriptor's first frame.
    int64_t server_ts_us{0};
    /// Frames from that server timestamp to the frame the tag names.
    uint32_t offset_frames{0};
    /// Sample rate the offsets are counted in, 0 when unknown -- a tag from a stream that has since
    /// changed rate must not be converted with the new one.
    uint32_t sample_rate{0};
  };
  TaggedRender tagged_render_{};
  /// Tagged renders seen since the last sync report, so the report can say whether the signal is live
  /// at all rather than leaving a silent fallback to the inferred phase looking like a measurement.
  uint32_t tagged_render_count_{0};
  /// @brief Running mean and spread of the TRANSPORT DELAY across the report window.
  ///
  /// The delay is (local render instant of a tagged frame) - (that frame's server time): a direct,
  /// captured measurement of the one quantity the whole system exists to control. It needs no
  /// ledger, and it does not care HOW the delay arises -- ring depth, DMA padding, mixer fill, a
  /// restart at an unobserved level are all just terms inside a black box whose output is measured.
  ///
  /// ACCUMULATED, not sampled. ~334 tagged renders arrive per 3.35 s report and the diagnostic was
  /// keeping only the newest, so a measurement available at ~100 Hz was being read at 0.3 Hz. A
  /// single observation carries ~70 us of sample-to-sample jitter (measured, both boards); if that
  /// jitter is independent, the window mean should land near 70/sqrt(334) ~ 4 us. Whether it
  /// actually does is the point of collecting this: if the spread does not shrink as sqrt(N), the
  /// 70 us is real phase movement rather than noise, and that is equally worth knowing.
  ///
  /// Welford, so the variance is stable without keeping the samples. Reset each report.
  /// Written on the speaker callback thread under playout_mutex_, read by the player task.
  /// @brief The deadline anchor, published for the SPEAKER CALLBACK thread.
  ///
  /// ServoState is a player-task local, so the tagged-render callback cannot read it. These two
  /// carry the same pair across the thread boundary under playout_mutex_, letting the callback
  /// evaluate the deadline-corrected error at the instant each tag arrives rather than once per
  /// report. deadline() is linear in server time for fixed buffer and offset, so
  /// deadline(T) == anchor_deadline + (T - anchor_server_ts).
  int64_t tag_anchor_deadline_us_{0};
  int64_t tag_anchor_server_ts_{0};

  /// @brief DELAY LOOP measurement accumulator: err_tag over the current control block.
  ///
  /// Separate from the diagnostic Welford below on purpose -- the diagnostics reset per report
  /// (3.35 s), the control block completes every DL_BLOCK_N arrivals (~320 ms), and sharing an
  /// accumulator between two consumers with different reset points is the conditional-reset bug
  /// this file already paid for once. Written on the speaker callback under playout_mutex_,
  /// drained by the player task in delay_loop_update_.
  uint32_t dl_acc_n_{0};
  double dl_acc_sum_us_{0.0};
  /// Local render instants of the block's first and last arrivals: their span is the PI's dt, and
  /// the last one is the freshness witness ("no arrival for DL_TAG_STALE_US" = tags lost).
  int64_t dl_acc_first_us_{0};
  int64_t dl_acc_last_us_{0};
  /// @brief Tag-stream invalidation: arrivals rendering before this local instant are discarded.
  ///
  /// A setpoint change (buffer_ms / server latency) re-anchors the deadline immediately, but the
  /// ~250 ms of audio already in flight was scheduled against the OLD deadline, so its tags would
  /// carry the step as a corrupted measurement -- counted twice, in opposite directions. The same
  /// applies to a hard resync and a timebase re-anchor. Blank for one pipeline depth and let the
  /// loop resume on genuinely post-change audio.
  int64_t dl_blank_until_us_{0};

  uint32_t delay_n_{0};
  double delay_mean_us_{0.0};
  double delay_m2_us_{0.0};
  /// @brief The same mean, computed over ODD and EVEN arrivals separately.
  ///
  /// sem = sd/sqrt(n) is only the true standard error if the samples are INDEPENDENT, and
  /// consecutive DMA descriptors share pipeline state, so they plausibly are not. That matters: an
  /// optimistic sem would make the delay look controllable when it is not.
  ///
  /// Report-to-report scatter cannot settle it, because the delay genuinely moves 35-222 us per
  /// report while the servo steers, which swamps a 3 us effect. Interleaving does: both halves span
  /// the SAME window, so any drift -- linear or not -- affects them equally and cancels in the
  /// difference. What is left is noise alone.
  ///
  ///   |mean_odd - mean_even| ~ 2*sem   -> independent, sem is honest
  ///   |mean_odd - mean_even| >> 2*sem  -> correlated, sem is optimistic by that ratio
  /// @brief Block-means variance sweep, for the EFFECTIVE sample size.
  ///
  /// Replaces an interleaved odd/even test that was mis-designed: interleaving cancels the
  /// correlated component it is trying to size, so it can detect correlation but never measure its
  /// cost. It also had its sense inverted in the reporting -- positive autocorrelation makes the two
  /// halves MORE alike, so the ratio falls below the independence expectation rather than rising
  /// above it. Measured 0.03-0.73 against ~0.8, i.e. correlated, by a factor that test could not
  /// give.
  ///
  /// This measures it directly. Accumulate the mean over blocks of B consecutive arrivals for
  /// several B, and track the variance of those block means. For INDEPENDENT samples the variance of
  /// a block mean falls as 1/B; for correlated samples it flattens, and where it stops falling is
  /// the effective sample size. The true standard error of the window mean is then sd/sqrt(n_eff),
  /// not sd/sqrt(n).
  ///
  /// Powers of two from 1 to 64: enough range to see the knee at the ~10 ms descriptor cadence
  /// against a 3.35 s window, and cheap -- one add and one compare per level per arrival.
  static constexpr size_t DELAY_BLOCK_LEVELS = 7;  // B = 1,2,4,8,16,32,64
  struct DelayBlock {
    uint32_t fill;      ///< arrivals accumulated into the block currently being built
    double sum;         ///< their sum
    uint32_t n;         ///< completed blocks this window
    double mean;        ///< running mean of completed block means (Welford)
    double m2;          ///< their M2
  };
  DelayBlock delay_blocks_[DELAY_BLOCK_LEVELS]{};
  /// Sample rate the tag offsets are counted in, published by the player task when it tags and read
  /// on the speaker callback. Atomic because those are different threads and this is the one term of
  /// a tagged observation that does not travel with the tag.
  std::atomic<uint32_t> tag_sample_rate_{0};

  std::atomic<bool> pipeline_starved_{false};
  bool starved_latched_{false};
  /// Until when a re-baseline's own aftermath is barred from re-arming the starvation latch.
  /// Written by the player task in rebaseline_after_starvation_, read on the speaker callback.
  std::atomic<int64_t> starve_suppress_until_us_{0};
  /// TEST HOOK, see inject_split(). REQUEST in us, consumed once by the player task; 0 when none.
  std::atomic<int32_t> inject_split_us_{0};
  /// @brief Remaining split still to be ramped in, us. Player task only. Signed: a negative target
  /// ramps down. Accumulates, so a second request while one is in flight adds to it rather than
  /// discarding it.
  int64_t split_ramp_remaining_us_{0};
  /// @brief Sub-frame carry for the ramp, us. The accounting moves in whole frames (22.7 us at
  /// 44.1 kHz), so a rate gentler than one frame per chunk -- 868 us/s -- can only be expressed by
  /// spending a frame every few chunks. Without this the per-chunk budget truncated to zero frames
  /// and the ramp never moved at all.
  int64_t split_ramp_carry_us_{0};
  /// TEST HOOK, see inject_starvation(). 0 when not injecting.
  std::atomic<int64_t> starve_until_us_{0};
  /// @brief ANCHOR ERROR measurement, armed by a re-baseline. Player task only.
  ///
  /// The seed asserts that the audio resident at that instant will take `latency` us to drain --
  /// a falsifiable prediction, and the one everything downstream depends on. If it is wrong by E,
  /// the accounting is off by E, the prediction built on it is off by E, and the servo steers the
  /// real audio E away from where it belongs while its own error reads ~0, because that error is
  /// measured against the very prediction E corrupts. That is why a planted offset is invisible
  /// here and needs an analyser.
  ///
  /// The playout FEEDBACK can answer it though, and it is ground truth: it comes from the speaker
  /// callback, not from `pushed`. When `played` reaches the frame count deemed in flight at the
  /// seed, that audio has actually drained. The elapsed time is the TRUE latency, so
  ///
  ///     E = (time for the resident audio to drain) - (latency the seed anchored to)
  ///
  /// with no prediction anywhere in it. 0 target = not armed.
  int64_t seed_drain_target_frames_{0};
  int64_t seed_drain_from_us_{0};
  int64_t seed_drain_latency_us_{0};
  int64_t seed_drain_prev_frames_{0};
  /// @brief Timestamp of the PREVIOUS playout-feedback report, so the crossing frame's render
  /// instant can be interpolated inside the batch that contains it rather than quantised to the
  /// batch boundary (~50 ms, against the tens of microseconds being measured).
  int64_t played_prev_ts_us_{0};
  // TEMPORARY DIAGNOSTIC: frames the clamp below has silently discarded. A PARTIAL clamp
  // (available_frames > 0 but under the credit) logs nothing and flags nothing, yet it shorts
  // `played` permanently -- which is the shape of the startup offset: DMA-buffer quantised,
  // varying per start. Remove once explained.
  // TEMPORARY DIAGNOSTIC: the startup offset is fully formed before the first sync report, so
  // per-report logging cannot watch it appear. This counts chunks from the first push so the same
  // one-instant reconciliation can run from the very beginning, at chunk resolution.
  uint32_t dbg_early_chunks_{0};
  /// @brief TEMPORARY DIAGNOSTIC: chunks of full-cadence reconciliation still to log after a
  /// re-baseline. The seed's damage is done inside ~150 ms, which the 3.3 s report cannot see and
  /// the every-other-chunk startup sampling would only catch half of; this runs every chunk for
  /// about two seconds so the seed instant and its immediate aftermath are both in the trace.
  /// Player task only.
  uint32_t dbg_seed_trace_left_{0};
  uint32_t dbg_seed_trace_idx_{0};
  /// @brief Set by either re-baseline site, consumed by the player task. The flush-path re-baseline
  /// runs on the speaker callback thread, so arming cannot be a plain write.
  std::atomic<bool> dbg_seed_trace_arm_{false};
  int64_t dbg_clamped_frames_{0};
  uint32_t dbg_clamp_events_{0};
  int64_t dbg_clamp_last_log_us_{0};

  // Recent history of the two playout counters, so the accounted queue can be evaluated at the
  // instant a sink reading describes rather than at the instant we read it.
  //
  // This exists because the two are not sampled together and cannot be. The sink publishes a
  // snapshot on ITS task's cadence; we read it on the player task at an arbitrary phase, and every
  // frame we pushed or that rendered in between lands in `pushed - played` but not in the reading.
  // Differencing the two directly measured that phase, not the accounting: on the fleet the residue
  // came out quantised in whole chunks (26.1 ms at 44.1 kHz), mean wandering tens of milliseconds
  // over hours, in opposite directions on two clients -- an order of magnitude above the ~7 us the
  // comparison is supposed to resolve, and shaped nothing like the steady offset it is looking for.
  //
  // Two separate rings rather than one of (pushed, played) pairs, because the two are stamped from
  // different clocks-in-spirit: a push is stamped when it happens, while a playback credit is
  // stamped with the DAC time the audio actually RENDERED, which is earlier than the callback that
  // reports it. Interleaving those into one sequence would not be monotone, and a level recorded
  // against the wrong instant is the error this is here to remove.
  //
  // A reading older than the ring is rejected rather than guessed at.
  PlayoutMark pushed_history_[PLAYOUT_HISTORY]{};
  PlayoutMark played_history_[PLAYOUT_HISTORY]{};
  size_t pushed_history_next_{0};
  size_t played_history_next_{0};

  // --- Network task locals ---
  int sock_{-1};
  int64_t last_scan_us_{0};  // last mDNS scan, rate-limits re-scans on reconnect
  std::string active_host_;  // host of the current session (config or mDNS result)
  // FNV-1a of the active session's "host:port" — identifies which server clock a
  // shared TSF mapping refers to
  uint32_t server_id_hash_{0};
  // FNV-1a of the snapcast STREAM name, 0 until the control session reports one. Scopes TSF
  // leadership: render_phase is expressed against the stream's server audio time, so it is only
  // comparable between devices on the SAME stream, and a delta taken across two of them is not
  // a playout offset at all. 0 means "unknown" and groups with anyone, which is the pre-existing
  // behaviour and what a client without control-port access falls back to.
  std::atomic<uint32_t> stream_id_hash_{0};
#ifdef CLOCK_SYNC_TSF_ACTIVE
  int64_t last_peer_refresh_us_{0};  // TSF unicast roster refresh (network task)
#endif
  uint16_t next_message_id_{0};
  bool stream_active_{false};
  bool emit_room_wait_logged_{false};  // emit_pcm_ has already reported this ring-room episode
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
  // speaker callback thread pokes invalidate_baseline() (atomic flag) and runs the
  // dither step tick() once per callback (see RateLock).
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
  /// Set by chunk_deadline_us_ when the deadline source toggles (shared mapping <-> local
  /// Kalman); consumed once per chunk by the player loop, which treats the toggle as a timebase
  /// re-anchor (gain re-arm + tag-stream blank) -- the deadline steps by the mappings'
  /// disagreement and nothing else reports it. Player task only, both sides.
  bool deadline_source_switched_{false};
  /// NVS slot for the delay loop's integral (the learned crystal offset). Player task only.
  ESPPreferenceObject dl_integral_pref_;
  /// Mirror of the ServoState EMA for the shutdown save (player task writes, main loop reads).
  std::atomic<float> dl_integral_ema_mirror_{0.0f};

  /// @brief RUNTIME-TUNABLE loop parameters, settable over the native API (servo_param action)
  /// so a tuning campaign needs no reflash -- every reflash costs five consensus membership
  /// changes and a boot transient. Written from the API (main loop task), read on the player
  /// task and speaker callback; atomics, defaults = the flashed constants. NOT persisted:
  /// a reboot returns to the flashed values, which keeps a bad experiment one power-cycle from
  /// gone. Every set is logged at WARN so the analyser's annotations carry it.
  std::atomic<float> tune_tau_s_{120.0f};  // floor; error-proportional gain stiffens it (DL_GAIN_KNEE_US)
  /// Integral time (s): Ki = Kp / Ti. Decoupled from tau -- Ti = tau (Ki = Kp^2) made the integral
  /// swing ~57 ppm p-p chasing the +-600 us / ~60 s common-mode wander (measured 21:08: +103->+114
  /// in 2 s), so a hold froze a wrong 'crystal offset'. With the integral restored from NVS the
  /// fast wind-up Ti = tau bought is moot; Ti belongs to the crystal's drift timescale (minutes).
  std::atomic<float> tune_ti_s_{600.0f};
  /// Error-proportional gain: Kp = (1/tau) * max(1, |err| / knee), effective tau floored at tau_min.
  /// Knee 25 us: tau 120 only inside the per-block noise, tau ~30 s at 100 us, tau_min beyond 120 us.
  // KNEE OFF (1e6 us): the error-proportional boost is a per-board gain, and 22:13-22:59 one board
  // was above knee 150 while the other was below 21 % of the time (median asymmetry 0.004 ppm/us =
  // 0.4 ppm of differential trim per 100 us of common wander). The rate gain must be the same
  // function of the error on every board; acquisition is the coarse path's job (position).
  std::atomic<float> tune_knee_us_{25.0f};   // A/B 2026-08-30 18:42: tails 30-50 s at flat tau died in ~4 s
  std::atomic<float> tune_tau_min_s_{5.0f};  // floor of the error-proportional boost (same A/B)
  /// render_align channel (inter-device, on the exchanged tag-derived render phase): cap in us
  /// (0 = off; seeded from YAML render_align_max), gain per due report, deadband in us.
  // Defaults = the 2026-08-29 18:00-18:45 operating point: 45 min, wire median +2.7 us, robust sd
  // 5.0 us, 1-s change 0.19 us, zero events, with the channel applied at the measured sign.
  std::atomic<int32_t> tune_align_max_us_{500};  // 300 was reached within ~2 h by the ~1 us/min creep against the exchanged phase bias
  std::atomic<float> tune_align_gain_{0.3f};
  std::atomic<int32_t> tune_align_deadband_us_{1};  // covers the exchanged phase's own ~10 us bias; 3 made the bias creep ~1 us/min forever
  std::atomic<int32_t> tune_align_reject_us_{500};  // pairs beyond this are not a measurement
  std::atomic<int32_t> tune_align_step_us_{20};      // per due report (~10 s): 0.4 ppm at most
  std::atomic<bool> tune_align_apply_{true};        // false = shadow: log the step, move nothing
  /// Resync window (s) after an event, and the splice threshold (us) inside it. Target: |A-B| < 100 us
  /// within 5 s of a disturbance.
  std::atomic<float> tune_resync_win_s_{60.0f};  // 30 closed while A still sat at -114 us (build 34)
  std::atomic<float> tune_resync_gain_{1.0f};    // fraction of the measured error corrected per step (clean block)
  std::atomic<float> tune_resync_reopen_us_{400.0f};  // a block error past this re-opens the window
  std::atomic<int32_t> tune_resync_splice_us_{100};  // in-window coarse arm, one step per block (build 42); 150 left 50-115 us residuals to tau 120
  std::atomic<float> tune_resync_close_s_{5.0f};     // inside the arm threshold this long -> window closes
  std::atomic<int32_t> tune_resync_local_us_{2000};  // below this an in-window step needs the group delta to agree: common TIMEBASE STEPS reach +400 us (23:22:03, both boards), only starvation-class errors are local by construction
  std::atomic<int32_t> tune_resync_blank_ms_{1200}; // step-and-verify cadence: the judging block must START after the step (block 0.65 s + pipeline 0.28 s)
  std::atomic<int32_t> tune_block_n_{64};
  /// -1 = use config_.fast_splice_threshold_us.
  std::atomic<int32_t> tune_splice_us_{-1};
  std::atomic<int32_t> tune_tag_stale_ms_{1000};
  std::atomic<int32_t> tune_blank_ms_{500};
  std::atomic<int32_t> tune_gap_blank_ms_{50};
  /// Master toggle: adapt tau automatically from the block-error series (see the autotune block
  /// in delay_loop_update_). Off by default; a manual tau_s set while this is on will be
  /// overridden by the next adaptation and says so in the log.
  std::atomic<bool> tune_autotune_{false};
  /// Persistence master switch (servo_param persist 0/1): off lets a suspected NVS-write side
  /// effect (A-only post-boot receive stalls, 2026-08-28) be tested without a reflash.
  std::atomic<bool> tune_persist_{true};

 public:
  /// @brief Set a runtime loop parameter by name; returns false for an unknown name or a value
  /// outside its bounds. Names: tau_s [2..120], ti_s [10..1200], block_n [8..64], splice_us [0..10000, -1=config],
  /// tag_stale_ms [200..10000], blank_ms [100..2000], gap_blank_ms [10..500], autotune {0,1}, persist {0,1}.
  bool set_servo_param(const std::string &name, float value);
  /// Save the integral's slow average to NVS now (shutdown hook); see persist_now() in the .cpp.
  void persist_now();

 protected:
#endif
  std::unique_ptr<uint8_t[]> slice_buffer_;
  static constexpr size_t SLICE_BUFFER_SIZE = 4096;
  // Rolling pre-trigger history for the resync trace; written and read by the player task only.
  // Here rather than in ServoState because ServoState is that task's stack. See PreSample.
  PreSample pre_trace_[RESYNC_PRE_CHUNKS]{};
  // Most recent pushed frame, for click-free servo insertion (player task only)
  uint8_t last_frame_[8]{};
  uint32_t last_frame_bytes_{0};
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32
