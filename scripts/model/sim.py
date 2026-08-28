"""A runnable model of the snapclient timing system.

WHAT IS MODELLED, and why each piece has to be here:

  * three clocks -- server, the AP's shared TSF, and each device's esp_timer -- plus a
    per-device DAC clock that the servo steers. Only RELATIVE quantities are audible, so
    everything is carried as an offset against server time.
  * the leaderless consensus: each device slews its PUBLISHED server<->TSF line toward its
    own Kalman estimate at <=50 us/s, everyone adopts the robustly weighted mean of all
    published lines, and the adoption itself slews at <=100 us/s. Set leaderless=False to
    get the old behaviour (everyone adopts one device's line verbatim) for comparison.
  * the plant: pushes are paced by the server (one chunk per chunk period), the DAC drains
    at its own trimmed rate, so QUEUE DEPTH INTEGRATES the rate error. The plant is an
    integrator -- that is why a stepping corrector limit-cycles and continuous PI does not.
  * the feedback path: played-frame callbacks at 10 ms with frame quantisation, the
    alpha=1/64 pivot EWMA, and the 31-chunk median. Together these are the ~0.85 s of loop
    lag that bounds the gain.
  * the controller: continuous PI on the median error with the derived clamp, conditional
    integration, Ki = Kp^2/4.
  * the instruments, each computed from exactly the terms the real one has:
      - `truth`      : where the audio actually renders (the logic analyser's quantity)
      - `sync_median`: the on-device tracking error (blind to its own reference)
      - `phase`      : render_phase / RAW-line arithmetic, INCLUDING its accounting inputs
    so a model fault (a frame of accounting bias, a wrong anchor) shows up in the
    instruments exactly as it would on the bench.

WHAT IS NOT MODELLED: decode, TCP delivery and starvation, hard resyncs, splice fallback,
the mixer wedge. This model is for the steady state where the bench measurements were
taken (correction off, no resyncs, rate lock steering).

Sign conventions, fixed once:
  * ppm > 0 means the clock runs FAST.
  * a displacement > 0 means LATE.
  * every inter-device quantity is B - A, matching the analyser's own convention.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

import params as P
import stats


@dataclass
class Trace:
    """Per-chunk record, one row per chunk per device."""
    t_s: list = field(default_factory=list)          # server time, seconds
    truth_us: list = field(default_factory=list)     # true render displacement (late = +)
    phase_us: list = field(default_factory=list)     # render_phase, as the firmware computes it
    err_us: list = field(default_factory=list)       # servo error (predicted - deadline)
    median_us: list = field(default_factory=list)    # the 31-chunk median it steers on
    trim_ppm: list = field(default_factory=list)
    depth_us: list = field(default_factory=list)     # (pushed - played) in us
    map_err_us: list = field(default_factory=list)   # adopted timebase error
    kal_err_us: list = field(default_factory=list)   # own Kalman error (what it publishes from)

    def arrays(self):
        return {k: np.asarray(v, dtype=float) for k, v in self.__dict__.items()}


class Device:
    def __init__(self, dp: P.DeviceParams, sp: P.SimParams, rng: np.random.Generator):
        self.p = dp
        self.sp = sp
        self.rng = rng

        # --- clocks. crystal_ppm is the board's own TSF-vs-esp_timer figure (what it
        # publishes as `mine`), so esp_timer against SERVER is that minus the TSF-vs-server
        # rate the whole group measures identically.
        self.local_ppm = P.TSF_VS_SERVER_PPM - dp.crystal_ppm
        self.local0 = rng.uniform(-1e6, 1e6)
        # The DAC hangs off the same crystal, so it inherits the same rate error; the trim
        # is the only thing that separates them.
        self.trim_ppm = 0.0

        # --- plant
        self.depth0_us = dp.pipeline_us
        self.pushed = 0.0                 # frames, absolute (audio time = pushed * FRAME_US)
        self.played_true = 0.0            # frames actually clocked out (continuous)
        self.k0 = 64                      # chunk index at which the window starts

        # --- feedback path
        self.played_int = 0
        self.played_ts = 0.0              # local time of the last callback
        self.fb_mean_frames = 0.0
        self.fb_mean_ts = 0.0
        self.fb_n = 0
        self.next_fb_s = 0.0

        # --- servo
        self.err_window = []
        self.trim_integral = 0.0
        self.clamp = P.trim_clamp_ppm(P.CONVERGE_FINE_US, dp.kp)

        # --- timebase. Everything is carried as an ERROR against the true server<->TSF
        # relation, in us: 0 means a perfect mapping.
        self.pub_valid = True             # a settled Kalman: publishes a mapping, not phase only
        self.consensus_n = 1
        self.kal_err = 0.0                # own Kalman estimate error (OU, per device)
        self.pub_err = 0.0                # slew-limited published line
        self.map_err = 0.0                # adopted consensus
        self.map_last_s = 0.0
        self.pub_last_s = 0.0
        self.offset_filter = None         # EWMA state of the offset the deadline uses
        self.offset_filter_valid = False
        self.offset_filter_s = 0.0        # local instant the filter state describes
        # PEER VIEWS. Each device knows only what has ARRIVED: a peer's published line as of
        # the last beacon it received, up to PEER_MAP_STALE_US old, and nothing at all when
        # beacons are being lost. This asymmetry of information is the only thing that can
        # make two devices adopt DIFFERENT consensus values, and a differential timebase
        # error is the only kind that is audible -- so it is modelled explicitly rather
        # than assumed away.
        self.peer_view = {}               # name -> (pub_err, received_at_s)
        self.beacon_phase_us = rng.uniform(0.0, P.BEACON_INTERVAL_US)
        self.next_beacon_s = None

        self.trace = Trace()

    # ---------------------------------------------------------------- clocks
    def local_of_server(self, s: float) -> float:
        return self.local0 + (1.0 + self.local_ppm * 1e-6) * s

    def tsf_of_server(self, s: float) -> float:
        # One shared counter; an arbitrary epoch, since only differences are used.
        return 1e9 + (1.0 + P.TSF_VS_SERVER_PPM * 1e-6) * s

    def dac_ppm(self) -> float:
        return self.local_ppm + self.trim_ppm

    # ---------------------------------------------------------------- plant
    def seed(self, s: float):
        """Steady state at server time s: depth = pipeline, displacement = 0."""
        self.pushed = (self.k0 * P.CHUNK_FRAMES)
        self.played_true = self.pushed - self.depth0_us / P.FRAME_US
        self.played_int = int(math.floor(self.played_true))
        self.played_ts = self.local_of_server(s)
        self.fb_mean_frames = self.played_true
        self.fb_mean_ts = self.played_ts
        self.fb_n = P.FB_MIN_SAMPLES
        self.next_fb_s = s + self.p.feedback_interval_us

    def drain(self, s_from: float, s_to: float):
        """Advance the DAC, firing played-frame callbacks on the way."""
        s = s_from
        while True:
            s_next = min(self.next_fb_s, s_to)
            dt = s_next - s
            if dt > 0:
                self.played_true += dt / P.FRAME_US * (1.0 + self.dac_ppm() * 1e-6)
            s = s_next
            if s >= s_to - 1e-9:
                break
            # a callback lands here
            self.played_int = int(math.floor(self.played_true))
            self.played_ts = self.local_of_server(s)
            f, t = float(self.played_int), self.played_ts
            if self.fb_n == 0:
                self.fb_mean_frames, self.fb_mean_ts = f, t
            else:
                self.fb_mean_frames += P.FB_ALPHA * (f - self.fb_mean_frames)
                self.fb_mean_ts += P.FB_ALPHA * (t - self.fb_mean_ts)
            self.fb_n += 1
            self.next_fb_s = s + self.p.feedback_interval_us

    # ---------------------------------------------------------------- timebase
    def step_kalman(self, value: float):
        self.kal_err = value

    def step_publish(self, s: float):
        """Slew the published line toward our own raw Kalman estimate (never the consensus)."""
        dt_s = (s - self.pub_last_s) / 1e6
        self.pub_last_s = s
        d = self.kal_err - self.pub_err
        rate = P.TMS_SLEW_CATCHUP_US_PER_S if abs(d) > P.TMS_CATCHUP_THRESHOLD_US else P.TMS_SLEW_US_PER_S
        step = max(1.0, rate * dt_s)
        self.pub_err += max(-step, min(step, d))

    def consensus_target(self, s: float) -> float:
        """The robustly weighted mean of every live line WE HOLD: our own published line
        (always fresh) plus each peer's as last received. Never the adopted consensus --
        feeding that back is positive feedback with gain 1 (tsf_sync.cpp, update_consensus_)."""
        vals = [self.pub_err] if self.pub_valid else []
        for _name, (v, t_rx) in self.peer_view.items():
            if s - t_rx <= P.PEER_MAP_STALE_US:
                vals.append(v)
        if not vals:
            return None
        self.consensus_n = len(vals)
        return stats.robust_mean(vals, P.CONSENSUS_SCALE_FLOOR_US, P.CONSENSUS_REWEIGHT_K)

    def adopt_consensus(self, s: float, target: float):
        dt_s = (s - self.map_last_s) / 1e6
        self.map_last_s = s
        d = target - self.map_err
        if abs(d) > P.MAP_SNAP_US:
            self.map_err = target          # a re-anchor: the timebase genuinely steps
            return True
        rate = P.MAP_SLEW_CATCHUP_US_PER_S if abs(d) > P.MAP_CATCHUP_THRESHOLD_US else P.MAP_SLEW_US_PER_S
        step = max(1.0, rate * dt_s)
        self.map_err += max(-step, min(step, d))
        return False

    def shared_offset_est(self, s: float) -> float:
        """What the deadline is computed from: the adopted mapping evaluated at a fresh TSF
        sandwich, FEED-FORWARDED by the measured local-vs-TSF rate, then low-passed at
        alpha = 1/256. Returns the ESTIMATE of (server - local).

        The feed-forward is not optional detail. (server - local) ramps at this board's whole
        crystal offset -- ~62 ppm, i.e. 62 us per second -- and the EWMA's time constant is
        256 chunks ~ 6.7 s, so WITHOUT it the filter trails the ramp by ~400 us and the model
        shows a 6.5 us/ppm standing displacement that the real firmware does not have
        (tsf_sync.cpp:1340). What survives is the filter trailing only the RATE ESTIMATE'S
        error, which is what the firmware's "340 us residual" note refers to.
        """
        true_offset = s - self.local_of_server(s)
        raw = true_offset + self.map_err + self.rng.normal(0.0, self.p.sandwich_jitter_us)
        if not self.offset_filter_valid:
            self.offset_filter, self.offset_filter_valid = raw, True
            self.offset_filter_s = s
            return self.offset_filter
        gap = min(max(s - self.offset_filter_s, 0.0), P.OFFSET_FF_MAX_GAP_US)
        self.offset_filter_s = s
        # True ramp of (server - local) is -local_ppm; the board knows it to within
        # offset_rate_err_ppm (it measures it over 8 s windows, EWMA 0.25).
        ff_ppm = -self.local_ppm + self.p.offset_rate_err_ppm
        self.offset_filter += ff_ppm * 1e-6 * gap
        if abs(raw - self.offset_filter) > P.OFFSET_SNAP_US:
            self.offset_filter = raw
        else:
            self.offset_filter += P.OFFSET_EWMA_ALPHA * (raw - self.offset_filter)
        return self.offset_filter

    # ---------------------------------------------------------------- servo
    def servo(self, s: float, s_ts: float, offset_est: float) -> tuple:
        nominal = P.FRAME_US
        predicted = self.fb_mean_ts + nominal * (self.pushed - self.fb_mean_frames)
        buffer_us = self.sp.buffer_ms * 1000.0 - self.p.static_delay_us
        deadline = s_ts + buffer_us - offset_est
        err = predicted - deadline

        self.err_window.append(err)
        if len(self.err_window) > P.MEDIAN_WINDOW:
            self.err_window.pop(0)
        median = float(np.median(self.err_window)) if len(self.err_window) == P.MEDIAN_WINDOW else err

        # Continuous PI, no deadband; Ki tracks the active Kp (critical damping).
        kp = self.p.kp
        dt_s = P.CHUNK_FRAMES / P.SAMPLE_RATE
        p_term = kp * median
        unclamped = p_term + self.trim_integral
        if abs(unclamped) < self.clamp or (unclamped > 0) != (median > 0):
            self.trim_integral = max(-self.clamp,
                                     min(self.clamp,
                                         self.trim_integral + P.trim_ki(kp) * median * dt_s))
        # error > 0 means this chunk would play LATE, so the DAC must run FASTER to drain the
        # queue sooner: trim follows +error, exactly as snapcast_client.cpp:2716 does.
        self.trim_ppm = max(-self.clamp, min(self.clamp, p_term + self.trim_integral))
        return err, median, predicted, deadline

    # ---------------------------------------------------------------- instruments
    def truth_us(self, s: float) -> float:
        """Where the audio actually is: displacement of the rendering frame, late = +.
        This is the logic analyser's quantity (differenced between two boards)."""
        ideal_audio_us = s - self.sp.buffer_ms * 1000.0
        return ideal_audio_us - self.played_true * P.FRAME_US

    def render_phase_us(self, s: float, s_ts: float) -> float:
        """render_phase, computed from the same terms the firmware uses and nothing else:

            render_tsf    = played_ts + (tsf - tsf_local)
            render_server = s_ts - (pushed - played) * 1e6 / rate
            phase         = render_tsf - render_server

        Absolute value is meaningless; only differences between devices are.
        """
        tsf = self.tsf_of_server(s) + self.rng.normal(0.0, self.p.sandwich_jitter_us)
        tsf_local = self.local_of_server(s)
        played_reported = self.played_int + self.p.accounting_bias_frames
        render_tsf = self.played_ts + (tsf - tsf_local)
        render_server = s_ts - (self.pushed - played_reported) * P.FRAME_US
        return render_tsf - render_server


