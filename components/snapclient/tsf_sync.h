#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_SNAPCLIENT_TSF_SYNC) && defined(USE_WIFI)
#define SNAPCLIENT_TSF_ACTIVE
#endif

#ifdef SNAPCLIENT_TSF_ACTIVE

#include "esphome/core/helpers.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace esphome::snapclient {

/// @brief TSF group sync (PLAN-tsf-sync.md): all clients on one wifi AP share the
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
  void service(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash);

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
  void set_playout_healthy(bool healthy) { this->playout_healthy_.store(healthy, std::memory_order_relaxed); }

  /// @brief Unicast peer roster (sockaddr s_addr values, network byte order), from
  /// the snapserver's Server.GetStatus client list. The leader unicasts its beacon
  /// to every peer in addition to the multicast group: client-to-client multicast
  /// is unreliable on many APs (isolation, IGMP snooping, mesh filtering), while
  /// unicast between associated clients works wherever snapcast itself does.
  /// THREAD CONTEXT: network task (same as service()).
  void set_peers(std::vector<uint32_t> peer_addrs);

  Role role() const { return this->role_.load(std::memory_order_relaxed); }
  /// @brief Age of the active mapping in seconds, or -1 when none. Diagnostics.
  float mapping_age_s(int64_t local_now_us);
  /// @brief Unicast peers currently on the roster. Diagnostics (any thread).
  uint8_t peer_count() const { return this->peer_count_.load(std::memory_order_relaxed); }

 protected:
  bool ensure_socket_();
  void reset_(const char *reason);
  void receive_(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash);
  void broadcast_(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash);
  /// Stores a mapping (from a packet, or our own broadcast) as the active one.
  void adopt_(int64_t tsf_base_us, int64_t tsf_minus_server_us, float drift_ppm, int64_t local_now_us);
  /// Sandwiched TSF read: local/tsf/local, midpoint local, retried when an
  /// interrupt widens the sandwich. @return false if TSF is unavailable.
  static bool sample_tsf_(int64_t &tsf_us, int64_t &local_us);

  enum class EvalResult : uint8_t { OK, NO_TSF, AGE_CLAMP };
  /// Evaluates a mapping at a fresh TSF sample. NO_TSF: TSF unreadable (sampling
  /// failed). AGE_CLAMP: extrapolation negative or too old (AP reboot resets TSF;
  /// extrapolating across that would produce garbage deadlines).
  static EvalResult evaluate_mapping_(int64_t tsf_base_us, int64_t tsf_minus_server_us, float drift_ppm,
                                      int64_t &offset_us, int64_t &extrapolation_us);

  const int64_t plausibility_us_;

  int sock_{-1};
  bool joined_{false};
  uint8_t my_mac_[6]{};
  bool have_mac_{false};
  uint8_t bssid_[6]{};
  bool have_bssid_{false};

  std::atomic<Role> role_{Role::IDLE};
  std::atomic<bool> playout_healthy_{false};
  int64_t unhealthy_since_us_{0};  // leader only; 0 = healthy
  uint8_t leader_mac_[6]{};
  int64_t last_rx_us_{0};       // last valid packet from another leader
  int64_t last_tx_us_{0};       // our last broadcast (leader)
  int64_t last_service_us_{0};   // rate-limits the wifi state polling
  bool warned_rejected_{false};  // one log line per rejection episode
  bool warned_foreign_bss_{false};
  bool warned_foreign_server_{false};
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
};

}  // namespace esphome::snapclient

#endif  // SNAPCLIENT_TSF_ACTIVE
