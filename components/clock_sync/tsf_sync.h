#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_CLOCK_SYNC_TSF_SYNC) && defined(USE_WIFI)
#define CLOCK_SYNC_TSF_ACTIVE
#endif

#ifdef CLOCK_SYNC_TSF_ACTIVE

#include "esphome/core/helpers.h"

#include <algorithm>
#include <esp_timer.h>
#include <atomic>
#include <limits>
#include <cstdint>
#include <vector>

namespace esphome::clock_sync {

/// @brief TSF group sync, LEADERLESS: all clients on one wifi AP share the AP's TSF timer to
/// ~us, so if they also agree on ONE server->TSF mapping, their mutual sync is TSF-grade while
/// the group tracks the server through a shared estimate (its wander is common-mode, which is
/// inaudible; per-device estimate wander against each other is what moves a stereo image).
///
/// THE AGREEMENT IS A CONSENSUS AVERAGE, NOT A LEADER'S NUMBER. Every device multicasts its OWN
/// raw server<->TSF estimate once a second as a linear function {tsf_base, tsf-server at base,
/// drift in TSF units}, and every device adopts the (robustly weighted) MEAN of everyone's,
/// including its own. Nobody is elected, so nothing is handed over.
///
/// Why this shape and not leader election, which this replaced:
///
///   * TSF IS ALREADY LEADERLESS -- the AP's counter is a shared timebase every station reads
///     directly and the AP does not participate, essentially Reference Broadcast Synchronization.
///     The leader existed for one narrow job, publishing a mapping; sharing a number does not
///     require electing anyone.
///   * Noise averages down as sqrt(N) instead of being inherited wholesale from one device.
///   * Nothing to hand over, so no reference discontinuity to correct around. Leadership changed
///     SIX TIMES IN SEVENTEEN MINUTES on a two-device group (it required a healthy device and
///     every resync briefly disqualified the incumbent), and every quantity referenced to the
///     leader moved with it -- four measured bugs were downstream of that alone.
///   * A device rebooting shifts the mean slightly instead of collapsing the timebase.
///   * Symmetric: an observer needs no special status, and the `always_healthy` escape hatch that
///     propped up leader stability is gone with the leader.
///
/// TWO INVARIANTS HOLD THE DESIGN UP, and breaking either would look healthy from inside:
///
///   1. NEVER PUBLISH THE CONSENSUS. What goes on the wire is this device's own raw estimate
///      (slew-limited against its own Kalman, nothing else). Feeding the adopted mean back into
///      the beacon is positive feedback: the whole group can then drift together while every
///      device agrees with every other. Only an outside reference could see it.
///   2. THE ADOPTED MAPPING MUST BE A DETERMINISTIC FUNCTION OF THE LIVE ESTIMATE SET -- no
///      per-device history, no slew. What a leader got right, and the only thing it got right, is
///      that every device computed deadlines from ONE IDENTICAL line, so the mapping's own error
///      was exactly common-mode and cancelled between devices. Any per-device path to the same
///      target destroys that: measured, an adoption slew cost 2.7x on sd (9.72 vs 3.6). Stepping
///      is safe BECAUSE it is deterministic -- every device holding the same set steps to the same
///      value at once, and common-mode timebase motion is inaudible.
///
/// Any failure (no packets, client isolation, roam, AP reboot, no wifi) falls back to the
/// caller's own Kalman offset: never worse than the non-TSF behavior. A lone device consenses
/// with itself, which is exactly its own published line.
///
/// THREAD CONTEXT: service() is network-task-only. shared_server_offset_us() and the diagnostics
/// accessors may be called from the player task.
class TsfSync {
 public:
  struct Estimate {
    bool valid{false};
    // Settled enough to PUBLISH: a fresh estimate can be 100+ ms off and converge in large
    // steps. Under a leader those steps dragged the whole group through hard resyncs; under
    // consensus they would drag the MEAN, which is the same damage divided by N. Still gated.
    bool mature{false};
    double offset_ms{0.0};  // server − client, ms (Kalman)
    double drift{0.0};      // offset ms per client ms (Kalman; 0 if insignificant)
  };

