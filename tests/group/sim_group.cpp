// TWO-BOARD simulator for the timing engine.
//
//   tests/group/run.sh
//
// tests/timing/ simulates ONE board against its own deadline, which cannot express the difference
// between "600 us off the server" and "600 us off my neighbour". Those need opposite responses --
// only a differential error is audible, and correcting a common one moves the pair apart -- so
// every property in that suite can pass while the group behaviour is wrong. It did.
//
// Here: N boards, each with its own plant rate, all measuring against a SHARED deadline that
// drifts (the server's clock offset), each publishing a render phase and receiving the group's
// delta the way clock_sync/tsf_sync computes it (mean including self, so the engine unhalves by
// n/(n-1)). The quantity that matters is the SKEW BETWEEN BOARDS, which is what the logic analyser
// measures on the bench and what a listener hears.
#include "../../components/snapclient/timing_engine.h"
#include "../../components/clock_sync/consensus_math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

using namespace esphome::snapclient::timing;

// The real intervals, from clock_sync/tsf_sync: beacons at 1 Hz, phases paired only within
// 300 ms, a kept delta expiring at 15 s.
constexpr int64_t BEACON_INTERVAL_US = 1000000;
constexpr int64_t PHASE_PAIR_WINDOW_US = 300000;
constexpr int64_t PHASE_STALE_US = 15000000;
constexpr int64_t GROUP_DELTA_STALE_US = 15000000;
// The engine clamps its crystal estimate at +-200 ppm. "Near the rail" is the failure this checks.
constexpr double CRYSTAL_RAIL_PPM = 150.0;
static long gd_ticks = 0, gd_present_ticks = 0, gd_fresh_ticks = 0;
static double gd_age_sum_us = 0.0;

namespace {

int failures = 0;

void check(bool ok, const char *what, double got = 0.0, double want = 0.0) {
  if (ok) printf("  ok    %s\n", what);
  else { printf("  FAIL  %s  (got %.1f, want %.1f)\n", what, got, want); failures++; }
}

struct Board {
  Engine eng;
  double err_us = 0.0;      // own render instant minus own deadline
  double plant_ppm = 0.0;
  std::deque<std::pair<int64_t,double>> obs;   // measurement lag
  std::vector<std::pair<int64_t,int32_t>> inflight;  // position actuator lag
  uint64_t pid = 0;
  bool live = false;
  int corrections = 0;
  int resyncs = 0;          ///< whole-chunk repairs taken by THIS board
  int resync_run = 0;       ///< consecutive same-direction samples past the threshold
  int resync_dir = 0;
  /// Repair already applied but not yet visible in the lagged measurement.
  double resync_pending_us = 0.0;
  int64_t resync_pending_until_us = 0;
  int64_t transient_end_us = -1;     ///< when this board's own transient last ended
  int64_t meas_at_us = 0;            ///< instant of the snapshot this evaluation is reading
  int64_t resync_confirm_at_us = -1; ///< snapshot the last confirmation was counted from
  long gross_frames = 0;
  double phase_us = 0.0;    // what it publishes
  int64_t phase_at_us = 0;  // and when it sampled it -- pairing needs both
  int64_t last_beacon_us = -1000000;
  /// What peers last told us about THEIR common error, and when. Carried on the same beacon as
  /// the phase and subject to the same staleness -- it is a measurement with an age like any
  /// other, and treating a stale one as current is how a shared value stops being shared.
  double heard_common_us[4] = {};
  int64_t heard_common_at_us[4] = {};
  bool heard_common_valid[4] = {};
  double heard_phase_us[4] = {};
  int64_t heard_at_us[4] = {};
  bool heard_valid[4] = {};
  int64_t last_gd_us = 0;
  int64_t last_gd_at_us = 0;
  double own_deadline_us = 0.0;   // this board's own deadline steps (re-anchors)
  /// AUDIO QUEUED AHEAD OF THE DAC, and the reason the starvation guard exists. This used to be
  /// the constant 1724000 handed to every Observation, so the buffer was always full, the guard's
  /// `obs.buffer_us < buffer_floor_us` was never true, and NO buffer-gated behaviour in the engine
  /// was reachable from this simulator -- including the guard that had been there all along.
  /// Starvation drains it 1:1 with the clock (nothing is arriving); recovery refills it faster
  /// than realtime, because the server keeps sending and the backlog lands; and a position
  /// correction is SPENT from it, which is the drain-and-starve spiral the engine documents.
  double buffer_us = 1724000.0;
  /// Mass-balance accounting, mirroring the firmware's MASSBAL counters so one analysis grades
  /// both. in - out - disc must track the change in buffer_us, or a path is uncounted.
  double mb_in_us = 0.0, mb_out_us = 0.0, mb_disc_us = 0.0;
  /// Audio the server sent while we could not receive it. An outage queues audio, it does not
  /// destroy it, and the queue is the only thing that may then arrive faster than realtime.
  double backlog_us = 0.0;
  /// Phase publication gating, as the client does it. Before this the simulator published
  /// unconditionally, so a board declining to publish -- the whole mechanism behind 4e68870 --
  /// could not be represented, let alone tested.
  int64_t phase_transient_until_us = 0;
  bool phase_publishing = true;
  /// The last decision, retained so the per-tick CSV row can carry BOTH boards. The wire's
  /// d(rate)/dt is what shows the ^/V excursions, and a skew-only dump cannot express it.
  double last_rate_ppm = 0.0;
  double last_xtal_ppm = 0.0;
  int32_t last_gd_snap_us = 0;
  /// The split is only computed when the group supplies a differential. Without this flag the CSV
  /// wrote a 0 for "not computed", indistinguishable from a genuine zero error -- 340731 of the
  /// 388786 rows, every one of which would have been averaged in as a measurement.
  /// When a position correction last LANDED. The bench shows the board's own landed step reappearing
  /// as a phantom differential -- gd read 25.0 ms against a 1136-frame (25.76 ms) correction -- because
  /// phase is derived from err_us and a peer's beaconed phase is up to a second stale. Recorded so the
  /// same signature can be looked for here rather than argued about.
  int64_t last_land_us = 0;
  int64_t last_common_c_us = 0;      ///< last consensus formed, held while fresh
  int64_t last_common_c_at_us = 0;
  double last_pc_ppm = 0.0;          ///< the shared common-mode term the engine applied
  bool last_common_valid = false;    ///< whether the group could form a consensus at all
  bool last_e_split_valid = false;
  int32_t last_e_common_us = 0;
  int32_t last_e_diff_us = 0;
  explicit Board(const Profile &p) : eng(p) {}
};

/// Per-tick skew dump, opened from SIM_SKEW_CSV. Null unless asked for, so the suite is unchanged
/// by default. g_scenario labels the rows so one file holds every scenario.
static FILE *g_skew_csv = nullptr;
static const char *g_scenario = "sim";

struct Result {
  double skew_med = 0.0, skew_p90 = 0.0, skew_max = 0.0;
  int resync_a = 0, resync_b = 0;   ///< whole-chunk repairs per board; unequal counts break the pair
  int corr = 0;
  long gross = 0;
  double err_med = 0.0;
  // OSCILLATION. skew_med/p90 are order statistics: they cannot tell a loop RINGING at +-50 us
  // from one sitting quietly at 50 us, and the bench does the first while this simulator reported
  // the second and passed. The rate loop is fully closed here (err_us integrates plant - rate_ppm
  // every tick, against the real timing_engine.cpp), so a limit cycle IS simulated -- it was only
  // ever invisible, because the series was stored as |skew| and a sign is what a crossing needs.
  double period_s = 0.0;    // median interval between upward crossings of the median
  /// Median p2p and sd within successive 120 s windows -- the SAME statistic the bench is graded
  /// on, so a sim number and a wire number are comparable rather than merely similar.
  double p2p_med = 0.0, sd_med = 0.0;
  int p2p_windows = 0;
  double amp_p05 = 0.0;     // signed, so a biased ring is distinguishable from a centred one
  double amp_p95 = 0.0;
  int n_periods = 0;        // fewer than ~6 and the period is not evidence (bench: 5 was not)
  // CRYSTAL WIND-UP. The integral cannot tell "my oscillator runs fast" from "I am behind because
  // the audio did not arrive" -- both are a persistent one-signed error. On the bench 2026-09-02 a
  // starved board wound a hand-zeroed crystal to +192 ppm in twelve minutes against a true +46.
  // Reported per board because the whole point is that it should stay near the PLANT rate.
  double xtal_a = 0.0, xtal_b = 0.0;
  /// THE TOTAL COMMANDED RATE, which is the quantity that must always equal plant + common
  /// whatever the shared correction does. The crystal alone does NOT: if pc_term supplies the
  /// common part, the integral is correct to settle at plant instead. Scoring the crystal against
  /// a fixed target across a gain sweep would therefore mark the mechanism working as a failure.
  double rate_a = 0.0, rate_b = 0.0;
  /// The shared correction actually applied, and how often the group could form one at all.
  double pc_a = 0.0, pc_b = 0.0;
  /// Mean |e_diff| within 2 s of this board's own correction landing, versus everywhere else.
  /// The bench's phantom signature: gd reading the board's own step back as a differential.
  double ediff_after_land = 0.0, ediff_baseline = 0.0;
  long ediff_after_n = 0;
  double ediff_after_flagged = 0.0;  ///< fraction of phantom samples the transient flag catches
  /// Peak |e_common| seen after settling. The drain exists to hold this under the 50 ms resync
  /// threshold; above it the cascade (solo repair -> broken pair -> phantom -> P excursion) starts.
  double common_max = 0.0;
  /// Mean sigma_e and mean gd_sigma, as the engine forms them. Their RATIO decides whether sizing
  /// Kp from the differential raises or lowers the gain, and it is inverted between here and the
  /// bench -- which is why 3h reported the opposite sign to the hardware sweep.
  double sigma_e_mean = 0.0, gd_sigma_mean = 0.0;
  /// Peak sigma_e. A measurement JUMP is counted as noise by the estimator, so this is what
  /// collapses Kp -- the bench saw sig=30590 us and kp=0.000 at the moment it mattered.
  double sigma_e_max = 0.0;
  /// Fraction of decisions with P pinned at the authority clamp, and the largest |P| seen.
  double p_sat_frac = 0.0, p_max_ppm = 0.0;
  /// Corrections taken AFTER the phase fault ended, within two filter lengths -- paid out of a
  /// filter still unwinding a phantom rather than out of any error present now.
  int decay_corr = 0;
  long decay_frames = 0;
  /// Corrections split by which signal drove them, as esrc reports on the bench.
  int corr_from_diff = 0, corr_from_dl = 0;
  double common_valid_frac = 0.0;
  /// Mass balance, per board: what arrived, what the DAC consumed, what the correction threw away,
  /// and where the buffer started and ended. The identity in - out - disc == dbuffer holds only
  /// while the buffer is UNCLAMPED -- at 0 the DAC cannot consume what is not there, and at
  /// nominal the arriving backlog is dropped -- so a mismatch is the clamp, not a lost path.
  double mb_in_a = 0.0, mb_out_a = 0.0, mb_disc_a = 0.0, buf_end_a = 0.0;
  double mb_in_b = 0.0, mb_out_b = 0.0, mb_disc_b = 0.0, buf_end_b = 0.0;
  double buf_start = 0.0;
  /// Mean |pc_a - pc_b|: the differential the "shared" correction actually injects. Must be ~0.
  double pc_absdiff = 0.0;
  double common_one_sided_frac = 0.0;  ///< exactly one board held a consensus
  double common_both_frac = 0.0;       ///< both did, which is the only case that is truly shared
  /// FILTER SNAPS. A confirmed jump ASSIGNS the filter rather than stepping it, which is a
  /// discontinuity in the signal P is computed from -- at Kp ~ 0.45 ppm/us a 45 us snap is ~20 ppm
  /// of commanded rate arriving in one decision. The bench shows full 2 pi cycles in d(rate)/dt at
  /// >20 ppm, one board at a time, sometimes with a doubled top; the suspicion is that these are
  /// snaps out and back. Counted here to see whether the simulator produces them at all.
  int gd_snaps = 0;
  double gd_snap_abs_sum = 0.0;
  int gd_snap_max = 0;
};

/// Everything below `noise_us` models something the bench does that this simulator did not, and
/// that the engine's tests therefore could not see:
///
///   blind_fraction      Observations that never arrive. The bench runs at NoEvidence=51/96 on
///                       board a -- blind more than half the time -- while every test here fed a
///                       valid sample every tick.
///   resync_period_s     UNANNOUNCED audio moves. The legacy hard-resync path drops chunks and
///                       inserts silence, and it does not tell the engine: grep shows the client
///                       calls into the engine from five places and none is in that path. So the
///                       engine's position model is wrong by however far a resync moved the audio,
///                       and it compensates only for corrections it issued itself. On board a
///                       these fire every ~80 s.
///   resync_us           How far one of those moves the audio.
struct Faults {
  double blind_fraction = 0.0;
  double resync_period_s = 0.0;
  double resync_us = 0.0;
  /// Chunk the repair is quantised to; 0 = the client's ~26 ms. The repair cannot null the error,
  /// only step it by whole chunks, which is what makes the residual board-dependent.
  double resync_chunk_us = 0.0;
  /// A single-sample measurement error, injected on board A only, every glitch_period_s. The
  /// audio does NOT move -- only the reading is wrong.
  double glitch_period_s = 0.0;
  double glitch_us = 0.0;
  /// How long the bad reading persists; 0 = one publish interval, i.e. a single bad snapshot
  /// served to every evaluation inside it.
  double glitch_span_s = 0.0;
  /// Offset added to a board's PUBLISHED phase while it is in its own transient (steady=0). The
  /// audio does not move; only the phase the group reads is wrong.
  double phase_fault_us = 0.0;
  /// A transient that moves NO audio -- acquisition, not a correction or a starvation. Board 0
  /// only. Needed because the other two arms are disturbances that swamp any measurement.
  double transient_period_s = 0.0;
  double transient_secs = 0.0;
  /// Board 1 stops publishing its phase, so board 0 has NO differential and position falls back to
  /// the deadline error. The bench's 1491-frame step happened in exactly that state (dif=n/a).
  double peer_silent_period_s = 0.0;
  double peer_silent_secs = 0.0;
  /// Depth-snapshot cadence; 0 = 50000 us, the sink's DMA buffer on this bench.
  int64_t publish_interval_us = 0;
  /// Count confirmations per DISTINCT SNAPSHOT rather than per evaluation. False reproduces the
  /// gate's first, ineffective form.
  bool confirm_distinct = false;
  /// Consecutive same-direction samples past the threshold before the repair may act; 0 = act on
  /// one sample, which is what the client did before 2026-09-03.
  int resync_confirm = 0;
  /// Test the repair against the lagged MEASUREMENT (as the client does) rather than the truth.
  /// Required for any measurement fault to reach the repair at all. See the note at its use.
  bool resync_on_measurement = false;
  /// Whether the resync path tells the engine what it did. The bench did not, until
  /// note_external_move() existed.
  bool announce_resync = false;

