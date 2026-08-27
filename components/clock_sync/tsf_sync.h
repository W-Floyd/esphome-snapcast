#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_CLOCK_SYNC_TSF_SYNC) && defined(USE_WIFI)
#define CLOCK_SYNC_TSF_ACTIVE
#endif

#ifdef CLOCK_SYNC_TSF_ACTIVE

#include "esphome/core/helpers.h"

#include <atomic>
#include <limits>
#include <cstdint>
#include <vector>

namespace esphome::clock_sync {

/// @brief TSF group sync: all clients on one wifi AP share the
/// AP's TSF timer to ~µs, so if they also share ONE server→TSF mapping, their
/// mutual sync is TSF-grade while the group tracks the server through a single
/// member's Kalman estimate (its wander becomes common-mode, which is inaudible;
/// per-device estimate wander against each other is what moves a stereo image).
///
/// One leader per BSS (lowest MAC wins) multicasts its mapping once a second as a
/// linear function {tsf_base, tsf−server at base, drift in TSF units} so followers
/// extrapolate between beacons. Everyone — leader included — computes deadlines
/// from the *published* mapping, so all members quantize identically. Any failure
/// (no packets, client isolation, roam, AP reboot, no wifi) falls back to the
/// caller's own Kalman offset: never worse than the non-TSF behavior.
///
/// THREAD CONTEXT: service() is network-task-only. shared_server_offset_us() and
/// the diagnostics accessors may be called from the player task.
class TsfSync {
 public:
  struct Estimate {
    bool valid{false};
    // Settled enough to PUBLISH: a fresh estimate can be 100+ ms off and converge
    // in large steps -- a leader broadcasting those steps drags the whole group
    // through hard resyncs (observed fleet-wide after a simultaneous reflash)
    bool mature{false};
    double offset_ms{0.0};  // server − client, ms (Kalman)
    double drift{0.0};      // offset ms per client ms (Kalman; 0 if insignificant)
  };

  enum class Role : uint8_t { IDLE, FOLLOWER, LEADER };

  /// @param plausibility_us reject mappings whose implied offset disagrees with our
  /// own Kalman estimate by more than this (callers mirror the hard-resync threshold).
  explicit TsfSync(int64_t plausibility_us) : plausibility_us_(plausibility_us) {}
  ~TsfSync();

  /// @brief Runs receive/election/broadcast. Call periodically (~every message /
  /// idle tick) from the network task while a session is active.
  /// @param local_now_us esp_timer time of the sampled @p est.
  /// @param est the caller's own Kalman estimate at @p local_now_us.
  /// @param server_id_hash identity of the connected server (FNV-1a of host:port);
  /// mappings are only shared between clients of the same server.
  /// @param stream_id_hash identity of the snapcast STREAM being played (FNV-1a of its
  ///        name), 0 when unknown. Leadership is scoped to it as well as the BSS:
  ///        render_phase is only comparable between devices on the same stream.
  void service(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash,
               uint32_t stream_id_hash = 0);

  /// @brief Computes the shared server−client offset, sampling TSF internally.
  /// @param local_now_us caller's esp_timer now (staleness check).
  /// @return false when no live, sane mapping is held (age clamp, expiry, no TSF):
  /// the caller uses its own Kalman offset.
  bool shared_server_offset_us(int64_t local_now_us, int64_t &offset_us);

  /// @brief Reports whether OUR OWN playout is currently tracking the timebase
  /// (converged and the median error small). A leader publishes the timebase the
  /// whole group follows, so a leader whose own playout has diverged -- mid-recovery,
  /// or stuck in a degraded buffer state -- must hand off rather than keep
  /// broadcasting. Only a healthy device may assume leadership.
  /// THREAD CONTEXT: player task (atomic).
  /// @brief Report our own tracking quality to the TSF layer.
  /// @param healthy playout is converged and tracking the timebase
  /// @param deadline_implausible the error is so large that the DEADLINE, not our
  ///        clock, must be wrong -- e.g. a resuming stream whose first chunk is
  ///        already past its playout time. A leader must not read this as evidence of
  ///        its own fault: the mapping it publishes is server<->TSF and stays valid
  ///        regardless, and an implausible deadline arrives group-wide, so stepping
  ///        down only destroys the timebase everyone is relying on.
  void set_playout_healthy(bool healthy, bool deadline_implausible = false) {
    this->playout_healthy_.store(healthy, std::memory_order_relaxed);
    this->deadline_implausible_.store(deadline_implausible, std::memory_order_relaxed);
  }

