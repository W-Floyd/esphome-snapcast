#include "tsf_sync.h"

#ifdef SNAPCLIENT_TSF_ACTIVE

#include "esphome/core/log.h"

#include <esp_timer.h>
#include <esp_wifi.h>
#include <fcntl.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace esphome::snapclient {

static const char *const TAG = "snapclient.tsf";

// Multicast rendezvous for the mapping packets; TTL 1 (never routed)
static const char *const TSF_GROUP = "239.255.83.84";
static constexpr uint16_t TSF_PORT = 47083;

static constexpr uint32_t TSF_MAGIC = 0x534E5446;  // 'SNTF'
// Bump on any wire change: mismatched packets are dropped, so a half-flashed fleet
// falls back to per-device Kalman rather than misreading fields.
static constexpr uint8_t TSF_VERSION = 1;

// Pipeline-depth divergence alarm. Absolute playout depth is the one quantity the
// sync median CANNOT see: the median is measured against this device's own
// predicted playout, so if the frame accounting is offset by F, the prediction is
// offset by F too and the error reads ~0 while the audio is physically F late.
// Observed: one device sat at 72-118 ms of pipeline against a fleet norm of ~250 ms
// and was audibly ~150 ms behind its pair, with textbook-clean sync reports on every
// device. The group is the only available reference, so followers compare their own
// depth against the leader's and shout when it diverges. Threshold sits above the
// normal spread (224-282 ms measured across four identical boards = 58 ms) so
// healthy variation stays quiet.
static constexpr int32_t PIPELINE_DIVERGE_MS = 100;
static constexpr int64_t PIPELINE_DIVERGE_MIN_US = 5000000;
static constexpr int64_t PIPELINE_DIVERGE_LOG_US = 30000000;
static constexpr int16_t PIPELINE_UNKNOWN = INT16_MIN;

static constexpr int64_t BEACON_INTERVAL_US = 1000000;   // leader broadcast cadence
static constexpr int64_t LEADER_TIMEOUT_US = 3500000;    // silence before takeover…
static constexpr int64_t STAGGER_STEP_US = 100000;       // …plus per-MAC stagger
static constexpr int64_t MAPPING_EXPIRY_US = 5000000;    // stale mapping → Kalman fallback
// A leader tolerates this much continuous playout divergence before handing off
static constexpr int64_t LEADER_UNHEALTHY_US = 5000000;
// After stepping down, do not grab leadership again for this long: a device whose
// health flickers would otherwise re-take it seconds later and oscillate
static constexpr int64_t LEAD_COOLDOWN_US = 20000000;
// Playout must be tracking this long before a device is eligible to lead
static constexpr int64_t LEAD_HEALTHY_MIN_US = 2000000;
static constexpr int64_t SERVICE_MIN_INTERVAL_US = 200000;
// Age clamp on TSF extrapolation: an AP reboot resets TSF to ~zero with the BSSID
// unchanged, leaving tsf_base "hours in the future" — evaluating that mapping would
// produce garbage deadlines (guaranteed hard-resync mute) until the next anchor
static constexpr int64_t MAX_EXTRAPOLATION_US = 10000000;
// Sandwich width above which a TSF sample was interrupted mid-read; retry.
// After all retries, the narrowest attempt is still accepted up to the loose
// bound (the servo median absorbs the noise; a drop to Kalman fallback is worse).
// The bracket is dominated by the DETERMINISTIC cost of esp_wifi_get_tsf_time(), not by
// jitter. Measured on four devices: median 42 us, full range 42-49 us. Two things follow.
//
// First, a threshold below ~42 us is unreachable, so demanding one just burns retries: an
// earlier attempt at 20 us with 8 attempts spent ~340 us of blocking work per deadline and
// returned the same best-of value it would have accepted immediately.
//
// Second, the error model behind that attempt was wrong. A CONSISTENT bracket means the TSF
// is latched at a consistent point within the call, so the midpoint carries a consistent
// BIAS -- and identical devices share that bias, so it cancels between them. The noise that
// actually matters is the variation in bracket width, ~7 us peak-to-peak, i.e. a few us.
// TSF read noise is therefore NOT the dominant timing term; it was assumed to be.
static constexpr int64_t SANDWICH_MAX_US = 60;
static constexpr int64_t SANDWICH_LOOSE_MAX_US = 400;
static constexpr int SANDWICH_ATTEMPTS = 3;
// A read wider than this is not allowed to update the offset filter: at a 42 us floor and
// 7 us of spread, anything past here is genuine interference rather than call cost.
static constexpr int64_t SANDWICH_TRUST_US = 80;
// Baseline spacing for the leader's own TSF-vs-esp_timer rate measurement
static constexpr int64_t RATE_WINDOW_US = 4000000;