  /// @param plausibility_us reject peer mappings whose implied offset disagrees with our
  /// own Kalman estimate by more than this (callers mirror the hard-resync threshold).
  explicit TsfSync(int64_t plausibility_us) : plausibility_us_(plausibility_us) {}
  ~TsfSync();

  /// @brief Runs receive/publish/consensus. Call periodically (~every message / idle tick)
  /// from the network task while a session is active.
  /// @param local_now_us esp_timer time of the sampled @p est.
  /// @param est the caller's own Kalman estimate at @p local_now_us.
  /// @param server_id_hash identity of the connected server (FNV-1a of host:port);
  /// estimates are only pooled between clients of the same server.
  /// @param stream_id_hash identity of the snapcast STREAM being played (FNV-1a of its
  ///        name), 0 when unknown. The group is scoped to it as well as the BSS:
  ///        render_phase is only comparable between devices on the same stream.
  void service(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash,
               uint32_t stream_id_hash = 0);

  /// @brief Computes the shared server−client offset, sampling TSF internally.
  /// @param local_now_us caller's esp_timer now (staleness check).
  /// @return false when no live, sane mapping is held (age clamp, expiry, no TSF):
  /// the caller uses its own Kalman offset.
  bool shared_server_offset_us(int64_t local_now_us, int64_t &offset_us);

  /// @brief Dump the group-consensus INPUTS: own phase, every peer phase with its age, the
  /// resulting group delta.
  ///
  /// Every conclusion about render_group_delta_us so far was inferred from its OUTPUT, and those
  /// inferences contradicted each other -- a regression against a logic analyser put the slope at
  /// -0.10 where the arithmetic demands -2.0. An output that is already suspect cannot diagnose
  /// itself; the inputs have to be visible.
  void log_phase_inputs(int64_t local_now_us) const;

  /// @brief Reports our own playout pipeline depth (pushed-but-unplayed audio, us).
  /// Published in our beacons and compared against the group's, because absolute
  /// depth is invisible to the sync median: the median is measured against this
  /// device's own predicted playout, so an accounting offset shifts prediction and
  /// audio together and reads as zero error. The group is the only reference we have.
  /// THREAD CONTEXT: player task (atomic).
  void set_pipeline_us(int32_t pipeline_us) { this->pipeline_us_.store(pipeline_us, std::memory_order_relaxed); }

  /// @brief Our pipeline depth minus the mean of our peers', us, or INT32_MIN when unknown
  /// (no peer has reported one, or we have not). Diagnostics.
  int32_t pipeline_delta_us() const { return this->pipeline_delta_us_.load(std::memory_order_relaxed); }

  /// @brief Publish this device's RENDER PHASE: the TSF instant at which it renders server
  /// audio time zero. See TsfPacket::render_phase_us for the derivation. Pass
  /// RENDER_PHASE_UNKNOWN before anything has rendered.
  /// @param at_us local time the phase describes. REQUIRED for the group average: the phase is an
  ///        absolute TSF-vs-server offset that drifts continuously, so differencing a fresh peer
  ///        phase against a stale local one injects (drift x staleness) of pure error. This is
  ///        written once per sync report (~3.3 s) while peer beacons arrive up to 4x faster, and
  ///        at ~50 ppm relative drift 3.3 s of staleness is ~165 us -- which is the entire signal.
  ///        Same lesson as AudioDepth::as_of_us: evaluate the comparison AT the instant, not at
  ///        read time.
  void set_render_phase_us(int64_t phase_us, int64_t at_us = 0) {
    this->render_phase_us_.store(phase_us, std::memory_order_relaxed);
    this->render_phase_at_us_.store(at_us, std::memory_order_relaxed);
    if (at_us != 0) {
      this->recompute_group_delta_(at_us);
    }
  }