  /// @brief Reports our own playout pipeline depth (pushed-but-unplayed audio, us).
  /// Published in our beacons and compared against the leader's, because absolute
  /// depth is invisible to the sync median: the median is measured against this
  /// device's own predicted playout, so an accounting offset shifts prediction and
  /// audio together and reads as zero error. The group is the only reference we have.
  /// THREAD CONTEXT: player task (atomic).
  void set_pipeline_us(int32_t pipeline_us) { this->pipeline_us_.store(pipeline_us, std::memory_order_relaxed); }

  /// @brief Our pipeline depth minus the leader's, us, or INT32_MIN when unknown (no
  /// leader mapping, leader too old to report it, or we are the leader). Diagnostics.
  int32_t pipeline_delta_us() const { return this->pipeline_delta_us_.load(std::memory_order_relaxed); }

  /// @brief Publish this device's RENDER PHASE: the TSF instant at which it renders server
  /// audio time zero. See TsfPacket::render_phase_us for the derivation. Pass
  /// RENDER_PHASE_UNKNOWN before anything has rendered.
  void set_render_phase_us(int64_t phase_us) { this->render_phase_us_.store(phase_us, std::memory_order_relaxed); }
  /// @brief This device's render phase minus the leader's, us; INT32_MIN when either side has
  /// not published one. THE true relative playout offset: two devices playing the same stream
  /// must map a given server frame to the same TSF instant, so a non-zero difference is real
  /// skew, measured without the servo, the prediction model or the pipeline depth in the path.
  int32_t render_delta_us() const { return this->render_delta_us_.load(std::memory_order_relaxed); }

  /// @brief Our crystal rate minus the leader's, in ppm, or NaN when either side is unknown.
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
  /// @brief This device's own render phase, or RENDER_PHASE_UNKNOWN. Diagnostics: a delta is
  /// only absent because one SIDE is unknown, and without seeing both there is no way to tell
  /// which -- a leader publishes a phase but reports no delta, so its own value is otherwise
  /// invisible.
  int64_t render_phase_us() const { return this->render_phase_us_.load(std::memory_order_relaxed); }

  static constexpr int64_t RENDER_PHASE_UNKNOWN = INT64_MIN;

  /// @brief Unicast peer roster (sockaddr s_addr values, network byte order), from
  /// the snapserver's Server.GetStatus client list. The leader unicasts its beacon
  /// to every peer in addition to the multicast group: client-to-client multicast
  /// is unreliable on many APs (isolation, IGMP snooping, mesh filtering), while
  /// unicast between associated clients works wherever snapcast itself does.
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

  Role role() const { return this->role_.load(std::memory_order_relaxed); }
  /// @brief Age of the active mapping in seconds, or -1 when none. Diagnostics.
  float mapping_age_s(int64_t local_now_us);
  /// @brief Unicast peers currently on the roster. Diagnostics (any thread).
  uint8_t peer_count() const { return this->peer_count_.load(std::memory_order_relaxed); }

 protected:
  bool ensure_socket_();
  /// @brief Hand off leadership while keeping the mapping, rate estimate and peers --
  /// everything a demotion does not invalidate. See the definition for why this is not
  /// reset_(), which is for the network genuinely changing underneath us.
  void demote_(const char *reason);
  void reset_(const char *reason);
  void receive_(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash,
                uint32_t stream_id_hash);
  void broadcast_(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash,
                  uint32_t stream_id_hash);
  /// Stores a mapping (from a packet, or our own broadcast) as the active one.
  void adopt_(int64_t tsf_base_us, int64_t tsf_minus_server_us, float drift_ppm, int64_t local_now_us);
  /// On promotion, continue the line the group is already playing to instead of
  /// re-anchoring to our own estimate (which would step every follower).
  void seed_published_from_mapping_();
  /// Compares our playout depth against the leader's and warns on sustained
  /// divergence. Diagnostics only: never touches the mapping.
  void check_crystal_delta_(float leader_crystal_ppm, int64_t local_now_us);
  void check_pipeline_divergence_(int32_t leader_pipeline_us, int64_t local_now_us);
  void check_render_phase_(int64_t leader_phase_us, int64_t local_now_us);
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