class Sim:
    def __init__(self, sp: P.SimParams):
        self.sp = sp
        self.rng = np.random.default_rng(sp.seed)
        self.devs = [Device(dp, sp, np.random.default_rng(sp.seed * 977 + i))
                     for i, dp in enumerate(sp.devices)]

    def run(self):
        sp = self.sp
        n_chunks = int(sp.duration_s * 1e6 / P.CHUNK_US)
        # Kalman wander: one independent OU path per device, sampled per chunk. This is the
        # error TSF exists to turn common-mode; it is what drives everything below.
        wander = [stats.ou_series(n_chunks, P.CHUNK_US / 1e6, d.p.kalman_wander_sd_us,
                                 P.KALMAN_WANDER_TAU_S, self.rng)
                  for d in self.devs]

        # Chunk k's audio timestamp, and the server instant its push is processed at: the
        # push runs one pipeline-depth ahead of that chunk's render deadline, which is what
        # makes the steady state depth == pipeline and the initial displacement zero.
        depth0 = self.devs[0].depth0_us
        buffer_us = sp.buffer_ms * 1000.0
        k0 = self.devs[0].k0

        def s_ts_of(k):
            return (k0 + k) * P.CHUNK_FRAMES * P.FRAME_US

        def s_of(k):
            return s_ts_of(k) + buffer_us - depth0

        s0 = s_of(0)
        for d in self.devs:
            d.seed(s0)
            d.pub_err = wander[0][0] * 0.0     # published line starts converged
            d.map_err = 0.0
            d.pub_last_s = d.map_last_s = s0

        for d in self.devs:
            d.next_beacon_s = s0 + d.beacon_phase_us
        next_consensus = s0 + P.CONSENSUS_INTERVAL_US
        s_prev = s0
        reanchors = 0
        lost = 0

        for k in range(n_chunks):
            s = s_of(k)
            s_ts = s_ts_of(k)

            for i, d in enumerate(self.devs):
                d.drain(s_prev, s)
                d.step_kalman(wander[i][k])

            # BEACONS. Each device publishes on its own phase of the 1 s cadence, and each
            # peer either receives it or does not -- multicast between stations is lossy on
            # real APs, which is why the firmware also unicasts.
            for d in self.devs:
                if s < d.next_beacon_s:
                    continue
                d.next_beacon_s += P.BEACON_INTERVAL_US
                d.step_publish(s)
                if not d.pub_valid:
                    continue          # phase-only report: carries no mapping
                for other in self.devs:
                    if other is d:
                        continue
                    if self.rng.random() < sp.beacon_loss:
                        lost += 1
                        continue
                    other.peer_view[d.p.name] = (d.pub_err, s)

            if s >= next_consensus:
                next_consensus += P.CONSENSUS_INTERVAL_US
                if sp.leaderless:
                    for d in self.devs:
                        target = d.consensus_target(s)
                        if target is not None:
                            reanchors += bool(d.adopt_consensus(s, target))
                else:
                    # LEADER ERA: everyone plays to ONE device's published line. The leader
                    # itself adopts its own line the instant it publishes; a follower adopts
                    # the copy that reached it, so the follower's timebase LAGS the leader's
                    # by the beacon interval plus its loss run -- an asymmetry consensus does
                    # not have, because under consensus every device lags equally.
                    lead = self.devs[0]
                    for d in self.devs:
                        if d is lead:
                            target = lead.pub_err
                        else:
                            v = d.peer_view.get(lead.p.name)
                            if v is None or s - v[1] > P.PEER_MAP_STALE_US:
                                continue
                            target = v[0]
                        reanchors += bool(d.adopt_consensus(s, target))

            for d in self.devs:
                offset_est = d.shared_offset_est(s)
                err, median, _pred, _dl = d.servo(s, s_ts, offset_est)
                tr = d.trace
                tr.t_s.append(s / 1e6)
                tr.truth_us.append(d.truth_us(s))
                tr.phase_us.append(d.render_phase_us(s, s_ts))
                tr.err_us.append(err)
                tr.median_us.append(median)
                tr.trim_ppm.append(d.trim_ppm)
                tr.depth_us.append((d.pushed - d.played_true) * P.FRAME_US)
                tr.map_err_us.append(d.map_err)
                tr.kal_err_us.append(d.kal_err)
                # push AFTER measuring, matching the firmware's order (the servo and the RAW
                # line both read `pushed` before push_chunk_ runs)
                d.pushed += P.CHUNK_FRAMES

            s_prev = s

        self.beacons_lost = lost
        self.reanchors = reanchors
        return {d.p.name: d.trace.arrays() for d in self.devs}


def two_board_bench(duration_s: float = 240.0, seed: int = 1, leaderless: bool = True,
                    n_devices: int = 2, **dev_kwargs) -> P.SimParams:
    """The bench: boards A and B (the analyser's two channels), optionally the observer."""
    names = ["a", "b", "obs"][:n_devices]
    devs = []
    for n in names:
        devs.append(P.DeviceParams(name=n,
                                   crystal_ppm=P.CRYSTAL_PPM.get(n, 41.0),
                                   **dev_kwargs))
    return P.SimParams(devices=devs, duration_s=duration_s, seed=seed, leaderless=leaderless)


def skew(out: dict, a: str = "a", b: str = "b", field: str = "truth_us"):
    """B - A of a per-device series, on the shared chunk grid."""
    return out[b][field] - out[a][field]