  /// @brief This device's render phase minus the GROUP AVERAGE, us; INT32_MIN when fewer than two
  ///        devices have published one inside the pairing window.
  ///
  /// THE true relative playout offset: two devices playing the same stream must map a given
  /// server frame to the same TSF instant, so a non-zero difference is real skew, measured
  /// without the servo, the prediction model or the pipeline depth in the path.
  ///
  /// AVERAGE, NOT MEDIAN. With two devices a median IS the mean, and it behaved. With three it is
  /// the MIDDLE VALUE, which hops whenever the ordering changes -- two phases close together and
  /// a third crossing between them steps the target with no real movement behind it. Measured
  /// 2026-08-28 with a third device in the group: the speakers' group delta swung +-96 us
  /// (+14 -31 +63 +81 +13 +11 +96 -33) while the observer, holding the same data at the same
  /// instant, read median +3 us sd 12. Robustness against outliers now comes from continuous
  /// weighting instead, which cannot step.
  int32_t render_group_delta_us() const {
    return this->render_group_delta_us_.load(std::memory_order_relaxed);
  }
  /// Age of the published render phase at this instant, in ms, for the beacon: 0xFFFF when there is
  /// no phase or no sample instant. Network task.
  uint16_t render_phase_age_ms_() const {
    const int64_t at = this->render_phase_at_us_.load(std::memory_order_relaxed);
    if (at == 0 || this->render_phase_us_.load(std::memory_order_relaxed) == RENDER_PHASE_UNKNOWN) {
      return 0xFFFF;
    }
    const int64_t age_ms = (esp_timer_get_time() - at) / 1000;
    return static_cast<uint16_t>(std::clamp<int64_t>(age_ms, 0, 0xFFFE));
  }

  /// @brief Our crystal rate minus the mean of our peers', in ppm, or NaN when unknown.
  ///
  /// Each device measures its own clock against the RADIO timebase, so this difference is a
  /// hardware property measured entirely outside the audio servo loop. It is the term that
  /// stands between the differential trim and a usable rate reference: with a logic analyser
  /// the differential trim sits -5.25..-5.40 ppm from the true differential achieved rate,
  /// stable across runs, and that offset is this quantity. Subtracting it took the integrated
  /// error from 505 us per 100 s to 17.
  ///
  /// Diagnostics only. Nothing steers on it, and it should not until the residual after
  /// correction (0.708 ppm, ~71 us per 100 s against a ~7 us floor) is understood.
  float crystal_delta_ppm() const { return this->crystal_delta_ppm_.load(std::memory_order_relaxed); }

  /// @brief This device's own crystal rate against the radio timebase, ppm, or NaN when unknown.
  /// Available within seconds of boot -- the delay loop seeds a cold (no-NVS) integral from it.
  float own_crystal_ppm() const { return this->pub_crystal_ppm_.load(std::memory_order_relaxed); }

  /// @brief This device's own render phase, or RENDER_PHASE_UNKNOWN. Diagnostics: a delta is
  /// only absent because one SIDE is unknown, and without seeing both there is no way to tell
  /// which.
  int64_t render_phase_us() const { return this->render_phase_us_.load(std::memory_order_relaxed); }

  static constexpr int64_t RENDER_PHASE_UNKNOWN = INT64_MIN;

  /// @brief Unicast peer roster (sockaddr s_addr values, network byte order), from
  /// the snapserver's Server.GetStatus client list. Every beacon is unicast to every
  /// peer in addition to the multicast: client-to-client multicast is unreliable on
  /// many APs (isolation, IGMP snooping, mesh filtering), while unicast between
  /// associated clients works wherever snapcast itself does.
  /// THREAD CONTEXT: network task (same as service()).
  void set_peers(std::vector<uint32_t> peer_addrs);

  /// @brief One raw sandwiched TSF sample: the AP's counter paired with our esp_timer,
  /// plus the bracket width that pairing was measured to. Exposed for offline
  /// cross-device analysis -- it is the only clock two devices provably share, so it is
  /// the reference any real timing comparison has to be expressed in.
  /// THREAD CONTEXT: any; performs its own reads.
  static bool raw_tsf_sample(int64_t &tsf_us, int64_t &local_us, int64_t &width_us) {
    width_us = 0;
    return sample_tsf_(tsf_us, local_us, &width_us);
  }