  /// THE ROOT DISTURBANCES. Without one of these the loop never leaves microseconds, the resync
  /// threshold is never reached, and everything downstream of it is untestable -- which is why
  /// this simulator disagreed with the bench by three orders of magnitude.
  ///
  /// starve: the ring drains and the board cannot render on time, so it falls behind in REAL
  /// TIME -- the error grows 1:1 with the clock, not with a rate error. The bench shows board a
  /// entering the ring-low band 65 times in a session, gap median 119 s, and
  /// "RSYNC ... ring=26 drops=1" with the error climbing 3 ms per report.
  double starve_period_s = 0.0;
  double starve_secs = 0.0;
  /// BURSTY DELIVERY, which is what the bench actually does and what a binary starve cannot
  /// express. Measured 2026-09-02 22:46 on board a: ARRGAP mean 25725-26954 us against a
  /// break-even 26000 -- enough audio ON AVERAGE -- with maxima of 261-862 ms and 4-12 gaps past
  /// 120 ms in every 10 s window. The ring drains during the stalls and refills between them, so
  /// it can reach the starvation floor without the mean ever looking deficient and without any
  /// single chunk missing its deadline.
  ///
  /// burst_stall_s / burst_period_s: audio stops for the first stall of each period, then arrives
  /// faster than realtime to catch up. Distinct from starve_*, which models a plain outage.
  double burst_period_s = 0.0;
  double burst_stall_s = 0.0;
  /// reanchor: the timebase moves under the board, so its own deadline steps. Not a plant move
  /// and not something rate can answer. The bench logs "Consensus over 3 estimate(s): spread
  /// 428637 us" around exactly the events that precede a storm.
  double reanchor_period_s = 0.0;
  double reanchor_us = 0.0;
  /// How far apart two devices adopt the SAME step. Beacons are 1 Hz, so a second is the natural
  /// scale; this is the whole disturbance, because the step itself is common.
  double reanchor_stagger_s = 1.0;
};

/// `common_ppm` drifts every board's deadline together -- a server/timebase offset, which is what
/// the bench sees: gd of tens of us while each board sits hundreds of us off its own deadline.
/// `label` is REQUIRED, not a global set by hand at each block: g_scenario was declared with a
/// comment saying it "labels the rows so one file holds every scenario" and then never assigned,
/// so all 388k rows of the dump read "sim" and every per-scenario statistic taken from the file
/// silently mixed the clean runs with the starvation ones. A parameter cannot be forgotten.
Result simulate(const char *label, Profile p, double plant_a, double plant_b, double common_ppm,
                double noise_us, double seconds, int64_t true_land_us, Faults f = {}) {
  g_scenario = label;
  std::vector<Board> b;
  b.emplace_back(p);
  b.emplace_back(p);
  b[0].plant_ppm = plant_a;
  b[1].plant_ppm = plant_b;
  std::mt19937 rng(5);
  std::normal_distribution<double> nd(0.0, noise_us);
  const int64_t tick = 25000;
  std::vector<double> skews, errs;
  std::vector<double> skews_signed, skew_t_us;
  int snap_n = 0, snap_max = 0;
  double snap_sum = 0.0;
  gd_ticks = 0; gd_present_ticks = 0; gd_fresh_ticks = 0; gd_age_sum_us = 0.0;
  double deadline_shift = 0.0;   // common-mode: moves both deadlines together
  long decay_corr = 0, decay_frames = 0, corr_from_diff = 0, corr_from_dl = 0;
  long psat_n = 0, psat_hit = 0;
  double psat_max = 0.0;
  double sig_e_sum = 0.0, sig_g_sum = 0.0, sig_e_max = 0.0;
  long sig_n = 0;
  double common_max_us = 0.0;
  double ph_after_sum = 0.0, ph_base_sum = 0.0;
  long ph_after_n = 0, ph_base_n = 0, ph_after_flagged = 0;
  double rate_sum_a = 0.0, rate_sum_b = 0.0, pc_sum_a = 0.0, pc_sum_b = 0.0;
  long settled_n = 0, common_valid_n = 0, common_one_sided_n = 0, both_valid_n = 0;
  double pc_absdiff_sum = 0.0;

  for (int64_t t = 0; t < static_cast<int64_t>(seconds * 1e6); t += tick) {
    deadline_shift += common_ppm * 1e-6 * static_cast<double>(tick);

    // BEACONS, not telepathy. Every board learns its peers' phases only from beacons, and this
    // simulator previously read them out of the other objects instantly, every 25 ms tick, with
    // the set always complete. The real path is 1 Hz beacons, a 300 us... 300 MS pairing window
    // that the code's own comment says "leaves 0-2 contributors 94% of the time", and INT32_MIN
    // when nothing pairs. Both actuators now steer on the differential, so its cadence and
    // availability are load-bearing -- and were the one thing here that was free.
    for (size_t i = 0; i < b.size(); i++) {
      if (t - b[i].last_beacon_us >= BEACON_INTERVAL_US) {
        b[i].last_beacon_us = t;
        // A BOARD IN TRANSIENT SENDS NO PHASE. Peers then hold their last delta (the firmware's
        // GROUP_DELTA_STALE_US) rather than aligning to audio that is about to move back, which
        // is the whole point of the gate. Modelled by not refreshing what the peers heard.
        if (!b[i].phase_publishing) continue;
        // What the beacon carries: this board's phase AND the instant it was sampled.
        for (size_t j = 0; j < b.size(); j++) {
          if (j == i) continue;
          b[j].heard_phase_us[i] = b[i].phase_us;
          b[j].heard_at_us[i] = t;
          b[j].heard_valid[i] = true;
          // The beacon also carries this board's own common error, when it has one. Absent is
          // sent as absent: a board with no differential has no split, and publishing a 0 for
          // that would drag every peer's consensus toward zero with a non-measurement.
          if (b[i].last_e_split_valid) {
            b[j].heard_common_us[i] = static_cast<double>(b[i].last_e_common_us);
            b[j].heard_common_at_us[i] = t;
            b[j].heard_common_valid[i] = true;
          }
        }
      }
    }

    for (size_t i = 0; i < b.size(); i++) {
      Board &x = b[i];
      for (auto it = x.inflight.begin(); it != x.inflight.end();) {
        if (t >= it->first) { x.err_us -= double(it->second) * double(p.frame_us()); x.last_land_us = t; it = x.inflight.erase(it); }
        else ++it;
      }
      if (x.live && x.inflight.empty()) { x.eng.confirm_position_landed(x.pid, t); x.live = false; }

      // ROOT DISTURBANCE 1: starvation. No audio queued, so the board cannot render on time and
      // falls behind in real time. This is the only mechanism here that grows the error without
      // bound, and it is the one that reaches the resync threshold.
      bool starved = false;
      if (f.starve_period_s > 0.0) {
        const double ph = std::fmod(static_cast<double>(t) / 1e6 + i * 37.0, f.starve_period_s);
        starved = ph < f.starve_secs;
        if (starved) x.err_us += static_cast<double>(tick);   // 1:1 with the clock
      }
      // THE BUFFER, which is what the engine's starvation guard actually reads. Nothing arriving
      // means it drains in real time; once audio resumes the backlog lands faster than realtime,
      // so recovery is quicker than the outage that caused it. Capped at the nominal depth.
      // BURSTY DELIVERY: nothing arrives during the stall, then the backlog lands. Unlike starve_*
      // this does NOT add error directly -- the board keeps playing from the ring, and the error
      // only grows if the ring actually runs out. That is the point: a bursty supply drains the
      // ring without any chunk missing its deadline, which is the case SUPPLY's deadline test
      // cannot see and the case the bench produced.
      bool stalled = false;
      if (f.burst_period_s > 0.0) {
        const double bph = std::fmod(static_cast<double>(t) / 1e6 + i * 11.0, f.burst_period_s);
        stalled = bph < f.burst_stall_s;
      }
      const double nominal_buf_us = 1724000.0;
      // SUPPLY, and the accounting kept beside it. in is what ARRIVED and out is what the DAC
      // consumed, so the two branches below are not just buffer arithmetic: starvation means
      // in = 0 while the DAC keeps consuming, and recovery means the backlog lands at 3x while
      // the DAC still consumes at 1x. Counting `in` unconditionally would have said supply never
      // failed, in the one scenario built to make it fail.
      const double tick_us = static_cast<double>(tick);
      // SUPPLY IS REALTIME PLUS A BACKLOG, not "2x whenever below nominal". The old form keyed
      // recovery to the buffer LEVEL, so the rate surplus dipping the level a few us below
      // nominal re-triggered it every tick: over a 900 s run it manufactured 1349 s of audio and
      // let the nominal clamp swallow the difference. Invisible while nothing counted supply.
      //
      // The server sends at realtime and nothing else, so that is what arrives. An outage does
      // not destroy that audio, it QUEUES it, and the queue is what lands faster than realtime
      // afterwards -- bounded by the backlog, which is finite and which recovery exhausts.
      double in_us = 0.0;
      if (starved || stalled) {
        x.backlog_us += tick_us;    // the server kept sending; none of it reached us
      } else {
        const double catchup = std::min(x.backlog_us, 2.0 * tick_us);
        x.backlog_us -= catchup;
        in_us = tick_us + catchup;
      }
      const double out_us = tick_us;
      x.buffer_us = std::min(nominal_buf_us, std::max(0.0, x.buffer_us + in_us - out_us));
      x.mb_in_us += in_us;
      x.mb_out_us += out_us;
      // Out of audio entirely: now the board really does fall behind in real time.
      if (x.buffer_us <= 0.0) {
        x.err_us += tick_us;
      }
      // THE RATE'S OWN EFFECT ON THE BUFFER, which this model did not have. Until now the buffer
      // was bang-bang -- drain 1:1 when starved, refill at 2x, then sit pinned at nominal -- and
      // the COMMANDED RATE never entered it. So the one thing the buffer exists to integrate,
      // audio-in minus audio-out, could not be expressed: no rate, however wrong, moved it.
      //
      // Derived from this simulator's OWN equilibrium rather than invented. The loop is balanced
      // when rate == plant + common (that is exactly the condition under which the measured error
      // holds still, and it is what 3a scores against), so the surplus consumption is the excess
      // of the commanded rate over it. Positive surplus = the DAC eats faster than supply
      // arrives = the ring drains.
      //
      // This is what makes the mass-balance question askable here at all: the buffer becomes the
      // integral of the loop's rate error, which is what it is on the hardware.
      const double want_ppm = x.plant_ppm + common_ppm;
      const double surplus_ppm = x.last_rate_ppm - want_ppm;
      const double out_extra_us = surplus_ppm * 1e-6 * tick_us;
      x.buffer_us = std::max(0.0, x.buffer_us - out_extra_us);
      x.mb_out_us += out_extra_us;
      // ROOT DISTURBANCE 2: the timebase re-anchors. COMMON-MODE, as the firmware states outright
      // ("every device holding this set steps identically") -- but the devices do not adopt it at
      // the same INSTANT. Beacons arrive once a second and a device steps when its own consensus
      // recomputes, so for up to a beacon interval one board has stepped and the other has not,
      // and during that window they disagree by the FULL step.
      //
      // My first model applied a permanent, per-board-DIFFERENT offset, which injected an
      // ever-growing differential and reported 384 ms of skew for ever. That was the model, not
      // the system -- the same mistake as modelling a resync as opposite shoves.
      if (f.reanchor_period_s > 0.0) {
        const double period = f.reanchor_period_s;
        const double phase = std::fmod(static_cast<double>(t) / 1e6, period);
        // Board i adopts the step stagger_i into the event; before that it holds the old timebase.
        const double stagger = (i == 0) ? 0.0 : f.reanchor_stagger_s;
        const double epoch = std::floor(static_cast<double>(t) / 1e6 / period);
        const double adopted = (phase >= stagger) ? epoch + 1.0 : epoch;
        x.own_deadline_us = adopted * f.reanchor_us;
      }

      // measured against its OWN deadline, which the common-mode term has moved
      const double measured = x.err_us + deadline_shift + x.own_deadline_us;
      // The PUBLISHED phase is noisy too -- it is a measurement, not a truth. On the bench gd
      // reads +8, +21, +42 us between boards that agree, and the gate tests |gd| * unhalve
      // against target_diff_us (20 us), so that noise alone crosses the threshold and
      // declares a purely common error "differential". Publishing it noiseless makes the gate
      // look perfect.
      x.phase_us = measured + nd(rng) * 0.25;
      // PHASE IS WRONG WHILE THE BOARD IS IN ITS OWN TRANSIENT. Modelled as an observed fact, not
      // as a mechanism: steady=0 exists precisely because the board knows its phase does not
      // describe its audio then, and the bench shows it wrong by MILLISECONDS while the wire says
      // the pair is within microseconds.
      //
      // Bench 2026-09-03 03:46, board b, seven minutes post-reboot:
      //     GDIN raw=-8633 gd=+4317 gap=-103188 extrap=+1.42 steady=0
      //     RATEWHY p=-150.00 kp=0.156 dif=-2519 clip=1   <- P PINNED at the authority clamp
      // while the analyser had the pair at p2p 33 us, mean +1.1 us. So dif was false by ~2.5 ms,
      // and neither the 103 ms pairing gap nor the 1.42 us extrapolation accounts for it.
      //
      // Applied to the PUBLISHED phase only. err_us -- the truth -- is untouched, so the audio has
      // not moved and any skew that follows is manufactured by the loop believing a bad phase.
      if (f.phase_fault_us != 0.0 && !x.phase_publishing) {
        x.phase_us += f.phase_fault_us;
      }
      x.phase_at_us = t;
      // A BOARD IN TRANSIENT PUBLISHES NO PHASE, as the client gates it. Armed by a delivered
      // correction and -- since 4e68870 -- by the ring falling below its starvation floor, because
      // a drained ring means this board's audio has been displaced and a peer aligning to it is
      // chasing a target about to move back. The simulator published unconditionally before, so
      // the whole mechanism was unrepresentable here: every board always broadcast, and the client
      // fix could not be verified at all.
      if (x.buffer_us > 0.0 && p.buffer_floor_us > 0 &&
          x.buffer_us < static_cast<double>(p.buffer_floor_us)) {
        x.phase_transient_until_us = t + p.compensation_us();
      }
      // A TRANSIENT THAT MOVES NO AUDIO, which is the bench's actual case and the one this model
      // could not express. Both existing arms -- the ring floor and a delivered correction -- are
      // disturbances that dominate every metric, so a phase fault injected alongside them cannot
      // be measured (3k's first attempt: control p2p 96.9 ms). Board b's bench transient was
      // post-reboot ACQUISITION: it declines to publish, its phase is untrustworthy, and its audio
      // is not being stepped at all. Board 0 only, so the pair stays asymmetric as it was.
      // THE PEER GOES SILENT, so board 0 loses its differential entirely and e_position falls back
      // to the DEADLINE error. Without this the observation glitch cannot reach the position path
      // at all -- position acts on e_diff whenever the group supplies one, and in this simulator it
      // almost always does (gd present 100% of decisions, held when stale). The bench had
      // ESPLIT dif=n/a at the moment it spent 1491 frames, i.e. no differential, which is the
      // condition that let a 33 ms deadline error buy an irreversible step.
      if (f.peer_silent_period_s > 0.0 && i == 1) {
        const double sph = std::fmod(static_cast<double>(t) / 1e6, f.peer_silent_period_s);
        if (sph < f.peer_silent_secs) {
          x.phase_transient_until_us = std::max(x.phase_transient_until_us, t + tick);
        }
      }
      if (f.transient_period_s > 0.0 && i == 0) {
        const double tph = std::fmod(static_cast<double>(t) / 1e6, f.transient_period_s);
        if (tph < f.transient_secs) {
          x.phase_transient_until_us = std::max(x.phase_transient_until_us, t + tick);
        }
      }
      const bool was_publishing = x.phase_publishing;
      x.phase_publishing = (t >= x.phase_transient_until_us);
      // WHEN THE FAULT ENDED. The phantom's damage is not only what it does while present -- the
      // filter is LOADED with it, and then pays it off in irreversible frames as it decays,
      // re-crossing a shrinking coarse gate on the way down. Bench 2026-09-03 04:52, board a,
      // three corrections after the fault had gone: ep -6049 -> -2740 -> -960 us against a raw
      // GDIN gd of +-20, frames -274 -> -124 -> -43, gate 48 -> 46 -> 35.
      if (!was_publishing && x.phase_publishing) x.transient_end_us = t;
      x.obs.emplace_back(t, measured + nd(rng));
      double seen = x.obs.front().second;
      while (x.obs.size() > 1 && x.obs.front().first <= t - p.measurement_lag_us) {
        seen = x.obs.front().second;
        x.obs.pop_front();
      }
      // A MEASUREMENT THAT LIES. Every fault here until now moved real audio (starve, reanchor,
      // resync) or withheld an observation (blind_fraction); none made a reading WRONG while the
      // audio stayed put. That is the bench's actual fault: board a's deadline error stepped
      // +50010 us for a single decision -- one whole DMA buffer plus drift -- while board b, 15.7
      // us away on the wire, never moved, and the repair then dropped 1136 frames of real audio.
      //
      // Injected into the OBSERVATION only. err_us, the truth, is untouched: the audio has not
      // moved, which is the whole point and the reason a confirmation gate can reject it.
      // THE SNAPSHOT'S INSTANT, not the tick's. The publisher runs once per DMA buffer while the
      // repair is evaluated far more often, so several consecutive evaluations read ONE snapshot.
      // Modelling that is what lets this suite tell a confirmation gate that counts DISTINCT
      // MEASUREMENTS from one that counts re-reads -- the first rejects a bad snapshot, the second
      // confirms it three times over and rejects nothing. A one-tick glitch cannot distinguish
      // them, which is why 3g reported success for both before this existed.
      const int64_t pub_us = f.publish_interval_us > 0 ? f.publish_interval_us : 50000;
      x.meas_at_us = (t / pub_us) * pub_us;
      bool glitched = false;
      if (f.glitch_period_s > 0.0 && f.glitch_us != 0.0 && i == 0) {
        const double gph = std::fmod(static_cast<double>(t) / 1e6, f.glitch_period_s);
        // Spans a whole publish interval by default: ONE bad snapshot, read by every evaluation
        // that falls inside it.
        const double span_s = f.glitch_span_s > 0.0 ? f.glitch_span_s
                                                    : static_cast<double>(pub_us) / 1e6;
        glitched = gph < span_s;
        if (glitched) seen += f.glitch_us;
      }

      // PAIR ONLY PHASES SAMPLED AT ROUGHLY THE SAME INSTANT, as recompute_group_delta_ does:
      // these are absolute offsets drifting continuously, so differencing a fresh peer phase
      // against a stale local one measures drift, not skew.
      GroupEvidence g{};
      double sum = 0.0;
      size_t n_contrib = 1;              // self is in the group, hence the halving
      for (size_t j = 0; j < b.size(); j++) {
        if (j == i || !x.heard_valid[j]) continue;
        const int64_t pair_gap = x.heard_at_us[j] - x.phase_at_us;
        if (pair_gap > PHASE_PAIR_WINDOW_US || pair_gap < -PHASE_PAIR_WINDOW_US) continue;
        if (t - x.heard_at_us[j] > PHASE_STALE_US) continue;
        sum += x.heard_phase_us[j] - x.phase_us;
        n_contrib++;
      }
      if (n_contrib >= 2) {
        const double mean_rel = sum / static_cast<double>(n_contrib);
        g.present = true;
        g.contributors = static_cast<uint8_t>(n_contrib);
        g.delta_us = static_cast<int64_t>(std::llround(-mean_rel));
        x.last_gd_us = g.delta_us;
        x.last_gd_at_us = t;
        gd_fresh_ticks++;
      } else if (x.last_gd_at_us != 0 && t - x.last_gd_at_us <= GROUP_DELTA_STALE_US) {
        // Keep the last valid delta rather than reporting unknown, as tsf_sync does.
        g.present = true;
        g.contributors = 2;
        g.delta_us = x.last_gd_us;
      }
      // THE SHARED COMMON ERROR, formed the way the mapping is: robust_mean over the whole set,
      // self included, using the REAL estimator rather than a plain average written here -- the
      // production weighting is what has to be shown to work, and a mean of my own would only
      // test itself. Self must be in the set or the members are not averaging the same thing.
      {
        double vals[8];
        size_t nv = 0;
        if (x.last_e_split_valid) {
          vals[nv++] = static_cast<double>(x.last_e_common_us);
        }
        for (size_t j = 0; j < b.size() && nv < 8; j++) {
          if (j == i || !x.heard_common_valid[j]) continue;
          if (t - x.heard_common_at_us[j] > PHASE_STALE_US) continue;
          vals[nv++] = x.heard_common_us[j];
        }
        // Two or more, for the same reason the group delta needs two: one value is not a
        // consensus, it is just this board's own opinion wearing the word.
        if (nv >= 2) {
          g.common_valid = true;
          g.common_n = static_cast<uint8_t>(nv);
          g.common_us = static_cast<int64_t>(std::llround(
              esphome::clock_sync::robust_mean(vals, nv, esphome::clock_sync::CONSENSUS_SCALE_FLOOR_US)));
          x.last_common_c_us = g.common_us;
          x.last_common_c_at_us = t;
        } else if (x.last_common_c_at_us != 0 &&
                   t - x.last_common_c_at_us <= GROUP_DELTA_STALE_US) {
          // HOLD THE LAST CONSENSUS while it is fresh, exactly as the group delta is held above.
          // Dropping to zero the moment a pairing is missed is itself an asymmetry, and the
          // measured one: the consensus formed on both boards only 34% of the time and on exactly
          // ONE of them 30% of the time, so the "shared" correction differed between boards by
          // 15.08 ppm -- as large as the correction itself. A correction that is not simultaneous
          // is a per-board gain, which is the one thing this mechanism exists to avoid.
          g.common_valid = true;
          g.common_n = 2;
          g.common_us = x.last_common_c_us;
        }
      }

      // The board's own transient, exactly the flag it already uses to stop beaconing.
      g.self_transient = !x.phase_publishing;
      // There is always another board here, so peers exist whether or not a delta does. This is
      // the bit that lets the engine tell "lone client" from "peer exists, delta missing".
      g.has_peers = b.size() > 1;

      if (g.present) {
        gd_present_ticks++;
        gd_age_sum_us += static_cast<double>(t - x.last_gd_at_us);
      }
      gd_ticks++;

      // UNANNOUNCED move: the resync path shifts the audio and the engine is never told.
      // A HARD RESYNC, as the client actually performs one: it fires when the board is already
      // far off its own deadline and moves the audio to CLOSE that gap -- it is a corrective
      // action, not an arbitrary shove. Modelling it as a fixed displacement in opposite
      // directions on the two boards (which is what I did first) puts them 100 ms apart every
      // period and no loop could survive it; that was a broken model, not a broken loop.
      //
      // The engine's problem is not the move, it is not being TOLD: it sees the error vanish in
      // one step and reads that as the plant having jumped.
      // WHOLE CHUNKS, not a continuous null. The client "drops chunks until we catch back up", so
      // the repair is QUANTISED to a chunk (~26 ms) and cannot land on zero. Modelling it as an
      // exact null hid the mechanism that matters: two boards either side of the threshold drop a
      // DIFFERENT WHOLE NUMBER of chunks, and an aligned pair is then a chunk apart.
      //
      // Bench 2026-09-03: board a's repair was frames=+1136 = 25.76 ms, i.e. exactly one chunk,
      // taken while the pair was 15.7 us apart on the wire.
      // ACTS ON THE MEASUREMENT, NOT THE TRUTH -- as the client does. This tested
      // x.err_us + shifts, i.e. the true error, so no measurement fault could ever reach it and
      // "the repair believed a bad reading" was unrepresentable. The firmware tests coarse_err_us,
      // which is derived from the observation, so a lie in the observation is a lie to the repair.
      //
      // CONFIRMATION, matching the client's RESYNC_CONFIRM_SAMPLES: a real displacement persists
      // across samples, an artifact does not. resync_confirm = 0 reproduces the old behaviour.
      // PENDING REPAIRS SUBTRACTED, as the client does with `err_pre - pend`. seen is lagged by
      // measurement_lag_us, so for ~10 ticks after a repair it still reports the error the repair
      // has already removed. Acting on that re-applies the same fix again and again: without this
      // the model took 5231 repairs from 7 injected glitches and diverged to NaN, and section 4's
      // frame counts reached 2.7e9. That was my model, not the loop.
      if (x.resync_pending_us != 0.0 && t >= x.resync_pending_until_us) {
        x.resync_pending_us = 0.0;
      }
      // OPT-IN, and the default is the LESS FAITHFUL path. The client always tests a value derived
      // from the observation; this model tested the TRUTH, which is why no measurement fault could
      // reach it. Switching every scenario over destabilised the calibrated ones badly -- 3a went
      // to 631 ms of median skew with gd pairing collapsing from 100%/32% to 42%/8% -- so the
      // faithful path is enabled only where it is being exercised (3g) rather than shipped broken
      // everywhere. Completing the model (the client's per-block pacing, which bounds repair rate
      // in a way this does not) is follow-up work, and until it is done the truth-based default is
      // a KNOWN fidelity gap, not a choice.
      const double measured_now =
          f.resync_on_measurement ? (seen - x.resync_pending_us)
                                  : (x.err_us + deadline_shift + x.own_deadline_us);
      const bool past = f.resync_us > 0.0 && std::fabs(measured_now) > f.resync_us;
      const int rdir = measured_now > 0 ? 1 : -1;
      if (past) {
        const bool fresh = !f.confirm_distinct || x.meas_at_us != x.resync_confirm_at_us;
        if (fresh) {
          if (rdir == x.resync_dir) x.resync_run++;
          else { x.resync_dir = rdir; x.resync_run = 1; }
          x.resync_confirm_at_us = x.meas_at_us;
        }
      } else {
        x.resync_run = 0; x.resync_dir = 0; x.resync_confirm_at_us = -1;
      }
      if (past && x.resync_run >= (f.resync_confirm > 0 ? f.resync_confirm : 1)) {
        const double chunk = f.resync_chunk_us > 0.0 ? f.resync_chunk_us : 26123.0;
        const double moved = std::trunc(measured_now / chunk) * chunk;
        if (moved != 0.0) {
          x.err_us -= moved;
          x.resyncs++;
          x.resync_pending_us += moved;
          x.resync_pending_until_us = t + p.measurement_lag_us;
          if (f.announce_resync) {
            x.eng.note_external_move(static_cast<int64_t>(std::llround(moved)), t);
          }
        }
      }

      // Observations that simply do not arrive.
      bool valid = true;
      if (f.blind_fraction > 0.0) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        valid = u(rng) >= f.blind_fraction;
      }
      Command c = x.eng.step(t, Observation{t, static_cast<int64_t>(std::llround(seen)), valid,
                                            static_cast<int64_t>(std::llround(x.buffer_us))}, g);
      if (c.decision.gd_snap_us != 0) {
        snap_n++;
        const int mag = c.decision.gd_snap_us < 0 ? -c.decision.gd_snap_us : c.decision.gd_snap_us;
        snap_sum += mag;
        if (mag > snap_max) snap_max = mag;
      }
      if (c.frames != 0 && !x.live) {
        x.pid = c.correction_id;
        x.inflight.push_back({t + true_land_us, c.frames});
        x.live = true;
        x.corrections++;
        x.gross_frames += std::abs(c.frames);
        // A DECAY CORRECTION: taken while the phase is trustworthy again, but within two filter
        // lengths of the fault ending -- i.e. paid out of a filter still unwinding a phantom, not
        // out of any error present now. These are the ones that cannot be justified by the current
        // measurement, and on the bench they were the whole visible excursion.
        if (x.transient_end_us >= 0 && t - x.transient_end_us <= 2 * p.filter_lag_us()) {
          decay_corr++;
          decay_frames += std::abs(c.frames);
        }
        // WHICH SIGNAL DROVE IT, counted the way esrc reports it on the bench. Needed to tell a
        // fix that FAILED from one that was never EXERCISED: the gd-filter compensation applies
        // only when have_diff is true, and if these corrections are have_diff=false then this
        // simulator cannot test it at all -- which byte-identical output would look exactly like.
        if (c.decision.e_from_diff) corr_from_diff++; else corr_from_dl++;
        // A DELIVERED CORRECTION ARMS THE TRANSIENT, which is what the client does at
        // snapcast_client.cpp `if (applied_frames != 0)`: "a delivered correction moves the audio,
        // so the phase does not describe it until the horizon passes". This simulator's comment
        // claimed the arm existed and the code only ever armed on the ring floor, so the phantom
        // differential was produced here but never FLAGGED -- and a gate on the flag would have
        // read as a no-op for the wrong reason. Armed at delivery, not at landing, because the
        // phase is wrong for the whole flight.
        x.phase_transient_until_us = std::max(x.phase_transient_until_us, t + p.compensation_us());
        // A DROP IS SPENT FROM THE BUFFER: dropping frames removes audio from the very thing that
        // lets playout continue. The engine caps a correction at half the buffer for this reason,
        // and that cap was unreachable here while the buffer was a constant.
        if (c.frames > 0) {
          x.buffer_us = std::max(0.0, x.buffer_us - static_cast<double>(c.frames) * p.frame_us());
          // Discarded, not played -- counted apart for the same reason the firmware counts it
          // apart: this is the CORRECTION draining the ring, not the DAC consuming it.
          x.mb_disc_us += static_cast<double>(c.frames) * p.frame_us();
        }
      }
      x.last_rate_ppm = c.rate_ppm;
      x.last_xtal_ppm = c.decision.crystal_ppm;
      x.last_gd_snap_us = c.decision.gd_snap_us;
      // THE PHANTOM TEST: is |e_diff| larger just after this board's own correction landed than at
      // other times? If the board is seeing its own step as a differential, it must be.
      if (c.decision.e_split_valid && t > 120000000) {
        const double ad = std::fabs(static_cast<double>(c.decision.e_diff_us));
        if (x.last_land_us > 0 && t - x.last_land_us <= 2000000) {
          ph_after_sum += ad; ph_after_n++;
          // Would the gate have caught it? The flag must actually be set on these samples, or a
          // gate on it is inert -- which is precisely what the missing delivered-correction arm
          // would have hidden.
          if (!x.phase_publishing) ph_after_flagged++;
        } else { ph_base_sum += ad; ph_base_n++; }
      }
      // PEAK COMMON ERROR is the quantity the drain exists to bound: the resync fires at 50 ms,
      // so keeping the peak under that is what prevents the whole cascade.
      if (c.decision.e_split_valid && t > 120000000) {
        const double ac = std::fabs(static_cast<double>(c.decision.e_common_us));
        if (ac > common_max_us) common_max_us = ac;
      }
      // THE NOISE RATIO, which is what decides the SIGN of sizing Kp from the differential. Both
      // numbers formed the same way the engine forms them, so they are comparable with the bench's
      // ENGINE sigma=/gsig= fields rather than merely similar.
      if (t > 120000000) {
        const double se = x.eng.sigma_e_us();
        if (se > sig_e_max) sig_e_max = se;
        sig_e_sum += se;
        sig_g_sum += c.decision.gd_sigma_us;
        sig_n++;
      }
      // P PINNED AT THE AUTHORITY CLAMP is the bench's signature, not merely a large P: with
      // dx=0 and dp=0 the command is no longer tracking anything, it is being slew-walked toward
      // a saturated setpoint, which is what draws the near-vertical cliff on the wire.
      if (t > 120000000) {
        const double ap = std::fabs(static_cast<double>(c.decision.p_ppm));
        psat_n++;
        if (ap >= 0.99 * static_cast<double>(p.rate_authority_ppm)) psat_hit++;
        if (ap > psat_max) psat_max = ap;
      }
      x.last_pc_ppm = c.decision.pc_ppm;
      x.last_common_valid = c.decision.common_shared_valid;
      x.last_e_split_valid = c.decision.e_split_valid;
      x.last_e_common_us = c.decision.e_common_us;
      x.last_e_diff_us = c.decision.e_diff_us;
      x.err_us += (x.plant_ppm - c.rate_ppm) * 1e-6 * double(tick);
    }
    if (t > 120000000) {
      const double skew_signed = b[1].err_us - b[0].err_us;
      skews.push_back(std::fabs(skew_signed));
      skews_signed.push_back(skew_signed);
      skew_t_us.push_back(static_cast<double>(t));
      // THE SERIES ITSELF, per tick, when asked for. Summary statistics answer only the question
      // they were written for; the bench's test.csv is analysed with whatever the question turns
      // out to need. Dumping the same shape here means ONE analysis runs over both, so a claim
      // about the sim and a claim about the wire are comparable rather than merely similar.
      //   SIM_SKEW_CSV=/tmp/sim.csv ./tests/group/run.sh
      if (g_skew_csv != nullptr) {
        // Everything needed to test the ^/V hypothesis offline, per board: the COMMANDED rate
        // (whose derivative is what the wire plots), the crystal under it, the snap that fired on
        // this decision, and the two error components the integral saw. A snap is a step in P, so
        // a cycle in d(rate)/dt should sit against a snap out and a snap back -- and that claim is
        // checkable from this file rather than from a shape.
        // An invalid split writes an EMPTY field, not a zero: the reader gets NaN and drops the
        // row, which is the only honest encoding of "the group supplied no differential here".
        char sa[16] = "", sb[16] = "", da[16] = "", db[16] = "";
        if (b[0].last_e_split_valid) {
          snprintf(sa, sizeof(sa), "%d", b[0].last_e_common_us);
          snprintf(da, sizeof(da), "%d", b[0].last_e_diff_us);
        }
        if (b[1].last_e_split_valid) {
          snprintf(sb, sizeof(sb), "%d", b[1].last_e_common_us);
          snprintf(db, sizeof(db), "%d", b[1].last_e_diff_us);
        }
        fprintf(g_skew_csv, "%s,%.6f,%.3f,%.4f,%.4f,%.4f,%.4f,%d,%d,%s,%s,%s,%s\n", g_scenario,
                static_cast<double>(t) / 1e6, skew_signed, b[0].last_rate_ppm, b[1].last_rate_ppm,
                b[0].last_xtal_ppm, b[1].last_xtal_ppm, b[0].last_gd_snap_us, b[1].last_gd_snap_us,
                sa, sb, da, db);
      }
      // Means over the SETTLED window only, the same window the skew statistics use.
      rate_sum_a += b[0].last_rate_ppm;  rate_sum_b += b[1].last_rate_ppm;
      pc_sum_a += b[0].last_pc_ppm;      pc_sum_b += b[1].last_pc_ppm;
      if (b[0].last_common_valid) common_valid_n++;
      // THE CLAIM THE WHOLE MECHANISM RESTS ON: both boards apply the SAME correction, so it
      // injects no differential. Measured rather than assumed -- if the two pc terms differ, the
      // shared correction is a per-board gain wearing a different name, which is the one thing
      // the design forbids. Split out by cause: a difference because only one board HAS the
      // consensus is a different defect from a difference in its value.
      pc_absdiff_sum += std::fabs(b[0].last_pc_ppm - b[1].last_pc_ppm);
      if (b[0].last_common_valid != b[1].last_common_valid) common_one_sided_n++;
      if (b[0].last_common_valid && b[1].last_common_valid) both_valid_n++;
      settled_n++;
      errs.push_back(std::fabs(b[0].err_us + deadline_shift));
    }
  }
  // How often the differential was AVAILABLE at all. The code's own comment says the 300 ms
  // pairing window "leaves 0-2 contributors 94% of the time", and both actuators now steer on
  // this signal -- so its availability is a property worth reporting, not assuming.
  // Present is not the same as fresh: tsf_sync KEEPS the last valid delta for up to 15 s rather
  // than reporting unknown, so "present" is ~always true once one pairing has happened. What
  // matters to an actuator steering on it is how OLD the number is.
  if (gd_ticks > 0 && gd_present_ticks > 0) {
    printf("        [gd present %.0f%% of decisions, FRESHLY PAIRED on %.1f%%, mean age %.0f ms]\n",
           100.0 * gd_present_ticks / static_cast<double>(gd_ticks),
           100.0 * gd_fresh_ticks / static_cast<double>(gd_ticks),
           gd_age_sum_us / static_cast<double>(gd_present_ticks) / 1000.0);
  }
  Result r;
  // Periodicity BEFORE sorting: the crossing test needs the series in time order. Crossings are
  // taken against the MEDIAN, not the mean -- a ring with a standing bias crosses its mean rarely,
  // which on the bench gave 5 crossings in 187 s and a "period" that was not evidence of anything.
  if (skews_signed.size() > 8) {
    std::vector<double> sorted_signed = skews_signed;
    std::sort(sorted_signed.begin(), sorted_signed.end());
    const double mid = sorted_signed[sorted_signed.size() / 2];
    std::vector<double> periods;
    double prev_cross = -1.0;
    for (size_t i = 1; i < skews_signed.size(); i++) {
      if (skews_signed[i - 1] <= mid && skews_signed[i] > mid) {
        const double now_s = skew_t_us[i] / 1e6;
        if (prev_cross >= 0.0) {
          periods.push_back(now_s - prev_cross);
        }
        prev_cross = now_s;
      }
    }
    r.n_periods = static_cast<int>(periods.size());
    if (!periods.empty()) {
      std::sort(periods.begin(), periods.end());
      r.period_s = periods[periods.size() / 2];
    }
    r.amp_p05 = sorted_signed[static_cast<size_t>(0.05 * sorted_signed.size())];
    r.amp_p95 = sorted_signed[static_cast<size_t>(0.95 * sorted_signed.size())];

    // PEAK-TO-PEAK, GRADED THE WAY THE BENCH IS GRADED: median of the p2p within successive 120 s
    // windows. This is the quantity that was asked for and it is NOT skew_med -- sd is the bulk of
    // the distribution while p2p is its tails, i.e. the ^/V excursions themselves. A change can
    // improve sd and leave p2p untouched, and reading only sd would score that as a win.
    //
    // Windowed rather than whole-run so it does not simply grow with run length, and median of
    // windows rather than the max so one disturbance does not become the headline.
    {
      std::vector<double> win_p2p, win_sd;
      size_t i0 = 0;
      while (i0 < skew_t_us.size()) {
        size_t i1 = i0;
        double lo = skews_signed[i0], hi = skews_signed[i0], sum = 0.0, sum2 = 0.0;
        size_t n = 0;
        while (i1 < skew_t_us.size() && skew_t_us[i1] - skew_t_us[i0] <= 120e6) {
          const double v = skews_signed[i1];
          lo = std::min(lo, v); hi = std::max(hi, v);
          sum += v; sum2 += v * v; n++; i1++;
        }
        if (n >= 100) {
          win_p2p.push_back(hi - lo);
          const double m = sum / static_cast<double>(n);
          win_sd.push_back(std::sqrt(std::max(0.0, sum2 / static_cast<double>(n) - m * m)));
        }
        i0 = i1;
      }
      if (!win_p2p.empty()) {
        std::sort(win_p2p.begin(), win_p2p.end());
        std::sort(win_sd.begin(), win_sd.end());
        r.p2p_med = win_p2p[win_p2p.size() / 2];
        r.sd_med = win_sd[win_sd.size() / 2];
        r.p2p_windows = static_cast<int>(win_p2p.size());
      }
    }
  }
  std::sort(skews.begin(), skews.end());
  std::sort(errs.begin(), errs.end());
  r.skew_med = skews[skews.size()/2];
  r.skew_p90 = skews[static_cast<size_t>(0.9*skews.size())];
  r.err_med = errs[errs.size()/2];
  r.skew_max = skews.empty() ? 0.0 : skews.back();   // skews is sorted by here
  r.resync_a = b[0].resyncs;
  r.resync_b = b[1].resyncs;
  r.corr = b[0].corrections + b[1].corrections;
  r.gross = b[0].gross_frames + b[1].gross_frames;
  r.gd_snaps = snap_n;
  r.gd_snap_abs_sum = snap_sum;
  r.gd_snap_max = snap_max;
  r.xtal_a = b[0].eng.crystal_ppm();
  r.xtal_b = b[1].eng.crystal_ppm();
  r.buf_start = 1724000.0;
  r.mb_in_a = b[0].mb_in_us;  r.mb_out_a = b[0].mb_out_us;  r.mb_disc_a = b[0].mb_disc_us;
  r.mb_in_b = b[1].mb_in_us;  r.mb_out_b = b[1].mb_out_us;  r.mb_disc_b = b[1].mb_disc_us;
  r.buf_end_a = b[0].buffer_us;  r.buf_end_b = b[1].buffer_us;
  if (settled_n > 0) {
    const double sn = static_cast<double>(settled_n);
    r.rate_a = rate_sum_a / sn;  r.rate_b = rate_sum_b / sn;
    r.pc_a = pc_sum_a / sn;      r.pc_b = pc_sum_b / sn;
    r.common_valid_frac = static_cast<double>(common_valid_n) / sn;
    r.ediff_after_land = ph_after_n ? ph_after_sum / ph_after_n : 0.0;
    r.ediff_baseline = ph_base_n ? ph_base_sum / ph_base_n : 0.0;
    r.ediff_after_n = ph_after_n;
    r.ediff_after_flagged = ph_after_n ? static_cast<double>(ph_after_flagged) / ph_after_n : 0.0;
    r.common_max = common_max_us;
    r.decay_corr = static_cast<int>(decay_corr);
    r.decay_frames = decay_frames;
    r.corr_from_diff = static_cast<int>(corr_from_diff);
    r.corr_from_dl = static_cast<int>(corr_from_dl);
    r.p_sat_frac = psat_n ? static_cast<double>(psat_hit) / psat_n : 0.0;
    r.p_max_ppm = psat_max;
    if (sig_n > 0) {
      r.sigma_e_mean = sig_e_sum / static_cast<double>(sig_n);
      r.gd_sigma_mean = sig_g_sum / static_cast<double>(sig_n);
      r.sigma_e_max = sig_e_max;
    }
    r.pc_absdiff = pc_absdiff_sum / sn;
    r.common_one_sided_frac = static_cast<double>(common_one_sided_n) / sn;
    r.common_both_frac = static_cast<double>(both_valid_n) / sn;
  }
  return r;
}

}  // namespace

