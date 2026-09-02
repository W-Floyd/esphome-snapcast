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
#include <cstring>
#include <deque>
#include <random>
#include <vector>

using namespace esphome::snapclient::timing;

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
  double own_deadline_us = 0.0;   // this board's own deadline steps (re-anchors)
  explicit Board(const Profile &p) : eng(p) {}
};

struct Result {
  double skew_med = 0.0, skew_p90 = 0.0;
  int corr = 0;
  long gross = 0;
  double err_med = 0.0;
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
  double deadline_shift = 0.0;   // common-mode: moves both deadlines together

  for (int64_t t = 0; t < static_cast<int64_t>(seconds * 1e6); t += tick) {
    deadline_shift += common_ppm * 1e-6 * static_cast<double>(tick);

    // group delta as tsf_sync computes it: mean of all phases INCLUDING self, so a two-board
    // group reports half the true pairwise difference and the engine unhalves it.
    double mean_phase = 0.0;
    for (auto &x : b) mean_phase += x.phase_us;
    mean_phase /= static_cast<double>(b.size());

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
      x.obs.emplace_back(t, measured + nd(rng));
      double seen = x.obs.front().second;
      while (x.obs.size() > 1 && x.obs.front().first <= t - p.measurement_lag_us) {
        seen = x.obs.front().second;
        x.obs.pop_front();
      }

      GroupEvidence g{};
      g.present = true;
      g.contributors = static_cast<uint8_t>(b.size());
      g.delta_us = static_cast<int64_t>(std::llround(x.phase_us - mean_phase));

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
                                            1724000}, g);
      if (c.frames != 0 && !x.live) {
        x.pid = c.correction_id;
        x.inflight.push_back({t + true_land_us, c.frames});
        x.live = true;
        x.corrections++;
        x.gross_frames += std::abs(c.frames);
      }
      x.err_us += (x.plant_ppm - c.rate_ppm) * 1e-6 * double(tick);
    }
    if (t > 120000000) {
      skews.push_back(std::fabs(b[1].err_us - b[0].err_us));
      errs.push_back(std::fabs(b[0].err_us + deadline_shift));
    }
  }
  std::sort(skews.begin(), skews.end());
  std::sort(errs.begin(), errs.end());
  Result r;
  r.skew_med = skews[skews.size()/2];
  r.skew_p90 = skews[static_cast<size_t>(0.9*skews.size())];
  r.err_med = errs[errs.size()/2];
  r.corr = b[0].corrections + b[1].corrections;
  r.gross = b[0].gross_frames + b[1].gross_frames;
  return r;
}

}  // namespace

int main() {
  Profile p;
  p.frame_rate_hz = 44100;
  p.measurement_lag_us = 250000;
  p.position_delay_us = 1250000;
  p.target_position_us = 20;
  p.rate_authority_ppm = 100.0f;
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