// The published mapping is slew-limited toward the live Kalman estimate: anchoring
// each beacon to the instantaneous estimate broadcast its sample-to-sample jitter
// (~±100-300 us) as 1 Hz deadline steps that every member's servo then chased
// (observed: trim swinging hundreds of ppm). The slewed line low-passes the jitter.
// Large estimate moves ramp at a faster (but still continuous) rate rather than
// snapping: a snap is a step every member chases clamp-limited and slightly
// time-offset from its peers (observed: ~6 ms snap -> ~2 ms differential for ~10 s),
// while a shared ramp keeps the pair identical throughout. Only implausibly large
// deltas (broken mapping / reconnect re-baseline) snap through.
// Expressed as RATES, scaled by the actual interval between broadcasts. They were
// per-beacon allowances, which silently coupled the tracking speed to
// BEACON_INTERVAL_US: raising the beacon rate to 10 Hz would have turned "50 us/s"
// into 500 us/s with nobody intending it. Using the measured interval also fixes a
// latent bug -- a delayed broadcast (service() rate limiting, task scheduling, a
// missed tick) previously still allowed only one beacon's worth of correction, so the
// published line fell further behind the longer the gap.
static constexpr int64_t TMS_SLEW_MAX_US_PER_S = 50;       // steady state
static constexpr int64_t TMS_SLEW_CATCHUP_US_PER_S = 300;  // once |delta| > 1 ms
static constexpr int64_t TMS_CATCHUP_THRESHOLD_US = 1000;
static constexpr int64_t TMS_SNAP_US = 20000;

// Low-pass on the shared offset: 1/32 over per-chunk calls (~26 ms) is a ~0.8 s time constant,
// cutting uncorrelated TSF read noise by ~sqrt(32). See shared_server_offset_us().
// 1/64 over per-chunk calls (~26 ms) is a ~1.7 s time constant, cutting uncorrelated
// read noise by ~8x. Nearly free in lag terms: the mapping is drift-compensated, so the
// filtered quantity is near-constant rather than a moving signal.
static constexpr double OFFSET_EWMA_ALPHA = 1.0 / 64.0;
// Above this, the raw sample is a real re-anchor (leader change, reconnect), not slew or
// noise: the published mapping moves at most TMS_SLEW_CATCHUP_US_PER_S, and read
// noise is bounded by the sandwich. Snap instead of smearing it in over ~0.8 s.
static constexpr double OFFSET_SNAP_US = 2000.0;

// All ESP32 variants are little-endian; fields are sent raw (no htonl)
struct __attribute__((packed)) TsfPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t bssid[6];
  uint8_t sender_mac[6];
  uint8_t reserved;
  uint32_t server_id_hash;
  int64_t tsf_base_us;
  int64_t tsf_minus_server_us;
  float drift_ppm;  // d(tsf − server)/dt, in TSF units
  // Sender's playout pipeline depth (pushed-but-unplayed), ms; PIPELINE_UNKNOWN
  // before it has played anything. Diagnostics only -- never feeds the timebase.
  int16_t pipeline_ms;
};

TsfSync::~TsfSync() {
  if (this->sock_ >= 0) {
    ::close(this->sock_);
  }
}

bool TsfSync::sample_tsf_(int64_t &tsf_us, int64_t &local_us, int64_t *width_out) {
  // Sandwich the TSF read between esp_timer reads; a wide sandwich means an
  // interrupt (or a slow driver path) landed mid-read and the pairing is noisy.
  // Prefer a tight sandwich, but accept the best attempt up to a looser bound --
  // the servo's median absorbs occasional noisy samples, while a hard failure
  // drops the whole mapping to the Kalman fallback.
  int64_t best_width = INT64_MAX;
  for (int attempt = 0; attempt < SANDWICH_ATTEMPTS; attempt++) {
    const int64_t l1 = esp_timer_get_time();
    const int64_t tsf = esp_wifi_get_tsf_time(WIFI_IF_STA);
    const int64_t l2 = esp_timer_get_time();
    if (tsf == 0) {
      return false;  // radio asleep / not associated; retrying won't help now
    }
    const int64_t width = l2 - l1;
    if (width < best_width) {
      best_width = width;
      tsf_us = tsf;
      local_us = l1 + width / 2;
    }
    if (width <= SANDWICH_MAX_US) {
      if (width_out != nullptr) {
        *width_out = width;
      }
      return true;
    }
  }
  if (width_out != nullptr) {
    *width_out = best_width;
  }
  return best_width <= SANDWICH_LOOSE_MAX_US;
}

