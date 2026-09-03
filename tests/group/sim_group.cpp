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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
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
  long gross_frames = 0;
  double phase_us = 0.0;    // what it publishes
  int64_t phase_at_us = 0;  // and when it sampled it -- pairing needs both
  int64_t last_beacon_us = -1000000;
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
  explicit Board(const Profile &p) : eng(p) {}
};

/// Per-tick skew dump, opened from SIM_SKEW_CSV. Null unless asked for, so the suite is unchanged
/// by default. g_scenario labels the rows so one file holds every scenario.
static FILE *g_skew_csv = nullptr;
static const char *g_scenario = "sim";

struct Result {
  double skew_med = 0.0, skew_p90 = 0.0;
  int corr = 0;
  long gross = 0;
  double err_med = 0.0;
  // OSCILLATION. skew_med/p90 are order statistics: they cannot tell a loop RINGING at +-50 us
  // from one sitting quietly at 50 us, and the bench does the first while this simulator reported
  // the second and passed. The rate loop is fully closed here (err_us integrates plant - rate_ppm
  // every tick, against the real timing_engine.cpp), so a limit cycle IS simulated -- it was only
  // ever invisible, because the series was stored as |skew| and a sign is what a crossing needs.
  double period_s = 0.0;    // median interval between upward crossings of the median
  double amp_p05 = 0.0;     // signed, so a biased ring is distinguishable from a centred one
  double amp_p95 = 0.0;
  int n_periods = 0;        // fewer than ~6 and the period is not evidence (bench: 5 was not)
  // CRYSTAL WIND-UP. The integral cannot tell "my oscillator runs fast" from "I am behind because
  // the audio did not arrive" -- both are a persistent one-signed error. On the bench 2026-09-02 a
  // starved board wound a hand-zeroed crystal to +192 ppm in twelve minutes against a true +46.
  // Reported per board because the whole point is that it should stay near the PLANT rate.
  double xtal_a = 0.0, xtal_b = 0.0;
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
Result simulate(Profile p, double plant_a, double plant_b, double common_ppm,
                double noise_us, double seconds, int64_t true_land_us, Faults f = {}) {
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
  gd_ticks = 0; gd_present_ticks = 0; gd_fresh_ticks = 0; gd_age_sum_us = 0.0;
  double deadline_shift = 0.0;   // common-mode: moves both deadlines together

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
        // What the beacon carries: this board's phase AND the instant it was sampled.
        for (size_t j = 0; j < b.size(); j++) {
          if (j == i) continue;
          b[j].heard_phase_us[i] = b[i].phase_us;
          b[j].heard_at_us[i] = t;
          b[j].heard_valid[i] = true;
        }
      }
    }

    for (size_t i = 0; i < b.size(); i++) {
      Board &x = b[i];
      for (auto it = x.inflight.begin(); it != x.inflight.end();) {
        if (t >= it->first) { x.err_us -= double(it->second) * double(p.frame_us()); it = x.inflight.erase(it); }
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
      const double nominal_buf_us = 1724000.0;
      if (starved) {
        x.buffer_us = std::max(0.0, x.buffer_us - static_cast<double>(tick));
      } else if (x.buffer_us < nominal_buf_us) {
        x.buffer_us = std::min(nominal_buf_us, x.buffer_us + 2.0 * static_cast<double>(tick));
      }
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
      // against target_position_us (20 us), so that noise alone crosses the threshold and
      // declares a purely common error "differential". Publishing it noiseless makes the gate
      // look perfect.
      x.phase_us = measured + nd(rng) * 0.25;
      x.phase_at_us = t;
      x.obs.emplace_back(t, measured + nd(rng));
      double seen = x.obs.front().second;
      while (x.obs.size() > 1 && x.obs.front().first <= t - p.measurement_lag_us) {
        seen = x.obs.front().second;
        x.obs.pop_front();
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
      if (f.resync_us > 0.0 && std::fabs(x.err_us + deadline_shift + x.own_deadline_us) > f.resync_us) {
        const double moved = x.err_us + deadline_shift + x.own_deadline_us;
        x.err_us -= moved;
        if (f.announce_resync) {
          x.eng.note_external_move(static_cast<int64_t>(std::llround(moved)), t);
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
      if (c.frames != 0 && !x.live) {
        x.pid = c.correction_id;
        x.inflight.push_back({t + true_land_us, c.frames});
        x.live = true;
        x.corrections++;
        x.gross_frames += std::abs(c.frames);
        // A DROP IS SPENT FROM THE BUFFER: dropping frames removes audio from the very thing that
        // lets playout continue. The engine caps a correction at half the buffer for this reason,
        // and that cap was unreachable here while the buffer was a constant.
        if (c.frames > 0) {
          x.buffer_us = std::max(0.0, x.buffer_us - static_cast<double>(c.frames) * p.frame_us());
        }
      }
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
        fprintf(g_skew_csv, "%s,%.6f,%.3f\n", g_scenario, static_cast<double>(t) / 1e6, skew_signed);
      }
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
  }
  std::sort(skews.begin(), skews.end());
  std::sort(errs.begin(), errs.end());
  r.skew_med = skews[skews.size()/2];
  r.skew_p90 = skews[static_cast<size_t>(0.9*skews.size())];
  r.err_med = errs[errs.size()/2];
  r.corr = b[0].corrections + b[1].corrections;
  r.gross = b[0].gross_frames + b[1].gross_frames;
  r.xtal_a = b[0].eng.crystal_ppm();
  r.xtal_b = b[1].eng.crystal_ppm();
  return r;
}

}  // namespace

int main() {
  if (const char *path = getenv("SIM_SKEW_CSV")) {
    g_skew_csv = fopen(path, "w");
    if (g_skew_csv != nullptr) {
      fprintf(g_skew_csv, "scenario,t_s,skew_us\n");
      fprintf(stderr, "dumping per-tick skew to %s\n", path);
    } else {
      fprintf(stderr, "could not open %s -- continuing without the dump\n", path);
    }
  }
  Profile p;
  p.frame_rate_hz = 44100;
  p.measurement_lag_us = 250000;
  p.position_delay_us = 1250000;
  p.target_position_us = 20;
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
    Result r = simulate(p, 0.0, 0.0, 40.0, 80.0, 600.0, TRUE_LAND);
    printf("        skew median %.0f us, p90 %.0f us; own error median %.0f us; "
           "%d corrections, %ld frames\n", r.skew_med, r.skew_p90, r.err_med, r.corr, r.gross);
    check(r.skew_med < 2.0 * static_cast<double>(p.frame_us()),
          "boards stay in sync with EACH OTHER", r.skew_med, 2.0 * p.frame_us());
    check(r.corr == 0, "and spend no frame corrections on a common error", r.corr, 0);
  }

  printf("\n2. DIFFERENTIAL: the boards disagree with each other, which IS audible\n");
  {
    Result r = simulate(p, -15.0, +15.0, 0.0, 80.0, 600.0, TRUE_LAND);
    printf("        skew median %.0f us, p90 %.0f us; %d corrections, %ld frames\n",
           r.skew_med, r.skew_p90, r.corr, r.gross);
    check(r.skew_med < 2.0 * static_cast<double>(p.frame_us()),
          "a real differential IS pulled in", r.skew_med, 2.0 * p.frame_us());
  }

  printf("\n3. both at once: common drift plus a differential split\n");
  {
    Result r = simulate(p, -15.0, +15.0, 40.0, 80.0, 600.0, TRUE_LAND);
    printf("        skew median %.0f us, p90 %.0f us; own error median %.0f us; "
           "%d corrections, %ld frames\n", r.skew_med, r.skew_p90, r.err_med, r.corr, r.gross);
    check(r.skew_med < 2.0 * static_cast<double>(p.frame_us()),
          "the differential is corrected without chasing the common part",
          r.skew_med, 2.0 * p.frame_us());
  }

  printf("\n3a. STARVATION MUST NOT WIND THE CRYSTAL\n");
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
    // 50% duty, far harsher than the bench's ~119 s cadence, because that is where the guard's
    // effect is unambiguous. Measured with the guard forced off, same seed:
    //
    //   starve  6s/40s over 1800s   on  +7.7 / +33.5    off +10.4 / +38.3    (small)
    //   starve 10s/30s over 1800s   on -60.9 / -32.6    off -60.0 / -32.5    (NO effect)
    //   starve 10s/20s over 1800s   on  +2.5 / +29.0    off +23.2 / +47.2    (~20 ppm)
    //
    // THE MIDDLE ROW IS UNEXPLAINED and is recorded rather than hidden: at that duty both boards
    // wind to 45 ppm from plant whether the hold is there or not, so something other than the
    // post-starvation catch-up is winding them and this guard does not address it. The guard is a
    // real improvement, not a solution -- board b is still 14 ppm off plant with it.
    const double plant_a = -15.0, plant_b = +15.0;
    Faults hungry{};
    hungry.starve_period_s = 20.0;
    hungry.starve_secs = 10.0;
    Result r = simulate(p, plant_a, plant_b, 40.0, 80.0, 1800.0, TRUE_LAND, hungry);
    printf("        plant %+.0f / %+.0f ppm   ->   crystal %+.1f / %+.1f ppm   (skew med %.0f us)\n",
           plant_a, plant_b, r.xtal_a, r.xtal_b, r.skew_med);
    // The crystal is an estimate OF THE PLANT. Starvation must not move it far from one.
    check(std::fabs(r.xtal_a - plant_a) < 20.0,
          "board a's crystal still estimates its plant, not the shortage", r.xtal_a, plant_a);
    check(std::fabs(r.xtal_b - plant_b) < 20.0,
          "board b's crystal still estimates its plant, not the shortage", r.xtal_b, plant_b);
    check(std::fabs(r.xtal_a) < CRYSTAL_RAIL_PPM && std::fabs(r.xtal_b) < CRYSTAL_RAIL_PPM,
          "and neither is anywhere near the rail", std::max(std::fabs(r.xtal_a), std::fabs(r.xtal_b)),
          CRYSTAL_RAIL_PPM);
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
      Result r = simulate(q, -15.0, +15.0, 40.0, 80.0, 600.0, TRUE_LAND);
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

  printf("\n4. THE BENCH'S OWN FAULTS: half the observations missing, and audio moved\n");
  printf("   behind the engine's back by a resync path that never tells it\n");
  {
    // This is the case the simulator could not express, and the reason its numbers disagreed with
    // hardware. On the bench: NoEvidence=51/96 on board a, hard resyncs every ~80 s, crystals
    // winding 15-26 ppm/min to 280 ppm apart, |ef| in milliseconds. In here, previously:
    // skew median 19 us, p90 34 us, zero corrections. One of those is wrong about the same system.
    // Each fault on its own, then together, so the dominant one is visible rather than inferred.
    struct Case { const char *name; Faults f; };
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
    const Case cases[] = {{"clean (as before)", none},
                          {"50% observations missing", blind},
                          {"starvation bursts", starve},
                          {"timebase re-anchors", anchor},
                          {"all three, UNANNOUNCED resync", all},
                          {"all three, ANNOUNCED resync", all_told}};
    printf("        %-28s %10s %10s %8s %9s\n", "faults", "skew med", "skew p90", "corr", "frames");
    for (const Case &c : cases) {
      Result r = simulate(p, -1.5, +1.5, 40.0, 80.0, 900.0, TRUE_LAND, c.f);
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