  /// @brief How many estimates went into the last consensus, our own included. 0 = no mapping
  /// at all (Kalman fallback), 1 = alone with our own line, >=2 = a genuinely shared timebase.
  uint8_t consensus_n() const { return this->consensus_n_.load(std::memory_order_relaxed); }

  /// @brief Counts genuine RE-ANCHORS of the adopted mapping -- a consensus move too large to
  /// slew, i.e. the timebase the deadline is measured against actually stepped.
  ///
  /// This is the leaderless replacement for watching the role change. A handover used to swap
  /// the timebase wholesale, so anything the servo had converged to was suddenly measured against
  /// a different clock and the servo needed telling. Consensus removes the routine case (a join
  /// or a departure now slews), leaving only the real discontinuities to report.
  uint32_t timebase_epoch() const { return this->timebase_epoch_.load(std::memory_order_relaxed); }

  /// @brief Age of the active mapping in seconds, or -1 when none. Diagnostics.
  float mapping_age_s(int64_t local_now_us);
  /// @brief Unicast peers currently on the roster. Diagnostics (any thread).
  uint8_t peer_count() const { return this->peer_count_.load(std::memory_order_relaxed); }

 protected:
  bool ensure_socket_();
  void reset_(const char *reason);
  void receive_(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash,
                uint32_t stream_id_hash);
  void broadcast_(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash,
                  uint32_t stream_id_hash);
  /// Stores a mapping as the active one (players compute deadlines from it).
  void adopt_(int64_t tsf_base_us, int64_t tsf_minus_server_us, float drift_ppm, int64_t local_now_us);
  /// Averages every live raw estimate -- ours and every peer's -- and slews the adopted
  /// mapping toward the result. THE core of the leaderless design; see the definition.
  void update_consensus_(int64_t local_now_us);
  /// Compares our playout depth and crystal rate against the group mean and warns on sustained
  /// divergence. Diagnostics only: never touches the mapping.
  void update_group_diagnostics_(int64_t local_now_us);
  /// Sandwiched TSF read: local/tsf/local, midpoint local, retried when an
  /// interrupt widens the sandwich. @return false if TSF is unavailable.
  static bool sample_tsf_(int64_t &tsf_us, int64_t &local_us, int64_t *width_out = nullptr);

  enum class EvalResult : uint8_t { OK, NO_TSF, AGE_CLAMP };
  /// Evaluates a mapping at a fresh TSF sample. NO_TSF: TSF unreadable (sampling
  /// failed). AGE_CLAMP: extrapolation negative or too old (AP reboot resets TSF;
  /// extrapolating across that would produce garbage deadlines).
  /// @param tsf_out,local_out the sandwich sample the evaluation used, so a caller that
  /// tracks the TSF-vs-local crystal ratio does not have to take a second read for it.
  static EvalResult evaluate_mapping_(int64_t tsf_base_us, int64_t tsf_minus_server_us, float drift_ppm,
                                      int64_t &offset_us, int64_t &extrapolation_us, int64_t *width_out = nullptr,
                                      int64_t *tsf_out = nullptr, int64_t *local_out = nullptr);

  const int64_t plausibility_us_;

  int sock_{-1};
  bool joined_{false};
  uint8_t my_mac_[6]{};
  bool have_mac_{false};
  uint8_t bssid_[6]{};
  bool have_bssid_{false};

  std::atomic<int32_t> pipeline_us_{INT32_MIN};
  std::atomic<int32_t> pipeline_delta_us_{INT32_MIN};
  std::atomic<int64_t> render_phase_us_{INT64_MIN};
  /// Local time render_phase_us_ describes; 0 when unknown. See set_render_phase_us().
  std::atomic<int64_t> render_phase_at_us_{0};
  int64_t pipeline_diverged_since_us_{0};  // 0 = currently within tolerance
  int64_t last_diverge_log_us_{0};
  int64_t last_render_log_us_{0};
  int64_t last_tx_us_{0};        // our last mapping beacon
  int64_t last_phase_tx_us_{0};  // our last phase-only beacon (no mature estimate yet)
  int64_t last_service_us_{0};   // rate-limits the wifi state polling
  bool warned_rejected_{false};  // one log line per rejection episode
  bool warned_foreign_bss_{false};
  bool warned_foreign_server_{false};
  bool warned_foreign_stream_{false};