TsfSync::EvalResult TsfSync::evaluate_mapping_(int64_t tsf_base_us, int64_t tsf_minus_server_us, float drift_ppm,
                                               int64_t &offset_us, int64_t &extrapolation_us, int64_t *width_out) {
  int64_t tsf_now, local_now;
  extrapolation_us = 0;
  if (!sample_tsf_(tsf_now, local_now, width_out)) {
    return EvalResult::NO_TSF;
  }
  extrapolation_us = tsf_now - tsf_base_us;
  // Small negative skew is legitimate: two stations' TSF free-run on their own
  // crystals between beacons (~us apart), and the sender samples before we do
  if (extrapolation_us < -1000 || extrapolation_us > MAX_EXTRAPOLATION_US) {
    return EvalResult::AGE_CLAMP;  // TSF reset (AP reboot) or mapping far past expiry
  }
  const double tms_now = static_cast<double>(tsf_minus_server_us) +
                         static_cast<double>(drift_ppm) * 1e-6 * static_cast<double>(extrapolation_us);
  const int64_t server_now_us = tsf_now - static_cast<int64_t>(tms_now);
  offset_us = server_now_us - local_now;
  return EvalResult::OK;
}

bool TsfSync::ensure_socket_() {
  if (this->sock_ >= 0 && this->joined_) {
    return true;
  }
  if (this->sock_ < 0) {
    this->sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (this->sock_ < 0) {
      return false;
    }
    int reuse = 1;
    setsockopt(this->sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int flags = fcntl(this->sock_, F_GETFL, 0);
    fcntl(this->sock_, F_SETFL, flags | O_NONBLOCK);
    uint8_t ttl = 1;
    setsockopt(this->sock_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    uint8_t loop = 0;  // never hear our own packets back
    setsockopt(this->sock_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(TSF_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(this->sock_, reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0) {
      ::close(this->sock_);
      this->sock_ = -1;
      return false;
    }
  }
  // Group membership needs a live interface; retried each service tick until it takes
  struct ip_mreq mreq = {};
  mreq.imr_multiaddr.s_addr = inet_addr(TSF_GROUP);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  this->joined_ = setsockopt(this->sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0;
  return this->joined_;
}

void TsfSync::reset_(const char *reason) {
  if (this->role_.load(std::memory_order_relaxed) != Role::IDLE || this->mapping_valid_) {
    ESP_LOGD(TAG, "Reset (%s)", reason);
  }
  this->role_.store(Role::IDLE, std::memory_order_relaxed);
  this->last_rx_us_ = 0;
  this->unhealthy_since_us_ = 0;
  this->tsf_rate_valid_ = false;
  this->rate_ref_local_us_ = 0;
  this->pub_valid_ = false;  // TSF timebase changed; the published line with it
  this->learned_peers_.clear();  // addresses may change with the network
  this->update_peer_count_();
  this->mapping_mutex_.lock();
  this->mapping_valid_ = false;
  this->mapping_mutex_.unlock();
}

void TsfSync::adopt_(int64_t tsf_base_us, int64_t tsf_minus_server_us, float drift_ppm, int64_t local_now_us) {
  this->mapping_mutex_.lock();
  this->mapping_valid_ = true;
  this->map_tsf_base_us_ = tsf_base_us;
  this->map_tsf_minus_server_us_ = tsf_minus_server_us;
  this->map_drift_ppm_ = drift_ppm;
  this->map_updated_local_us_ = local_now_us;
  this->mapping_mutex_.unlock();
}

void TsfSync::receive_(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash) {
  TsfPacket pkt;
  struct sockaddr_in from = {};
  while (true) {
    socklen_t from_len = sizeof(from);
    const ssize_t n =
        recvfrom(this->sock_, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&from), &from_len);
    if (n < static_cast<ssize_t>(sizeof(pkt))) {
      if (n < 0) {
        break;  // drained (EWOULDBLOCK) or error; retried next tick either way
      }
      continue;  // runt packet
    }
    if (pkt.magic != TSF_MAGIC || pkt.version != TSF_VERSION) {
      continue;
    }
    if (memcmp(pkt.sender_mac, this->my_mac_, 6) == 0) {
      continue;  // our own packet (multicast loop disabled, but the AP reflects)
    }
    // Only mappings for our AP's TSF timer apply: a peer on another BSS is a
    // separate TSF island by design (both lead) -- surface it, it usually means
    // the pair should be pinned to one AP with the BSSID select
    if (memcmp(pkt.bssid, this->bssid_, 6) != 0) {
      if (!this->warned_foreign_bss_) {
        this->warned_foreign_bss_ = true;
        ESP_LOGW(TAG,
                 "Peer %02X:%02X:%02X:%02X:%02X:%02X is on another AP (BSSID %02X:%02X:%02X:%02X:%02X:%02X vs our "
                 "%02X:%02X:%02X:%02X:%02X:%02X) - no shared TSF timebase; pin both to one AP to group them",
                 pkt.sender_mac[0], pkt.sender_mac[1], pkt.sender_mac[2], pkt.sender_mac[3], pkt.sender_mac[4],
                 pkt.sender_mac[5], pkt.bssid[0], pkt.bssid[1], pkt.bssid[2], pkt.bssid[3], pkt.bssid[4], pkt.bssid[5],
                 this->bssid_[0], this->bssid_[1], this->bssid_[2], this->bssid_[3], this->bssid_[4], this->bssid_[5]);
      }
      continue;
    }
    if (pkt.server_id_hash != server_id_hash) {
      if (!this->warned_foreign_server_) {
        this->warned_foreign_server_ = true;
        ESP_LOGD(TAG, "Ignoring TSF packets for a different snapserver");
      }
      continue;
    }
    this->rx_peer_count_++;
    // Learn the sender's address for unicast beacons: the server roster can predate
    // a peer's connection (boot race) and multicast may be blocked entirely
    this->learn_peer_(from.sin_addr.s_addr);
    const bool sender_outranks = memcmp(pkt.sender_mac, this->my_mac_, 6) < 0;
    const Role role = this->role_.load(std::memory_order_relaxed);
    if (role == Role::LEADER && !sender_outranks) {
      // We outrank a rival leader: they yield only if they can hear us, and their
      // roster may not include us -- beacon back now (their address just got
      // learned above), rate-limited against reply ping-pong
      if (est.valid && local_now_us - this->last_tx_us_ >= 500000) {
        this->broadcast_(local_now_us, est, server_id_hash);
      }
      continue;
    }
    // Election first, independent of mapping sanity: any valid packet from an
    // outranking sender proves a live leader. Tying this to mapping acceptance
    // made a rejection episode flap roles every few seconds (observed on
    // hardware: yield -> reject -> timeout -> assume -> yield ...).
    if (role == Role::LEADER) {
      ESP_LOGI(TAG, "Yielding leadership to %02X:%02X:%02X:%02X:%02X:%02X", pkt.sender_mac[0], pkt.sender_mac[1],
               pkt.sender_mac[2], pkt.sender_mac[3], pkt.sender_mac[4], pkt.sender_mac[5]);
    } else if (role != Role::FOLLOWER) {
      ESP_LOGI(TAG, "Following TSF leader %02X:%02X:%02X:%02X:%02X:%02X", pkt.sender_mac[0], pkt.sender_mac[1],
               pkt.sender_mac[2], pkt.sender_mac[3], pkt.sender_mac[4], pkt.sender_mac[5]);
    }
    this->role_.store(Role::FOLLOWER, std::memory_order_relaxed);
    memcpy(this->leader_mac_, pkt.sender_mac, 6);
    this->last_rx_us_ = local_now_us;

    // Mapping gates (adoption only; rejected mappings leave the previous one to
    // expire into the Kalman fallback). Plausibility: a mapping farther from our
    // own estimate than the hard-resync threshold is garbage (or the leader's
    // clock is) -- playing to it would hard-resync.
    int64_t implied_offset_us, extrapolation_us;
    const EvalResult ev =
        evaluate_mapping_(pkt.tsf_base_us, pkt.tsf_minus_server_us, pkt.drift_ppm, implied_offset_us,
                          extrapolation_us);
    if (ev != EvalResult::OK) {
      if (!this->warned_rejected_) {
        this->warned_rejected_ = true;
        if (ev == EvalResult::NO_TSF) {
          ESP_LOGD(TAG, "Rejected mapping (TSF unreadable)");
        } else {
          ESP_LOGD(TAG, "Rejected mapping (age clamp: extrapolation %" PRId64 " us)", extrapolation_us);
        }
      }
      continue;
    }
    // Plausibility only means something when our own estimate deserves trust: a
    // freshly-booted follower's raw Kalman swings +-100 ms under the post-reboot
    // congestion, vetoing the (maturity-gated, trustworthy) leader's mapping and
    // churning on its own bad clock instead (observed: a whole fleet rejecting
    // sign-flipping "implausible" deltas for minutes after a simultaneous OTA).
    // Immature followers adopt the leader's mapping unconditionally.
    if (est.valid && est.mature) {
      const int64_t own_offset_us = static_cast<int64_t>(est.offset_ms * 1000.0);
      if (std::abs(implied_offset_us - own_offset_us) > this->plausibility_us_) {
        if (!this->warned_rejected_) {
          this->warned_rejected_ = true;
          ESP_LOGD(TAG, "Rejected mapping (implausible: %+" PRId64 " us vs own estimate)",
                   implied_offset_us - own_offset_us);
        }
        continue;
      }
    }
    this->warned_rejected_ = false;
    this->adopt_(pkt.tsf_base_us, pkt.tsf_minus_server_us, pkt.drift_ppm, local_now_us);
    this->check_pipeline_divergence_(pkt.pipeline_ms, local_now_us);
  }
}

void TsfSync::check_pipeline_divergence_(int16_t leader_pipeline_ms, int64_t local_now_us) {
  const int32_t mine = this->pipeline_ms_.load(std::memory_order_relaxed);
  if (leader_pipeline_ms == PIPELINE_UNKNOWN || mine == INT32_MIN) {
    this->pipeline_delta_ms_.store(INT32_MIN, std::memory_order_relaxed);
    this->pipeline_diverged_since_us_ = 0;
    return;
  }
  const int32_t delta = mine - static_cast<int32_t>(leader_pipeline_ms);
  this->pipeline_delta_ms_.store(delta, std::memory_order_relaxed);

  if (std::abs(delta) < PIPELINE_DIVERGE_MS) {
    this->pipeline_diverged_since_us_ = 0;
    return;
  }
  if (this->pipeline_diverged_since_us_ == 0) {
    this->pipeline_diverged_since_us_ = local_now_us;
    return;
  }
  // Sustained: a transient counts for nothing, since depth legitimately swings while
  // a device refills after a starvation. Only a depth that STAYS wrong is an offset.
  if (local_now_us - this->pipeline_diverged_since_us_ < PIPELINE_DIVERGE_MIN_US) {
    return;
  }
  if (this->last_diverge_log_us_ != 0 && local_now_us - this->last_diverge_log_us_ < PIPELINE_DIVERGE_LOG_US) {
    return;
  }
  this->last_diverge_log_us_ = local_now_us;
  // WARN, not DEBUG: this is inaudible to every other metric we have. The sync
  // report will look perfect while this device plays |delta| ms out from the group.
  ESP_LOGW(TAG, "Playout depth %+" PRId32 " ms vs leader (%" PRId32 " vs %d ms) for %.0f s: "
                "audio is likely offset by about this much, sync reports notwithstanding",
           delta, mine, static_cast<int>(leader_pipeline_ms),
           (local_now_us - this->pipeline_diverged_since_us_) / 1e6);
}

void TsfSync::broadcast_(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash) {
  int64_t tsf_now, local_mid;
  if (!sample_tsf_(tsf_now, local_mid)) {
    return;
  }

  // Track our own TSF-vs-esp_timer rate: the published drift must be in TSF units,
  // and the AP-vs-our-crystal difference usually dominates the Kalman drift
  if (this->rate_ref_local_us_ == 0) {
    this->rate_ref_tsf_us_ = tsf_now;
    this->rate_ref_local_us_ = local_mid;
  } else if (local_mid - this->rate_ref_local_us_ >= RATE_WINDOW_US) {
    const double dl = static_cast<double>(local_mid - this->rate_ref_local_us_);
    const double dt = static_cast<double>(tsf_now - this->rate_ref_tsf_us_);
    const float measured_ppm = static_cast<float>((dt - dl) / dl * 1e6);
    // Sanity: crystals differ by well under ±100 ppm; larger = TSF discontinuity
    if (std::fabs(measured_ppm) < 100.0f) {
      this->tsf_rate_ppm_ = this->tsf_rate_valid_ ? 0.5f * this->tsf_rate_ppm_ + 0.5f * measured_ppm : measured_ppm;
      this->tsf_rate_valid_ = true;
    }
    this->rate_ref_tsf_us_ = tsf_now;
    this->rate_ref_local_us_ = local_mid;
  }

  // server_now at the sandwich midpoint from our Kalman estimate
  const int64_t server_now_us =
      local_mid + static_cast<int64_t>((est.offset_ms + est.drift * ((local_mid - local_now_us) / 1000.0)) * 1000.0);
  // d(tsf−server)/dt = d(tsf−local)/dt + d(local−server)/dt = tsf_rate − kalman_drift
  const float drift_ppm = (this->tsf_rate_valid_ ? this->tsf_rate_ppm_ : 0.0f) - static_cast<float>(est.drift * 1e6);

  // Slew-limit the published line toward the live estimate (see TMS_SLEW_MAX_US_PER_S)
  const int64_t tms_target = tsf_now - server_now_us;
  int64_t tms_pub = tms_target;
  if (this->pub_valid_) {
    const int64_t elapsed_us = tsf_now - this->pub_tsf_base_;
    const int64_t tms_expected =
        this->pub_tms_base_ +
        static_cast<int64_t>(static_cast<double>(this->pub_drift_ppm_) * 1e-6 * static_cast<double>(elapsed_us));
    const int64_t delta = tms_target - tms_expected;
    if (std::abs(delta) <= TMS_SNAP_US) {
      const int64_t rate_us_per_s =
          std::abs(delta) > TMS_CATCHUP_THRESHOLD_US ? TMS_SLEW_CATCHUP_US_PER_S : TMS_SLEW_MAX_US_PER_S;
      // At least 1 us of authority, so a very short interval cannot stall tracking entirely
      const int64_t slew = std::max<int64_t>(1, rate_us_per_s * std::max<int64_t>(0, elapsed_us) / 1000000);
      tms_pub = tms_expected + std::clamp<int64_t>(delta, -slew, slew);
    }
  }
  this->pub_valid_ = true;
  this->pub_tsf_base_ = tsf_now;
  this->pub_tms_base_ = tms_pub;
  this->pub_drift_ppm_ = drift_ppm;

  TsfPacket pkt = {};
  pkt.magic = TSF_MAGIC;
  pkt.version = TSF_VERSION;
  memcpy(pkt.bssid, this->bssid_, 6);
  memcpy(pkt.sender_mac, this->my_mac_, 6);
  pkt.server_id_hash = server_id_hash;
  pkt.tsf_base_us = tsf_now;
  pkt.tsf_minus_server_us = tms_pub;
  pkt.drift_ppm = drift_ppm;
  {
    const int32_t depth = this->pipeline_ms_.load(std::memory_order_relaxed);
    pkt.pipeline_ms = (depth == INT32_MIN || depth < INT16_MIN + 1 || depth > INT16_MAX)
                          ? PIPELINE_UNKNOWN
                          : static_cast<int16_t>(depth);
  }
  struct sockaddr_in dest = {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(TSF_PORT);
  dest.sin_addr.s_addr = inet_addr(TSF_GROUP);
  if (sendto(this->sock_, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest)) < 0 &&
      !this->warned_tx_) {
    this->warned_tx_ = true;
    ESP_LOGW(TAG, "Beacon multicast send failed: errno %d", errno);
  }
  // Unicast to every rostered peer: client-to-client multicast is unreliable on
  // many APs, unicast works wherever snapcast itself does. Own address may be on
  // the roster; the AP hands the packet back and the own-mac check drops it.
  for (const uint32_t addr : this->peers_) {
    dest.sin_addr.s_addr = addr;
    sendto(this->sock_, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
  }
  for (const uint32_t addr : this->learned_peers_) {
    dest.sin_addr.s_addr = addr;
    sendto(this->sock_, &pkt, sizeof(pkt), 0, reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
  }
  this->last_tx_us_ = local_now_us;
  // The leader plays from its own published mapping so everyone quantizes alike
  this->adopt_(pkt.tsf_base_us, pkt.tsf_minus_server_us, pkt.drift_ppm, local_now_us);
}

void TsfSync::service(int64_t local_now_us, const Estimate &est, uint32_t server_id_hash) {
  const int64_t since_last_service = local_now_us - this->last_service_us_;
  if (since_last_service < SERVICE_MIN_INTERVAL_US) {
    return;
  }
  this->last_service_us_ = local_now_us;
  if (since_last_service > 3 * BEACON_INTERVAL_US && this->last_rx_us_ != 0) {
    // Resuming after an idle gap (service only runs while a stream is active):
    // beacons were legitimately absent, so give the known leader a fresh timeout
    // window instead of seizing leadership before its first resumed beacon lands
    this->last_rx_us_ = local_now_us;
  }

  if (!this->have_mac_) {
    this->have_mac_ = esp_wifi_get_mac(WIFI_IF_STA, this->my_mac_) == ESP_OK;
    if (!this->have_mac_) {
      return;
    }
  }

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    if (this->have_bssid_) {
      this->have_bssid_ = false;
      this->reset_("disassociated");
    }
    return;
  }
  if (!this->have_bssid_ || memcmp(ap.bssid, this->bssid_, 6) != 0) {
    // New BSS: its TSF timer is unrelated to the previous one's
    if (this->have_bssid_) {
      this->reset_("BSSID changed");
    }
    memcpy(this->bssid_, ap.bssid, 6);
    this->have_bssid_ = true;
  }

  if (!this->ensure_socket_()) {
    return;
  }

  this->receive_(local_now_us, est, server_id_hash);

  const bool healthy = this->playout_healthy_.load(std::memory_order_relaxed);
  const Role role = this->role_.load(std::memory_order_relaxed);
  if (role == Role::LEADER) {
    if (!est.valid || !est.mature) {
      // Never publish an unsettled estimate (e.g. after a reconnect restarts the
      // time-sync burst): a mature peer takes over, or leadership resumes once
      // our estimate settles
      this->reset_("estimate not settled");
      return;
    }
    // Step down if our own playout has been off the timebase for a while: the
    // group should not follow a mapping published by the device that is itself
    // out of sync. Brief excursions are tolerated (a leader recovering from a
    // hiccup would otherwise hand off constantly).
    if (healthy) {
      this->unhealthy_since_us_ = 0;
    } else {
      if (this->unhealthy_since_us_ == 0) {
        this->unhealthy_since_us_ = local_now_us;
      } else if (local_now_us - this->unhealthy_since_us_ >= LEADER_UNHEALTHY_US) {
        this->reset_("own playout unsynced, stepping down");
        this->no_lead_until_us_ = local_now_us + LEAD_COOLDOWN_US;
        return;
      }
    }
    if (local_now_us - this->last_tx_us_ >= BEACON_INTERVAL_US) {
      this->broadcast_(local_now_us, est, server_id_hash);
    }
    return;
  }

  // Takeover: silence beyond the timeout plus a per-MAC stagger (lower MACs move
  // first, so the winner usually claims before anyone else's timer fires). Only a
  // settled estimate may lead: a raw one converges in 100+ ms steps that would be
  // broadcast as mapping snaps, dragging every follower through hard resyncs.
  const int64_t stagger = static_cast<int64_t>(this->my_mac_[5] & 0x0F) * STAGGER_STEP_US;
  const bool silence =
      this->last_rx_us_ == 0 || (local_now_us - this->last_rx_us_) > LEADER_TIMEOUT_US + stagger;
  // Only a device whose own playout has been tracking for a while may publish the
  // group timebase, and not immediately after having stepped down
  if (healthy) {
    if (this->healthy_since_us_ == 0) {
      this->healthy_since_us_ = local_now_us;
    }
  } else {
    this->healthy_since_us_ = 0;
  }
  const bool eligible = est.valid && est.mature && this->healthy_since_us_ != 0 &&
                        local_now_us - this->healthy_since_us_ >= LEAD_HEALTHY_MIN_US &&
                        local_now_us >= this->no_lead_until_us_;
  if (silence && eligible) {
    ESP_LOGI(TAG, "Assuming TSF leadership");
    // Continuity: if we were following, keep publishing the line the group is
    // ALREADY playing to and let the slew walk it toward our own estimate. A new
    // leader that anchored to its own fresh estimate stepped every follower's
    // deadline, which resynced them, which made them unhealthy -- observed as
    // leadership bouncing d->c->b->b->c->a in 50 s with re-locks throughout.
    this->seed_published_from_mapping_();
    this->role_.store(Role::LEADER, std::memory_order_relaxed);
    this->broadcast_(local_now_us, est, server_id_hash);
  }
}

void TsfSync::seed_published_from_mapping_() {
  this->mapping_mutex_.lock();
  if (this->mapping_valid_) {
    this->pub_valid_ = true;
    this->pub_tsf_base_ = this->map_tsf_base_us_;
    this->pub_tms_base_ = this->map_tsf_minus_server_us_;
    this->pub_drift_ppm_ = this->map_drift_ppm_;
  }
  this->mapping_mutex_.unlock();
}

void TsfSync::set_peers(std::vector<uint32_t> peer_addrs) {
  if (peer_addrs.size() > 16) {
    peer_addrs.resize(16);
  }
  if (peer_addrs != this->peers_) {
    ESP_LOGD(TAG, "Unicast peer roster: %zu entries", peer_addrs.size());
  }
  this->peers_ = std::move(peer_addrs);
  // Drop learned entries the roster now covers (avoids double-sends)
  this->learned_peers_.erase(std::remove_if(this->learned_peers_.begin(), this->learned_peers_.end(),
                                            [this](uint32_t a) {
                                              return std::find(this->peers_.begin(), this->peers_.end(), a) !=
                                                     this->peers_.end();
                                            }),
                             this->learned_peers_.end());
  this->update_peer_count_();
}

void TsfSync::learn_peer_(uint32_t addr) {
  if (std::find(this->peers_.begin(), this->peers_.end(), addr) != this->peers_.end() ||
      std::find(this->learned_peers_.begin(), this->learned_peers_.end(), addr) != this->learned_peers_.end() ||
      this->learned_peers_.size() >= 16) {
    return;
  }
  this->learned_peers_.push_back(addr);
  this->update_peer_count_();
}

// THREAD CONTEXT: player task
bool TsfSync::shared_server_offset_us(int64_t local_now_us, int64_t &offset_us) {
  this->mapping_mutex_.lock();
  const bool valid = this->mapping_valid_ && (local_now_us - this->map_updated_local_us_) <= MAPPING_EXPIRY_US;
  const int64_t tsf_base = this->map_tsf_base_us_;
  const int64_t tms_base = this->map_tsf_minus_server_us_;
  const float drift_ppm = this->map_drift_ppm_;
  this->mapping_mutex_.unlock();
  if (!valid) {
    this->offset_filter_valid_ = false;
    return false;
  }
  int64_t extrapolation_us;
  int64_t raw_us;
  int64_t sandwich_us = 0;
  if (evaluate_mapping_(tsf_base, tms_base, drift_ppm, raw_us, extrapolation_us, &sandwich_us) != EvalResult::OK) {
    this->offset_filter_valid_ = false;
    return false;
  }

  // Low-pass the offset. Every call takes a FRESH sandwiched TSF sample, so its read noise --
  // up to SANDWICH_MAX_US, and uncorrelated between devices, therefore NOT cancelled by sharing
  // the mapping -- lands directly in each chunk's deadline. Measured on four clients: the
  // per-chunk sync error is white noise (consecutive-difference sigma / sigma = 1.32-1.43,
  // versus sqrt(2) = 1.41 for pure white noise) at ~270 us, which is an order of magnitude
  // coarser than stereo imaging tolerates while being perfectly adequate room-to-room.
  //
  // Filtering costs almost no lag because the quantity is already drift-compensated: the
  // published mapping carries drift_ppm and evaluate_mapping_ extrapolates with it, so what
  // remains should be near-constant and any real movement is ppm-scale. Reset on a new mapping
  // so a leader change or re-anchor steps through immediately instead of being smeared.
  // Snap on a genuine step, filter otherwise. NOT keyed on adopt_: that runs once per
  // beacon, so re-anchoring there would reset the filter every second and filter nothing.
  // Consecutive mappings differ by at most the slew rate times the interval, so anything
  // this large is a leader change or a re-anchor, not slew and not read noise.
  if (!this->offset_filter_valid_ ||
      std::abs(static_cast<double>(raw_us) - this->offset_filter_us_) > OFFSET_SNAP_US) {
    this->offset_filter_us_ = static_cast<double>(raw_us);
    this->offset_filter_valid_ = true;
  } else if (sandwich_us <= SANDWICH_TRUST_US) {
    // Only narrow reads move the filter. A wide bracket is mostly interrupt latency, and
    // averaging it in would spend filter authority tracking noise we already know is bad.
    this->offset_filter_us_ += OFFSET_EWMA_ALPHA * (static_cast<double>(raw_us) - this->offset_filter_us_);
  }
  offset_us = static_cast<int64_t>(this->offset_filter_us_);
  return true;
}

float TsfSync::mapping_age_s(int64_t local_now_us) {
  this->mapping_mutex_.lock();
  const bool valid = this->mapping_valid_;
  const int64_t updated = this->map_updated_local_us_;
  this->mapping_mutex_.unlock();
  if (!valid) {
    return -1.0f;
  }
  return static_cast<float>(local_now_us - updated) * 1e-6f;
}

}  // namespace esphome::snapclient

#endif  // SNAPCLIENT_TSF_ACTIVE