int main() {
  if (const char *path = getenv("SIM_SKEW_CSV")) {
    g_skew_csv = fopen(path, "w");
    if (g_skew_csv != nullptr) {
      fprintf(g_skew_csv,
              "scenario,t_s,skew_us,rate_a_ppm,rate_b_ppm,xtal_a_ppm,xtal_b_ppm,"
              "snap_a_us,snap_b_us,ecom_a_us,ecom_b_us,ediff_a_us,ediff_b_us\n");
      fprintf(stderr, "dumping per-tick skew to %s\n", path);
    } else {
      fprintf(stderr, "could not open %s -- continuing without the dump\n", path);
    }
  }
  Profile p;
  p.frame_rate_hz = 44100;
  p.measurement_lag_us = 250000;
  p.position_delay_us = 1250000;
  p.target_diff_us = 20;
  p.rate_authority_ppm = 100.0f;
  // An eighth of the ring's capacity, as snapcast_client derives it. Never set here before, so it
  // was 0 and the engine's starvation guard was disabled outright -- the guard has never once run
  // in this simulator.
  p.buffer_floor_us = 1724000 / 8;
  const int64_t TRUE_LAND = 1250000;

  printf("two boards, shared drifting deadline; frame %lld us\n\n",
         static_cast<long long>(p.frame_us()));

  printf("1. COMMON-MODE drift: both boards off the deadline together, in sync with each other\n");
  {
    // The bench's own case: gd tens of us while each board sits hundreds of us off its deadline.
    // Correcting that with position moves the pair APART, which is the only thing a listener can
    // hear. The boards must stay together and must not spend frames doing it.
    Result r = simulate("common", p, 0.0, 0.0, 40.0, 80.0, 600.0, TRUE_LAND);
    printf("        skew median %.0f us, p90 %.0f us; own error median %.0f us; "
           "%d corrections, %ld frames\n", r.skew_med, r.skew_p90, r.err_med, r.corr, r.gross);
    check(r.skew_med < 2.0 * static_cast<double>(p.frame_us()),
          "boards stay in sync with EACH OTHER", r.skew_med, 2.0 * p.frame_us());
    check(r.corr == 0, "and spend no frame corrections on a common error", r.corr, 0);
  }

  printf("\n2. DIFFERENTIAL: the boards disagree with each other, which IS audible\n");
  {
    Result r = simulate("differential", p, -15.0, +15.0, 0.0, 80.0, 600.0, TRUE_LAND);
    printf("        skew median %.0f us, p90 %.0f us; %d corrections, %ld frames\n",
           r.skew_med, r.skew_p90, r.corr, r.gross);
    check(r.skew_med < 2.0 * static_cast<double>(p.frame_us()),
          "a real differential IS pulled in", r.skew_med, 2.0 * p.frame_us());
  }

  printf("\n3. both at once: common drift plus a differential split\n");
  {
    Result r = simulate("both", p, -15.0, +15.0, 40.0, 80.0, 600.0, TRUE_LAND);
    printf("        skew median %.0f us, p90 %.0f us; own error median %.0f us; "
           "%d corrections, %ld frames\n", r.skew_med, r.skew_p90, r.err_med, r.corr, r.gross);
    check(r.skew_med < 2.0 * static_cast<double>(p.frame_us()),
          "the differential is corrected without chasing the common part",
          r.skew_med, 2.0 * p.frame_us());
    // THE QUIET CASE, which is where the bench sees the >20 ppm d(rate)/dt cycles. The jump
    // detector's threshold is max(gmax, 4*sigma) and BOTH terms shrink when the loop is calm:
    // gmax = (authority + 200) * gdt + frame, with gdt the OBSERVATION interval because
    // gd_last_at_us_ is refreshed every decision rather than every gd change, and sigma floors at
    // a quarter frame. So the threshold collapses to ~26 us exactly when gd is quietest -- and
    // gd's own beacon staircase has a p90 step of 61 us on the bench.
    printf("        QUIET: gd filter SNAPS %d, mean |snap| %.0f us, max %d us -> Kp*max ~ %.0f ppm\n",
           r.gd_snaps, r.gd_snaps ? r.gd_snap_abs_sum / r.gd_snaps : 0.0, r.gd_snap_max,
           0.45 * r.gd_snap_max);
  }

  printf("\n3a. CRYSTAL UNDER STARVATION (characterisation; the guard that tried to fix it was reverted)\n");
  {
    // The integral's input is clamped to one frame per component, which bounds the winding RATE
    // (~38 ppm/min here) but not where it stops. A starved board is behind for as long as the
    // shortage lasts AND for the whole catch-up after it, and that error is one-signed throughout.
    //
    // Bench 2026-09-02: board a starved (ARRGAP mean 61-100 ms against 26 ms of audio per chunk,
    // n down to a fifth) and wound a hand-zeroed crystal to +192 ppm in twelve minutes, 8 ppm off
    // the rail, against a true crystal of +46. The starvation guard already held DURING the
    // shortage; it released the moment the buffer cleared its floor, which is not when the error
    // stops describing the shortage.
    // THE TARGET IS plant + common, NOT plant. Holding the error still requires cancelling the
    // plant AND the common deadline drift, since measured = err + deadline_shift and
    // d(err)/dt = plant - rate. Scoring against `plant` alone is what made a REVERTED guard look
    // like a 20 ppm improvement when it was really a 25 ppm regression -- the convenient property
    // rather than the one that matters.
    // THE BENCH'S OWN CADENCE: ~6 s of shortage every ~120 s. A 50% duty was tried and is
    // pathological -- it drives skew to seconds and tells us nothing about the real fleet.
    const double plant_a = -15.0, plant_b = +15.0, common = 40.0;
    const double want_a = plant_a + common, want_b = plant_b + common;
    Faults hungry{};
    hungry.starve_period_s = 120.0;
    hungry.starve_secs = 6.0;
    // THE CLIENT'S HARD RESYNC, 50 ms. Omitted here until 2026-09-02, and its absence was not a
    // simplification -- it removed the only thing bounding the error. A starved board fell behind
    // with nothing to catch it up, so the COMMON error ran to 129 SECONDS and skew p90 to 2.7 s,
    // and the wind-up figures below were measured in that regime rather than in the client's.
    // Found because a shared common-mode correction saturated its clamp for every gain: the
    // scenario, not the mechanism, was what the sweep was measuring.
    hungry.resync_us = 50000.0;
    Result r = simulate("starve-xtal", p, plant_a, plant_b, common, 80.0, 1800.0, TRUE_LAND, hungry);
    printf("        want %+.0f / %+.0f (plant + common)  ->  got %+.1f / %+.1f ppm"
           "   differential want %+.0f got %+.1f   (skew med %.0f us)\n",
           want_a, want_b, r.xtal_a, r.xtal_b, want_b - want_a, r.xtal_b - r.xtal_a, r.skew_med);

    // WHAT SURVIVES A STARVATION IS THE DIFFERENTIAL, AND THAT IS WHAT IS AUDIBLE. Both crystals
    // wind far above the truth -- reproducing the bench's +192 ppm against a true +46 -- but they
    // wind TOGETHER, so the difference between them stays right and the pair still sounds
    // synchronised. That is the property to hold the engine to.
    check(std::fabs((r.xtal_b - r.xtal_a) - (want_b - want_a)) < 12.0,
          "the DIFFERENTIAL between the crystals survives, which is the audible part",
          r.xtal_b - r.xtal_a, want_b - want_a);
    check(r.skew_med < 60.0, "and the boards stay in sync with each other", r.skew_med, 60.0);

    // THIS SCENARIO NO LONGER REPRODUCES THE WIND-UP AT ALL, and every step of that was a defect
    // in the model rather than a change to the engine:
    //
    //   no resync, bang-bang buffer     +156.5/+183.6   excess +131/+129 ppm
    //   + the client's 50 ms resync      +46.4/ +75.4   excess  +21/ +20
    //   + honest supply (backlog)        +22.5/ +51.6   excess -2.5/ -3.4
    //
    // Against a target of +25/+55. The first version let the error run to 129 s because nothing
    // bounded it; the second still refilled the buffer at 2x whenever it sat below nominal, which
    // the rate surplus triggered every tick -- manufacturing 449 s of audio across a 900 s run and
    // hiding it in the nominal clamp. With supply at realtime plus a finite backlog, the integral
    // settles within ~3 ppm of correct and there is no wind-up to see.
    //
    // So: the bench wound a hand-zeroed crystal to +192 against a true +46, WITH a resync active,
    // and this model given the same conditions winds essentially nothing. The gap is now total,
    // not narrowed -- the bench's wind-up has a driver this simulator does not contain, and the
    // three drivers it used to be blamed on here were all artifacts. Do not cite 3a as evidence
    // that the wind-up is reproduced, and do not tune against it.
    //
    // The wind-up is inaudible right up until a board RAILS, at which point it loses the authority
    // to correct anything -- which is what board a did on the bench 2026-09-02. Three attempts at this
    // have now failed: holding the integral during starvation (reverted, bc80f18: it suppresses
    // real drift identically) and gating phase publication (4e68870: measured NEUTRAL here at this
    // cadence, and actively harmful at 50% duty, where losing gd sends position onto the common
    // error). The third was a shared common-mode correction on RATE (3c): the consensus is sound
    // and the plumbing works, but rate is the wrong actuator for a displacement -- see 3c.
    // The bound below is where it sits today, not where it should sit.
    check(std::fabs(r.xtal_a) < 200.0f && std::fabs(r.xtal_b) < 200.0f,
          "neither has RAILED yet (documented gap: the wind-up itself is unaddressed)",
          std::max(std::fabs(r.xtal_a), std::fabs(r.xtal_b)), 200.0);
    printf("        gd filter SNAPS: %d, mean |snap| %.0f us, max %d us  -> Kp*snap ~ %.0f ppm at the max\n",
           r.gd_snaps, r.gd_snaps ? r.gd_snap_abs_sum / r.gd_snaps : 0.0, r.gd_snap_max,
           0.45 * r.gd_snap_max);
  }

  printf("\n3c. SHARED COMMON-MODE CORRECTION: can the group take the common error out itself,\n");
  printf("    so the crystal integral stops absorbing it?\n");
  {
    // THE PREREQUISITE, checked before the mechanism was written: e_common is genuinely common.
    // Across every scenario above the two boards' values agree to 1-4% with r up to 1.00, and
    // disagree by a flat ~80 us that does NOT scale with the error -- exactly the per-board
    // measurement noise. So a consensus over them is well posed, and the reason to take it is not
    // the sqrt(2) of noise it saves at N=2 but that a SHARED correction is the same number on
    // every board and therefore injects no differential at all.
    //
    // WHAT MUST HOLD, and what must not be confused:
    //   - total commanded rate stays at plant + common          (the loop still tracks)
    //   - skew does not regress                                 (nothing audible was traded)
    //   - the CRYSTAL falls from plant + common toward plant    (the integral stops absorbing it)
    // The third is the point, and it is why the crystal cannot be scored against a fixed target
    // across this sweep: the correct value for the integral MOVES as the gain takes the common
    // part away from it. Scoring against `plant + common` throughout would report the mechanism
    // working as a failure -- the same error that made a reverted guard look like a 20 ppm win.
    const double plant_a = -15.0, plant_b = +15.0, common = 40.0;
    Faults hungry{};
    hungry.starve_period_s = 120.0;
    hungry.starve_secs = 6.0;
    // THE HARD RESYNC, which 3a deliberately omits and this test must not. Without it the error
    // accumulates unbounded -- the common error reaches 129 SECONDS and the baseline skew p90 is
    // 2.7 s -- so the correction saturates at its clamp for every gain and the sweep returns four
    // identical rows. That is a property of the scenario, not of the mechanism. The real client
    // resyncs at 50 ms, which is what keeps the common error in the millisecond range the bench
    // actually shows (1-11 ms) and where a proportional correction means anything at all.
    hungry.resync_us = 50000.0;
    printf("        plant %+.0f/%+.0f, common %+.0f  ->  total wants %+.0f/%+.0f;"
           " integral wants %+.0f/%+.0f once the gain carries the common part\n",
           plant_a, plant_b, common, plant_a + common, plant_b + common, plant_a, plant_b);
    printf("        THE TARGET IS max|com| < 50000 us -- below the resync threshold, so the solo\n");
    printf("        repair that breaks the pair (3f) never fires at all.\n");
    // TWO TARGETS, and the deadband is the point. A common error inside target_common_us is
    // inaudible and must not be chased: correcting it spends rate-command noise, and that noise
    // lands on the DIFFERENTIAL, which is the audible quantity. So the drain works on the EXCESS
    // beyond the target, which is also what lets it coexist with the crystal integral rather than
    // fighting it -- the integral owns common drift at all times, the drain owns only excursions.
    printf("        target_diff=%lld us (audible, drives Kp and the position gate)\n",
           static_cast<long long>(p.target_diff_us));
    printf("        %-8s %8s %8s %8s %8s %8s %10s %7s %8s %7s\n",
           "drain_s", "cmn_tgt", "xtal a", "xtal b", "pc a", "pc b", "max|com|", "rsync", "skew med", "p90");
    struct CmnCase { double drain; int64_t target; };
    const CmnCase ccases[] = {{0.0, 5000}, {300.0, 0}, {300.0, 5000}, {300.0, 20000}};
    for (const CmnCase &cc : ccases) {
      const double drain = cc.drain;
      Profile q = p;
      q.common_drain_s = static_cast<float>(drain);
      q.target_common_us = cc.target;
      char tag[24];
      snprintf(tag, sizeof(tag), "cmn-%.0f-%lld", drain, static_cast<long long>(cc.target));
      Result r = simulate(tag, q, plant_a, plant_b, common, 80.0, 1800.0, TRUE_LAND, hungry);
      printf("        %-8.0f %8lld %8.1f %8.1f %8.1f %8.1f %10.0f %3d/%-3d %8.0f %7.0f\n",
             drain, static_cast<long long>(cc.target), r.xtal_a, r.xtal_b, r.pc_a, r.pc_b,
             r.common_max, r.resync_a, r.resync_b, r.skew_med, r.skew_p90);
      printf("               [shared? mean |pc_a - pc_b| = %.2f ppm; consensus BOTH %.0f%%,"
             " ONE-SIDED %.0f%%]\n",
             r.pc_absdiff, 100.0 * r.common_both_frac, 100.0 * r.common_one_sided_frac);
      if (drain == 0.0) {
        // The consensus is formed and recorded even when the gain is zero -- that is the shadow,
        // and it is what makes the first row a genuine control rather than a different experiment.
        printf("               [shadow: consensus available on %.0f%% of settled decisions,"
               " correction applied %.2f ppm]\n", 100.0 * r.common_valid_frac, r.pc_a);
      }
    }
  }

  printf("\n3b. SPLIT FILTER: does rate benefit from a faster error filter than position?\n");
  {
    // One EWMA serves both actuators today, with tau = ERR_TAU_HORIZONS * compensation_us(), and
    // compensation is dominated by position_delay (ring + pipe). So RATE is smoothed on the
    // timescale of an actuator it does not use: measured on the bench, rate_horizon 2.0 s against
    // rate's own measurement lag of ~250 ms.
    //
    // Sweeping the rate-side horizon alone. Position keeps the slow filter throughout -- it is
    // irreversible and quantised, and must not fire on noise. The question is only whether rate,
    // which is continuous and reversible, is being held back for no benefit.
    //
    // A faster filter cuts rate's lag but passes more noise to Kp, and Kp = budget / sigma_e is
    // sized from the noise it sees, so this is a genuine trade rather than a free win. If skew
    // does not improve, the shared filter is correct and the coupling is not a defect.
    const int64_t shared = p.filter_lag_us();
    printf("        shared filter lag %lld ms (position keeps this throughout)\n",
           static_cast<long long>(shared / 1000));
    printf("        %-22s %9s %9s %9s %9s %9s\n", "rate filter", "skew med", "skew p90", "period", "amp p05", "amp p95");
    for (int64_t div : {1, 2, 4, 8}) {
      Profile q = p;
      q.rate_filter_lag_us = div == 1 ? 0 : shared / div;   // 0 = shared, i.e. today
      char tag[24];
      snprintf(tag, sizeof(tag), "rtfilt-%lld", static_cast<long long>(div));
      Result r = simulate(tag, q, -15.0, +15.0, 40.0, 80.0, 600.0, TRUE_LAND);
      char lbl[48];
      if (div == 1) {
        snprintf(lbl, sizeof(lbl), "shared (%lld ms)", static_cast<long long>(shared / 1000));
      } else {
        snprintf(lbl, sizeof(lbl), "1/%lld (%lld ms)", static_cast<long long>(div),
                 static_cast<long long>(shared / div / 1000));
      }
      printf("        %-22s %9.0f %9.0f %9.1f %9.0f %9.0f   %d corr, %ld frames\n",
             lbl, r.skew_med, r.skew_p90, r.period_s, r.amp_p05, r.amp_p95, r.corr, r.gross);
    }
    printf("        (a faster rate filter should cut lag; it also passes more noise to Kp)\n");
  }

  printf("\n3d. MASS BALANCE: does audio in equal audio out? Nothing in the loop enforces it --\n");
  printf("    the servo closes on the deadline error and buffer occupancy is never a setpoint.\n");
  {
    // The firmware now emits MASSBAL for exactly this; this is the same accounting in the model,
    // so the analysis that grades a log can be reasoned about before a board ever runs it.
    //
    // WHAT A DEFICIT MEANS depends entirely on which column carries it, which is why discards are
    // counted apart from playout: in < out with disc ~ 0 is the network failing to supply, while
    // disc carrying it is the correction eating the buffer. From the buffer level alone -- all
    // anything had until now -- those are indistinguishable.
    struct MbCase { const char *name; const char *tag; Faults f; };
    Faults none{};
    Faults starve{};  starve.starve_period_s = 119.0; starve.starve_secs = 3.0;
                      starve.resync_us = 50000.0;
    const MbCase cases[] = {{"clean", "mb-clean", none},
                            {"starvation bursts", "mb-starve", starve}};
    printf("        %-20s %10s %10s %9s %10s %10s %9s\n",
           "case", "in (s)", "out (s)", "disc (ms)", "d (ms)", "dbuf (ms)", "resid");
    for (const MbCase &c : cases) {
      Result r = simulate(c.tag, p, -15.0, +15.0, 40.0, 80.0, 900.0, TRUE_LAND, c.f);
      const double d_a = r.mb_in_a - r.mb_out_a - r.mb_disc_a;
      const double dbuf_a = r.buf_end_a - r.buf_start;
      printf("        %-20s %10.1f %10.1f %9.0f %10.0f %10.0f %9.0f\n",
             c.name, r.mb_in_a / 1e6, r.mb_out_a / 1e6, r.mb_disc_a / 1000.0,
             d_a / 1000.0, dbuf_a / 1000.0, (d_a - dbuf_a) / 1000.0);
      // THE IDENTITY, on the clean case only. With starvation the buffer spends real time pinned
      // at 0 and at nominal, and a clamp genuinely destroys audio -- the DAC cannot consume what
      // has not arrived -- so a residual there is the clamp doing its job, not a missing path.
      // Asserting it in both cases would be asserting that starvation is lossless.
      if (c.f.starve_period_s == 0.0) {
        check(std::fabs(d_a - dbuf_a) < 20000.0,
              "unclamped, the buffer IS the integral of in - out - disc",
              (d_a - dbuf_a) / 1000.0, 0.0);
      }
    }
    printf("        (a clean run should show in ~ out, disc 0, and the residual at the clamp only)\n");
  }

  printf("\n3e. THE PHANTOM DIFFERENTIAL: does a board read its OWN landed correction as skew?\n");
  {
    // BENCH 2026-09-03: board a applied frames=+1136 (25.76 ms) and its group delta immediately
    // read gd=+25005 us -- 97% of its own step, with a pairing gap of 71 us and extrap 0.00, so
    // neither staleness nor extrapolation. Every such sample carried steady=0; every clean one
    // carried steady=1. The board stops BEACONING during its transient but keeps feeding that
    // same phase into its own gd filter, which clamps rather than rejects, and P = 0.46 * gd then
    // commands tens of ppm. Position corrects a displacement and rate corrects the correction.
    //
    // The mechanism is expressible here: a landing steps err_us instantly, phase is derived from
    // err_us, and a peer's beaconed phase is up to a second stale. So if it happens on the bench
    // it must happen here -- unless the simulator's corrections are too small or too rare to show
    // it. That is what this measures, rather than assuming either way.
    printf("        %-26s %9s %9s %7s %8s %8s %8s %8s\n",
           "case", "eD base", "after", "ratio", "flagged", "skew med", "skew p90", "corr");
    struct PhCase { const char *name; const char *tag; Faults f; };
    Faults none{};
    Faults starve{};  starve.starve_period_s = 119.0; starve.starve_secs = 3.0; starve.resync_us = 50000.0;
    const PhCase cases[] = {{"clean", "ph-clean", none}, {"starvation bursts", "ph-starve", starve}};
    for (const PhCase &c : cases) {
      for (int gate = 0; gate <= 1; gate++) {
        Profile q = p;
        q.gate_gd_on_transient = (gate == 1);
        char tag[32]; snprintf(tag, sizeof(tag), "%s-g%d", c.tag, gate);
        Result r = simulate(tag, q, -15.0, +15.0, 40.0, 80.0, 900.0, TRUE_LAND, c.f);
        const double ratio = r.ediff_baseline > 0 ? r.ediff_after_land / r.ediff_baseline : 0.0;
        char nm[48]; snprintf(nm, sizeof(nm), "%s %s", c.name, gate ? "GATE ON" : "gate off");
        printf("        %-26s %9.0f %9.0f %7.2f %7.0f%% %8.0f %8.0f %8d\n",
               nm, r.ediff_baseline, r.ediff_after_land, ratio,
               100.0 * r.ediff_after_flagged, r.skew_med, r.skew_p90, r.corr);
      }
    }
    printf("        (flagged%% must be high or the gate has nothing to act on -- that was the\n");
    printf("         missing delivered-correction arm, which made the flag inert here)\n");
  }

  printf("\n3f. THE RESYNC BREAKING AN ALIGNED PAIR on a COMMON error (bench 2026-09-03)\n");
  {
    // THE CASE THE FIX HAS TO BE TESTED AGAINST, and the one this simulator could not produce.
    //
    // Bench: both boards sat ~51 ms from the server deadline TOGETHER while 15.7 us apart on the
    // wire (rival 0.027, a clean lock). Board a crossed the 50 ms threshold first, RSKIP fired on
    // the DEADLINE error -- `if (coarse_err_us > hard_us && ...)`, no e_diff, no group check --
    // and it stepped one chunk alone. The pair went from 15.7 us to 2.9 ms apart: an inaudible
    // common error converted into an audible differential one.
    //
    // Modelled as a SHARED deadline step (reanchor_stagger_s = 0, so both adopt at the same
    // instant) large enough to cross the threshold. The plants are equal and the common drift is
    // zero, so ANY skew here is manufactured by the repair -- there is no differential to find.
    const double step_us = 60000.0;   // one shared step, past the 50 ms threshold
    printf("        equal plants, no common drift: any skew below is MADE by the repair\n");
    printf("        %-30s %9s %9s %9s %8s %8s\n",
           "case", "skew med", "skew p90", "skew max", "rsync a", "rsync b");
    struct RCase { const char *name; const char *tag; double step; double stagger; double resync; };
    const RCase cases[] = {
      {"no step (control)",           "rs-none",   0.0,     0.0, 50000.0},
      {"shared step 60 ms, no resync","rs-nofix",  step_us, 0.0,     0.0},
      {"shared step 60 ms + resync",  "rs-common", step_us, 0.0, 50000.0},
      {"staggered 1 s + resync",      "rs-stag",   step_us, 1.0, 50000.0},
      // ON A CHUNK BOUNDARY. The repair steps whole 26.123 ms chunks, so trunc() is a cliff: two
      // boards whose common error straddles a multiple of the chunk drop a DIFFERENT COUNT, and
      // an aligned pair is instantly a chunk apart. 52.246 ms is exactly two chunks, so 80 us of
      // measurement noise is enough to put the boards on opposite sides of it. Simultaneous
      // adoption, equal plants, no common drift -- nothing here is asymmetric except the rounding.
      {"shared step 52.246 ms (2 chunks)", "rs-cliff", 52246.0, 0.0, 50000.0},
    };
    for (const RCase &c : cases) {
      Faults f{};
      if (c.step > 0.0) {
        f.reanchor_period_s = 200.0;
        f.reanchor_us = c.step;
        f.reanchor_stagger_s = c.stagger;
      }
      f.resync_us = c.resync;
      Result r = simulate(c.tag, p, 0.0, 0.0, 0.0, 80.0, 900.0, TRUE_LAND, f);
      printf("        %-30s %9.0f %9.0f %9.0f %8d %8d\n",
             c.name, r.skew_med, r.skew_p90, r.skew_max, r.resync_a, r.resync_b);
    }
    printf("        (unequal resync counts are the signature: one board took a chunk the other did not)\n");
  }

  printf("\n3g. A MEASUREMENT THAT LIES: one bad sample, an irreversible repair (bench 2026-09-03)\n");
  {
    // Board a's deadline error stepped +50010 us for a single decision -- one whole DMA buffer
    // (50000 us) plus 10 us of drift -- while board b sat 15.7 us away on the wire and never
    // moved. hard_resync_threshold_ms was 50, so the artifact equalled the threshold EXACTLY, and
    // the repair dropped 1136 frames of real audio, putting the pair 2.9 ms apart.
    //
    // Equal plants, no drift, no starvation: the audio is never displaced here. Any skew is the
    // repair acting on a reading that was wrong.
    Faults g{};
    g.glitch_period_s = 120.0;
    g.resync_on_measurement = true;   // the fault is a bad READING; it cannot reach a truth-tested repair
    g.glitch_us = 50010.0;      // one DMA buffer plus drift, on board A only
    g.resync_us = 50000.0;      // the coincidence: threshold == the artifact
    printf("        glitch +50010 us on board A every 120 s; audio never moves\n");
    printf("        %-34s %9s %9s %9s %8s\n", "case", "skew med", "skew p90", "skew max", "rsync a/b");
    struct GCase { const char *name; const char *tag; int confirm; double thresh; bool distinct; };
    const GCase cases[] = {
      {"no glitch (control)",             "gl-none", 0, 50000.0, false},
      {"glitch, act on 1 sample (today)", "gl-1",    0, 50000.0, false},
      // THE TWO GATE DESIGNS, and the reason the distinction is not cosmetic: the lie spans one
      // publish interval, so it is READ by several evaluations. Counting reads confirms it.
      {"glitch, confirm 3 reads",         "gl-3r",   3, 50000.0, false},
      {"glitch, confirm 3 SNAPSHOTS",     "gl-3s",   3, 50000.0, true},
      {"glitch, threshold 75 ms (1.5x)",  "gl-thr",  0, 75000.0, false},
      {"glitch, 3 snapshots + 75 ms",     "gl-both", 3, 75000.0, true},
    };
    for (const GCase &c : cases) {
      Faults f = g;
      f.resync_confirm = c.confirm;
      f.resync_us = c.thresh;
      f.confirm_distinct = c.distinct;
      if (std::string(c.tag) == "gl-none") { f.glitch_period_s = 0.0; f.glitch_us = 0.0; }
      Result r = simulate(c.tag, p, 0.0, 0.0, 0.0, 80.0, 900.0, TRUE_LAND, f);
      printf("        %-34s %9.0f %9.0f %9.0f %4d/%-4d\n",
             c.name, r.skew_med, r.skew_p90, r.skew_max, r.resync_a, r.resync_b);
    }
    printf("        (a repair on a lie shows as skew with resyncs on A only and none on B)\n");
  }

  printf("\n3h. Kp's DENOMINATOR: sized from the deadline error, applied to the differential\n");
  {
    // Kp = budget / sigma is a promise that command noise integrates to no more than
    // target_diff_us over the horizon. P multiplies e_position, which is the DIFFERENTIAL when the
    // group supplies one -- but sigma_e is the DEADLINE error's noise. Wrong distribution.
    //
    // Bench 2026-09-03: sigma_e pinned at its 22 us frame floor while sigma(gd) measured 30.5 us.
    // Halving timing_target_us (halving Kp) improved BOTH wire and rate -- offset sd 16.25 -> 12.41
    // us, rate p2p 87.9 -> 58.6 ppm -- and reverting degraded both again. Two metrics improving
    // together is over-gain, not a trade. But the target only compensated for the wrong
    // denominator by coincidence, and would stop the moment gd's noise moved.
    //
    // Swept at two noise levels because that is the whole claim: the fix should matter MORE when
    // the differential is noisier than the deadline error, and do nothing when it is not.
    printf("        %-28s %9s %9s %9s %8s\n", "case", "skew med", "skew p90", "corr", "frames");
    for (double noise : {80.0, 200.0}) {
      for (int fix = 0; fix <= 1; fix++) {
        Profile q = p;
        q.kp_from_diff_sigma = (fix == 1);
        char tag[32]; snprintf(tag, sizeof(tag), "kp-%.0f-%d", noise, fix);
        char nm[48]; snprintf(nm, sizeof(nm), "noise %.0f us, Kp from %s", noise,
                              fix ? "DIFF sigma" : "sigma_e");
        Result r = simulate(tag, q, -15.0, +15.0, 40.0, noise, 600.0, TRUE_LAND);
        printf("        %-28s %9.0f %9.0f %9d %8ld   sigma_e=%.1f gd_sigma=%.1f ratio=%.2f\n",
               nm, r.skew_med, r.skew_p90, r.corr, r.gross,
               r.sigma_e_mean, r.gd_sigma_mean,
               r.sigma_e_mean > 0 ? r.gd_sigma_mean / r.sigma_e_mean : 0.0);
      }
    }
    printf("        (the fix should help where sigma(gd) > sigma_e and be neutral otherwise)\n");
  }

  printf("\n3i. NEIGHBOUR TARGET SWEEP, graded on p2p (the quantity actually wanted)\n");
  {
    // Kp scales with target_diff_us (budget = 1e6 * target / rate_horizon), so this sweeps the
    // gain. Graded on median 2-minute p2p, matching how the wire is graded -- sd is the bulk and
    // p2p is the tails, and the tails ARE the ^/V excursions. Reading sd alone would have scored
    // a change that left p2p untouched as a win.
    //
    // BENCH REFERENCE for the same statistic: test.csv at target=10 gives p2p 94.0 us / sd 16.8
    // over 33 windows -- the smallest sustained figure in any archive on that host.
    //
    // Two reasons the sim's optimum need not be the bench's, both measured: its observation noise
    // is 80 us where the bench's sigma_e sits at its 22 us floor, and sigma(gd)/sigma_e is 0.13
    // here against 0.34 there. So treat the SHAPE of the curve as the finding, not the argmin.
    printf("        %-10s %10s %9s %8s %8s %8s %8s\n",
           "target_us", "p2p/2min", "sd", "windows", "corr", "frames", "period");
    for (int64_t tgt : {5, 10, 20, 40, 80}) {
      Profile q = p;
      q.target_diff_us = tgt;
      char tag[24];
      snprintf(tag, sizeof(tag), "tgt-%lld", static_cast<long long>(tgt));
      Result r = simulate(tag, q, -15.0, +15.0, 40.0, 80.0, 900.0, TRUE_LAND);
      printf("        %-10lld %10.1f %9.2f %8d %8d %8ld %8.1f\n",
             static_cast<long long>(tgt), r.p2p_med, r.sd_med, r.p2p_windows,
             r.corr, r.gross, r.period_s);
    }
    printf("        (lower target = lower Kp; watch for corr/frames rising as rate gives up authority)\n");
  }

  printf("\n3j. THE STABILITY CAP: Kp = budget/sigma_e has no phase margin in it\n");
  {
    // Rate drives position through an integrator behind rate_horizon_us of dead time, so the
    // critical proportional gain is Kp_crit = pi/(2L). budget/sigma_e is a NOISE constraint and
    // lands wherever sigma_e puts it -- which on the bench is 58% of critical at target=20, with
    // the loop ringing at 10-14 s against a predicted 4L = 8 s.
    //
    // NOISE 20 us IS THE BENCH'S REGIME, and it is the only row that can test this. sigma_e floors
    // at one frame (22 us) there, exactly as measured on hardware, so Kp lands high relative to
    // Kp_crit. At the simulator's usual 80 us sigma_e is MEASURED at ~80, Kp lands at 16% of
    // critical, and the loop is under-gained instead -- which is why lowering the target damped the
    // bench and made the sim sluggish, and why 3i and the hardware sweep disagreed all night.
    // Opposite sides of one optimum, not a fidelity bug.
    //
    // The cap can only REDUCE Kp, so it cannot introduce an instability of its own. What it should
    // do: help where the loop rings, and be neutral where it is already below the limit.
    const double L_s = static_cast<double>(p.rate_horizon_us()) / 1e6;
    printf("        L=%.2f s so Kp_crit=pi/(2L)=%.3f /s; cap at 0.30 -> Kp_max=%.3f\n",
           L_s, 1.5707963 / L_s, 0.30 * 1.5707963 / L_s);
    printf("        %-26s %9s %8s %8s %8s %8s\n",
           "case", "p2p/2min", "sd", "period", "corr", "frames");
    for (double noise : {20.0}) {
      for (int64_t tgt : {10, 20}) {
        for (double frac : {0.0, 0.30, 0.20, 0.12, 0.07}) {
          Profile q = p;
          q.target_diff_us = tgt;
          q.kp_stability_frac = static_cast<float>(frac);
          const double budget = 1e6 * static_cast<double>(tgt) / static_cast<double>(p.rate_horizon_us());
          char tag[32]; snprintf(tag, sizeof(tag), "cap-%.0f-%lld-%.0f", noise,
                                 static_cast<long long>(tgt), frac * 100);
          char nm[48]; snprintf(nm, sizeof(nm), "noise %.0f tgt %lld cap %s", noise,
                                static_cast<long long>(tgt), frac > 0 ? "ON " : "off");
          Result r = simulate(tag, q, -15.0, +15.0, 40.0, noise, 900.0, TRUE_LAND);
          printf("        %-26s %9.1f %8.2f %8.1f %8d %8ld\n",
                 nm, r.p2p_med, r.sd_med, r.period_s, r.corr, r.gross);
          (void) budget;
        }
      }
    }
    printf("        (bench reference, target=10 diluted gd_sigma: p2p 87.7 us, sd 14.15, period 10-14 s)\n");
  }

  printf("\n3k. THE PHANTOM SATURATING P: a bad phase during a board's own transient\n");
  {
    // BENCH 2026-09-03 03:46, board b, seven minutes post-reboot:
    //     GDIN raw=-8633 gd=+4317 gap=-103188 extrap=+1.42 steady=0
    //     RATEWHY p=-150.00 kp=0.156 sig=22.0 dif=-2519 dx=+0.00 dp=+0.00 clip=1
    // while the analyser had the pair at p2p 33 us, mean +1.1 us. The differential was false by
    // ~2.5 ms, P was PINNED at the -150 ppm authority clamp, and dx=dp=0 with clip=1 means the
    // command was being slew-walked toward a saturated setpoint -- which draws the near-vertical
    // cliff the wire showed, followed by the limiter walking back.
    //
    // The fault is injected on the PUBLISHED PHASE while steady=0, i.e. as the observed fact that
    // a board's phase does not describe its audio during its own transient. err_us is untouched,
    // so no audio moves and every microsecond of skew below is manufactured by the loop believing
    // a phase the board itself has already declined to broadcast.
    //
    // The signature to reproduce is not "large P" -- it is P PINNED at the clamp with the slew
    // limiter walking toward it.
    // NO STARVATION, NO CORRECTIONS: the transient here moves no audio, so the control is clean
    // and every microsecond below is the loop believing a bad phase.
    Faults pf{};
    pf.transient_period_s = 60.0; pf.transient_secs = 4.0;
    pf.phase_fault_us = 8600.0;   // the bench's raw phase error
    printf("        phase wrong by %.1f ms while steady=0; audio never moves\n", pf.phase_fault_us / 1000.0);
    printf("        %-30s %9s %8s %8s %6s %7s %7s %7s\n",
           "case", "p2p/2min", "sd", "max|P|", "corr", "frames", "decayC", "decayF");
    // LONG transients too: the bench's was post-reboot acquisition lasting MINUTES, and gd can
    // only climb at the filter's gmax per sample -- so a 4 s transient never lets dif reach the
    // 2.5 ms that pinned P at the clamp on hardware. The 40 s rows are what test that path.
    struct PkCase { const char *name; const char *tag; bool fault; bool gate; double secs; };
    const PkCase cases[] = {
      {"no fault, 4 s transient",   "pk-none",  false, false,  4.0},
      {"fault 4 s, gate off",       "pk-off",   true,  false,  4.0},
      {"fault 4 s, GATE ON",        "pk-on",    true,  true,   4.0},
      {"no fault, 40 s transient",  "pk-nl",    false, false, 40.0},
      {"fault 40 s, gate off",      "pk-offl",  true,  false, 40.0},
      {"fault 40 s, GATE ON",       "pk-onl",   true,  true,  40.0},
    };
    for (const PkCase &c : cases) {
      Profile q = p;
      q.gate_gd_on_transient = c.gate;
      Faults f = pf;
      f.transient_secs = c.secs;
      if (!c.fault) f.phase_fault_us = 0.0;
      Result r = simulate(c.tag, q, -15.0, +15.0, 40.0, 80.0, 900.0, TRUE_LAND, f);
      printf("        %-30s %9.1f %8.2f %8.1f %6d %7ld %7d %7ld  esrc d/l=%d/%d\n",
             c.name, r.p2p_med, r.sd_med, r.p_max_ppm, r.corr, r.gross,
             r.decay_corr, r.decay_frames, r.corr_from_diff, r.corr_from_dl);
    }
    printf("        (gate_gd_on_transient holds gd while the board's own phase is meaningless)\n");
  }

  printf("\n3l. A CHUNK-SIZED MEASUREMENT JUMP, and the position path's missing confirmation\n");
  {
    // BENCH 2026-09-03 04:23, board b, with ARRGAP max 130-152 ms on both boards (absorbed) so
    // no supply event:
    //
    //   04:23:05  err=-26157                                     ~= -1 chunk (26123)
    //   04:23:06  RATEWHY kp=0.000 sig=30590.8 com=+32847 dif=+0  sigma_e exploded to 30 ms
    //   04:23:08  err=+32815  act=2 why=4  frames=+1491           33.8 ms correction
    //   04:23:12  err=+6603   act=2 why=4  frames=-195            opposite sign, 4 s later
    //
    // and the wire went to -4758 us. Board a was untouched, so it reached the wire as asymmetric
    // stepping.
    //
    // THE POINT: those were ORDINARY position corrections (act=2 why=4), not hard resyncs. The
    // confirmation gate added in 646837c covers only the resync path, so the same class of
    // quantised measurement jump that can no longer trigger a resync CAN still buy a 1491-frame
    // correction. RESYNC IS DISABLED in these rows precisely to isolate that path.
    //
    // sigma_e's collapse of Kp is the second half: the estimator counts a jump as noise, so the
    // rate loop goes open-loop exactly when the error is largest, leaving position to act alone.
    printf("        resync DISABLED, so only the ordinary position path can act\n");
    printf("        %-30s %9s %8s %8s %9s %10s\n",
           "case", "p2p/2min", "sd", "corr", "frames", "sigma_e max");
    // SPAN MATTERS MORE THAN MAGNITUDE. A one-tick chunk-sized glitch is REJECTED outright: the
    // position path acts on the FILTERED error, and the filter plus jump detector threw it away
    // (0 corrections, sigma_e peaking at 1591 us against the bench's 30590). So "the position path
    // has no confirmation" was wrong -- it has the filter, and for a single sample the filter is
    // enough. The bench's error instead STAYED displaced: -26157, then +32818, +32815, +6643,
    // +10971 across seconds, which the filter is designed to follow.
    struct MjCase { const char *name; const char *tag; double glitch; double span; };
    const MjCase cases[] = {
      {"no glitch (control)",          "mj-none",      0.0, 0.0},
      {"1 chunk, 1 tick",              "mj-c1t",   26123.0, 0.0},
      {"1 chunk, 3 s sustained",       "mj-c3s",   26123.0, 3.0},
      {"2 chunks, 3 s sustained",      "mj-2c3s",  52246.0, 3.0},
      {"1 chunk, 10 s sustained",      "mj-c10s",  26123.0, 10.0},
      // THE BENCH'S ACTUAL STATE: no differential (peer silent) AND a sustained deadline offset.
      {"1 chunk 3 s + PEER SILENT",    "mj-c3sp",  26123.0, 3.0},
      {"2 chunks 3 s + PEER SILENT",   "mj-2c3sp", 52246.0, 3.0},
    };
    for (const MjCase &c : cases) {
      Faults f{};
      f.resync_us = 0.0;                 // isolate the ordinary position path
      f.glitch_period_s = 90.0;
      f.glitch_us = c.glitch;
      f.glitch_span_s = c.span;
      f.resync_on_measurement = false;
      if (std::string(c.tag).find("p") == std::string(c.tag).size() - 1) {
        f.peer_silent_period_s = 90.0;   // aligned with the glitch, and wider so dif is absent
        f.peer_silent_secs = 20.0;   // > GROUP_DELTA_STALE_US (15 s), so the HELD delta expires and g.present goes false
      }
      Result r = simulate(c.tag, p, -15.0, +15.0, 40.0, 80.0, 900.0, TRUE_LAND, f);
      printf("        %-30s %9.1f %8.2f %8d %9ld %10.0f\n",
             c.name, r.p2p_med, r.sd_med, r.corr, r.gross, r.sigma_e_max);
    }
    printf("        (a jump the resync gate would now reject can still buy a 1491-frame step)\n");
  }

  printf("\n4. THE BENCH'S OWN FAULTS: half the observations missing, and audio moved\n");
  printf("   behind the engine's back by a resync path that never tells it\n");
  {
    // This is the case the simulator could not express, and the reason its numbers disagreed with
    // hardware. On the bench: NoEvidence=51/96 on board a, hard resyncs every ~80 s, crystals
    // winding 15-26 ppm/min to 280 ppm apart, |ef| in milliseconds. In here, previously:
    // skew median 19 us, p90 34 us, zero corrections. One of those is wrong about the same system.
    // Each fault on its own, then together, so the dominant one is visible rather than inferred.
    // `tag` is the CSV label and `name` the printed one: two of the display names contain a
    // comma, which would split into a phantom column and shift every field after it.
    struct Case { const char *name; const char *tag; Faults f; };
    Faults none{}, blind{}, resync{}, both{};
    blind.blind_fraction = 0.5;                                     // as measured on a
    resync.resync_us = 50000.0;                       // the client's hard_resync threshold, 50 ms
    both = blind; both.resync_us = 50000.0;
    // The bench's own cadences: starvation bursts every ~119 s, re-anchors on the order of the
    // 428 ms consensus spread, and a 50 ms resync threshold that only now gets reached.
    Faults starve{};  starve.starve_period_s = 119.0; starve.starve_secs = 3.0;
                      starve.resync_us = 50000.0;
    Faults anchor{};  anchor.reanchor_period_s = 180.0; anchor.reanchor_us = 120000.0;
                      anchor.resync_us = 50000.0;
    Faults all{};     all = starve; all.reanchor_period_s = 180.0; all.reanchor_us = 120000.0;
                      all.blind_fraction = 0.5;
    Faults all_told = all; all_told.announce_resync = true;
    const Case cases[] = {{"clean (as before)", "clean", none},
                          {"50% observations missing", "blind50", blind},
                          {"starvation bursts", "starve", starve},
                          {"timebase re-anchors", "reanchor", anchor},
                          {"all three, UNANNOUNCED resync", "all-blind", all},
                          {"all three, ANNOUNCED resync", "all-told", all_told}};
    printf("        %-28s %10s %10s %8s %9s\n", "faults", "skew med", "skew p90", "corr", "frames");
    for (const Case &c : cases) {
      Result r = simulate(c.tag, p, -1.5, +1.5, 40.0, 80.0, 900.0, TRUE_LAND, c.f);
      printf("        %-28s %9.0fu %9.0fu %8d %9ld\n", c.name, r.skew_med, r.skew_p90,
             r.corr, r.gross);
    }
    // Deliberately not asserted yet. The point of this case is to REPRODUCE the hardware, and
    // until the numbers here look like the numbers there, the simulator is still telling a
    // comfortable story and the fix would be aimed at the wrong thing.
    printf("        (bench for comparison: |ef| 1936-10418 us, crystals winding 15-26 ppm/min)\n");
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all properties hold", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