  /// One slot per peer MAC. Fixed and small: a stream group is a handful of speakers, and a full
  /// table evicts the stalest entry rather than allocating on the network task.
  static constexpr size_t MAX_PEERS = 8;
  /// A phase older than this says nothing about where that device is now.
  static constexpr int64_t PHASE_STALE_US = 15000000;
  /// A raw estimate older than this is dropped from the consensus. Tighter than PHASE_STALE_US
  /// because this one steers the timebase rather than reporting on it: beacons come once a
  /// second, so this tolerates four consecutive losses before a peer stops counting. A departing
  /// device therefore leaves the mean gradually-ish, and the adoption slew smooths the rest.
  static constexpr int64_t PEER_MAP_STALE_US = 5000000;
  /// How far apart two phases may have been sampled and still be worth differencing. They are
  /// absolute offsets drifting at ~50 ppm between devices, so the pairing error is
  /// window x drift: 300 ms bounds it at ~15 us, against a signal of order 100 us. Anything
  /// wider and the comparison measures drift rather than skew -- which is what it was doing.
  static constexpr int64_t PHASE_PAIR_WINDOW_US = 300000;
  struct Peer {
    uint8_t mac[6];
    bool used;
    int64_t seen_us;  // last accepted packet from this peer (eviction order)
    // Render phase, and the LOCAL instant we received it -- the pairing window is applied to
    // that, not to the phase's own value. RENDER_PHASE_UNKNOWN when the peer has not rendered.
    int64_t phase_us;
    int64_t phase_seen_us;
    // The peer's RAW published server<->TSF line. A line, not a point, so it is evaluated at a
    // common instant rather than needing to have been sampled at one.
    bool map_valid;
    int64_t map_seen_us;
    int64_t tsf_base_us;
    int64_t tms_base_us;
    float drift_ppm;
    // Diagnostics the peer publishes about itself.
    int32_t pipeline_us;
    float crystal_ppm;
  };
  Peer peer_[MAX_PEERS]{};
  Peer *find_peer_(const uint8_t mac[6], int64_t local_now_us);

  std::atomic<int32_t> render_group_delta_us_{INT32_MIN};
  /// Local time render_group_delta_us_ was last computed from a VALID pairing.
  ///
  /// A failed pairing must not wipe a good delta. recompute runs on every beacon arrival, but
  /// only ~25% of arrivals land inside PHASE_PAIR_WINDOW_US of our own phase instant, so
  /// clearing on failure destroyed the value four times out of five -- and the consumer, which
  /// checks once every three reports, then almost never saw one. Measured: zero corrections ran.
  int64_t group_delta_at_us_{0};
  /// A delta older than this is stale even if it was valid when computed.
  static constexpr int64_t GROUP_DELTA_STALE_US = 10000000;
  /// Cheap phase-only report: no TSF read, no rate state, multicast only. For a device with no
  /// mature estimate to contribute -- it still has a render phase worth publishing.
  void broadcast_phase_only_(uint32_t server_id_hash, uint32_t stream_id_hash);
  void record_peer_phase_(Peer &peer, int64_t phase_us, int64_t local_now_us);
  void recompute_group_delta_(int64_t local_now_us);
  bool warned_tx_{false};
  uint32_t rx_peer_count_{0};  // accepted packets (diagnostics)

  // Unicast peers (network task only) + count for diagnostics. peers_ comes from
  // the server roster; learned_peers_ from received packets' source addresses --
  // this closes the boot race where the earlier device's roster predates the later
  // device's connection and multicast is blocked.
  std::vector<uint32_t> peers_;
  std::vector<uint32_t> learned_peers_;
  std::atomic<uint8_t> peer_count_{0};

  void learn_peer_(uint32_t addr);
  void update_peer_count_() {
    this->peer_count_.store(static_cast<uint8_t>(this->peers_.size() + this->learned_peers_.size()),
                            std::memory_order_relaxed);
  }