  std::atomic<Role> role_{Role::IDLE};
  std::atomic<bool> playout_healthy_{false};
  std::atomic<bool> deadline_implausible_{false};
  std::atomic<int32_t> pipeline_us_{INT32_MIN};
  std::atomic<int32_t> pipeline_delta_us_{INT32_MIN};
  std::atomic<int64_t> render_phase_us_{INT64_MIN};
  std::atomic<int32_t> render_delta_us_{INT32_MIN};
  int64_t pipeline_diverged_since_us_{0};  // 0 = currently within tolerance
  int64_t last_diverge_log_us_{0};
  int64_t last_render_log_us_{0};
  int64_t unhealthy_since_us_{0};  // leader only; 0 = healthy
  int64_t healthy_since_us_{0};    // 0 = currently unhealthy
  int64_t no_lead_until_us_{0};    // cooldown after stepping down
  uint8_t leader_mac_[6]{};
  int64_t last_rx_us_{0};       // last valid packet from another leader
  int64_t last_tx_us_{0};       // our last broadcast (leader)
  int64_t last_service_us_{0};   // rate-limits the wifi state polling
  bool warned_rejected_{false};  // one log line per rejection episode
  bool warned_foreign_bss_{false};
  bool warned_foreign_server_{false};
  bool warned_foreign_stream_{false};
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

  // Leader's own TSF-vs-esp_timer rate (d(tsf−local)/dt, ppm): the published drift
  // must be in TSF units, and the AP-vs-leader crystal difference (up to ~±40 ppm)
  // usually dominates the Kalman drift itself
  int64_t rate_ref_tsf_us_{0};
  int64_t rate_ref_local_us_{0};
  float tsf_rate_ppm_{0.0f};
  bool tsf_rate_valid_{false};

  // Leader's published (slew-limited) mapping line, steered toward the live
  // Kalman estimate at most ~50 us per beacon so estimate jitter is not broadcast
  bool pub_valid_{false};
  int64_t pub_tsf_base_{0};
  int64_t pub_tms_base_{0};
  float pub_drift_ppm_{0.0f};

  // Active mapping: server_us(t) = tsf(t) − (tsf_minus_server + drift·(tsf − base))
  Mutex mapping_mutex_;
  bool mapping_valid_{false};
  int64_t map_tsf_base_us_{0};
  int64_t map_tsf_minus_server_us_{0};
  float map_drift_ppm_{0.0f};
  int64_t map_updated_local_us_{0};
  // Low-pass state for shared_server_offset_us(); player-task-only
  double offset_filter_us_{0.0};
  bool offset_filter_valid_{false};
  /// @brief Whether the offset filter has ever held a value. Distinct from offset_filter_valid_,
  /// which is cleared whenever the mapping is momentarily unavailable: carrying the filter across
  /// such a gap is what stops a leadership handover stepping the deadline by the filter's
  /// accumulated tracking lag. Never cleared once set.
  bool offset_filter_seeded_{false};
  /// @brief Local instant the offset filter's state describes -- the midpoint of the sandwich
  /// that last moved it. The feed-forward step below is a rate times an interval, and this is
  /// the interval's start. 0 = no sample yet.
  int64_t offset_filter_local_us_{0};
  /// @brief This device's TSF-vs-esp_timer rate, d(tsf - local)/dt in ppm, measured from the
  /// samples the offset filter already takes. A crystal ratio: stable, hardware-only, and in
  /// particular independent of the mapping, so a leader change or a slew cannot corrupt it.
  /// Distinct from tsf_rate_ppm_, which is the same quantity measured on the network task and
  /// only while leading -- this one is needed on every device in every role.
  /// THREAD CONTEXT: player task only.
  double offset_rate_ppm_{0.0};
  bool offset_rate_valid_{false};
  int64_t offset_rate_ref_tsf_us_{0};
  int64_t offset_rate_ref_local_us_{0};
  /// @brief Cross-thread mirror of offset_rate_ppm_, for the network task to publish in the
  /// beacon. offset_rate_ppm_ itself is player-task-only, and broadcast_ runs on the network
  /// task, so it must not read it directly. NaN until the first measurement.
  std::atomic<float> pub_crystal_ppm_{std::numeric_limits<float>::quiet_NaN()};
  /// @brief Our crystal rate minus the leader's, ppm, or NaN when either side is unknown. This
  /// is the term that stands between the differential trim and a usable rate reference: the
  /// trim sits -5.25..-5.40 ppm from the true differential achieved rate, and that offset IS
  /// this quantity. Diagnostics only; nothing steers on it yet.
  std::atomic<float> crystal_delta_ppm_{std::numeric_limits<float>::quiet_NaN()};
  int64_t last_crystal_log_us_{0};
  // Per-device sandwich floor, so the trust threshold is derived rather than assumed
  int64_t sandwich_floor_us_{0};
  int64_t sandwich_block_min_us_{0};
  uint32_t sandwich_block_n_{0};
};

}  // namespace esphome::clock_sync

#endif  // CLOCK_SYNC_TSF_ACTIVE
