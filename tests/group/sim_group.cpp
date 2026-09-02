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
  explicit Board(const Profile &p) : eng(p) {}
};

struct Result {
  double skew_med = 0.0, skew_p90 = 0.0;
  int corr = 0;
  long gross = 0;
  double err_med = 0.0;
};

/// `common_ppm` drifts every board's deadline together -- a server/timebase offset, which is what
/// the bench sees: gd of tens of us while each board sits hundreds of us off its own deadline.
Result simulate(Profile p, double plant_a, double plant_b, double common_ppm,
                double noise_us, double seconds, int64_t true_land_us) {
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

      // measured against its OWN deadline, which the common-mode term has moved
      const double measured = x.err_us + deadline_shift;
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

      Command c = x.eng.step(t, Observation{t, static_cast<int64_t>(std::llround(seen)), true}, g);
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

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all properties hold", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