  // Our own TSF-vs-esp_timer rate (d(tsf−local)/dt, ppm), measured on the network task: the
  // published drift must be in TSF units, and the AP-vs-our crystal difference (up to ~±40 ppm)
  // usually dominates the Kalman drift itself
  int64_t rate_ref_tsf_us_{0};
  int64_t rate_ref_local_us_{0};
  float tsf_rate_ppm_{0.0f};
  bool tsf_rate_valid_{false};

  // OUR OWN published (slew-limited) mapping line, steered toward the live Kalman estimate at
  // most ~50 us per second so estimate jitter is not broadcast. THIS is what goes on the wire and
  // what we contribute to the consensus -- never the consensus itself. See the class comment.
  bool pub_valid_{false};
  int64_t pub_tsf_base_{0};
  int64_t pub_tms_base_{0};
  float pub_drift_ppm_{0.0f};

  // Active (consensus) mapping: server_us(t) = tsf(t) − (tsf_minus_server + drift·(tsf − base))
  Mutex mapping_mutex_;
  bool mapping_valid_{false};
  int64_t map_tsf_base_us_{0};
  int64_t map_tsf_minus_server_us_{0};
  float map_drift_ppm_{0.0f};
  int64_t map_updated_local_us_{0};
  std::atomic<uint8_t> consensus_n_{0};
  std::atomic<uint32_t> timebase_epoch_{0};
  int64_t last_consensus_us_{0};
  int64_t last_consensus_log_us_{0};
  // Low-pass state for shared_server_offset_us(); player-task-only
  double offset_filter_us_{0.0};
  bool offset_filter_valid_{false};
  /// @brief Whether the offset filter has ever held a value. Distinct from offset_filter_valid_,
  /// which is cleared whenever the mapping is momentarily unavailable: carrying the filter across
  /// such a gap is what stops a momentary mapping outage stepping the deadline by the filter's
  /// accumulated tracking lag. Never cleared once set.
  bool offset_filter_seeded_{false};
  /// @brief Local instant the offset filter's state describes -- the midpoint of the sandwich
  /// that last moved it. The feed-forward step below is a rate times an interval, and this is
  /// the interval's start. 0 = no sample yet.
  int64_t offset_filter_local_us_{0};
  /// @brief This device's TSF-vs-esp_timer rate, d(tsf - local)/dt in ppm, measured from the
  /// samples the offset filter already takes. A crystal ratio: stable, hardware-only, and in
  /// particular independent of the mapping, so a consensus move or a slew cannot corrupt it.
  /// Distinct from tsf_rate_ppm_, which is the same quantity measured on the network task --
  /// this one is measured where it is needed, on the player task.
  /// THREAD CONTEXT: player task only.
  double offset_rate_ppm_{0.0};
  bool offset_rate_valid_{false};
  int64_t offset_rate_ref_tsf_us_{0};
  int64_t offset_rate_ref_local_us_{0};
  /// @brief Cross-thread mirror of offset_rate_ppm_, for the network task to publish in the
  /// beacon. offset_rate_ppm_ itself is player-task-only, and broadcast_ runs on the network
  /// task, so it must not read it directly. NaN until the first measurement.
  std::atomic<float> pub_crystal_ppm_{std::numeric_limits<float>::quiet_NaN()};
  /// @brief Our crystal rate minus the group mean, ppm, or NaN when unknown. This is the term
  /// that stands between the differential trim and a usable rate reference: the trim sits
  /// -5.25..-5.40 ppm from the true differential achieved rate, and that offset IS this
  /// quantity. Diagnostics only; nothing steers on it yet.
  std::atomic<float> crystal_delta_ppm_{std::numeric_limits<float>::quiet_NaN()};
  int64_t last_crystal_log_us_{0};
  // Per-device sandwich floor, so the trust threshold is derived rather than assumed
  int64_t sandwich_floor_us_{0};
  int64_t sandwich_block_min_us_{0};
  uint32_t sandwich_block_n_{0};
};

}  // namespace esphome::clock_sync

#endif  // CLOCK_SYNC_TSF_ACTIVE
