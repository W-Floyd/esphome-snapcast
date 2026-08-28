"""Closed forms for the quantities the simulator produces numerically.

These exist so a claim can be checked two ways. Anything the analytic expression and the
simulation disagree about is a bug in one of them, and both have been wrong at least once.
"""

from __future__ import annotations

import math

import params as P


# ---------------------------------------------------------------- the prediction lever

def pivot_lever_us(depth_us: float = 257_000.0,
                   fb_alpha: float = P.FB_ALPHA,
                   fb_interval_us: float = P.FEEDBACK_INTERVAL_US) -> float:
    """Time from the feedback pivot to the frame being predicted.

        lever = queue depth + EWMA lag,      EWMA lag = (1/alpha - 1) * callback interval

    `predicted` extrapolates from the pivot along the EXACT NOMINAL sample period, so any
    real rate offset (crystal + trim) becomes a prediction error of lever * ppm. That error
    is a real DISPLACEMENT, because the loop moves audio until the prediction hits the
    deadline -- the loop's blind spot in one line.
    """
    return depth_us + (1.0 / fb_alpha - 1.0) * fb_interval_us


def displacement_per_ppm(depth_us: float = 257_000.0) -> float:
    """us of displacement per ppm of rate offset. Measured on the bench at 1.5-1.7 us/ppm
    (joint fit of wire skew on both boards' trims); the firmware comment quotes 3.15."""
    return pivot_lever_us(depth_us) * 1e-6


# ---------------------------------------------------------------- the loop

def tracking_lag_us(disturbance_us_per_s: float, kp_ppm_per_us: float) -> float:
    """An integrator plant trails a ramp by rate/gain. The Kalman offset wanders ~100 us/s
    on wifi, so Kp = 0.5 leaves ~200 us of standing tracking error -- which is why gain is
    set by disturbance tracking and not by settling time."""
    return disturbance_us_per_s / kp_ppm_per_us


def loop_lag_us(median_window: int = P.MEDIAN_WINDOW,
                fb_alpha: float = P.FB_ALPHA,
                fb_interval_us: float = P.FEEDBACK_INTERVAL_US) -> float:
    """Measurement lag: the pivot EWMA plus half the median window."""
    return (1.0 / fb_alpha) * fb_interval_us + 0.5 * median_window * P.CHUNK_US


def crossover_rad_per_s(kp_ppm_per_us: float) -> float:
    """The plant is an integrator: depth (us of timing error) integrates rate error (ppm).
    d(err)/dt = -1e-6 * trim = -1e-6 * Kp * err, so the loop's own corner is at
    1e-6 * Kp rad/us."""
    return kp_ppm_per_us * 1e-6 * 1e6            # rad/s


def phase_margin_deg(kp_ppm_per_us: float, lag_us: float = None) -> float:
    """Integrator + pure delay: margin = 90 deg - w_c * lag."""
    lag_us = loop_lag_us() if lag_us is None else lag_us
    wc = crossover_rad_per_s(kp_ppm_per_us)       # rad/s
    return 90.0 - math.degrees(wc * lag_us / 1e6)


def limit_cycle_amplitude_us(step_us: float, slew_us_per_s: float, lag_s: float) -> float:
    """A stepping/bang-bang corrector on an integrator plant cannot settle: it overshoots by
    the slew accumulated over one loop delay. Amplitude ~ step + slew * lag."""
    return step_us + slew_us_per_s * lag_s


# ---------------------------------------------------------------- noise floors

def quantisation_sd_us(quantum_us: float = P.FRAME_US) -> float:
    """Uniform quantiser: sd = q/sqrt(12). The played-frames feedback is integral frames, so
    the pair (played, played_ts) is inconsistent by up to one frame."""
    return quantum_us / math.sqrt(12.0)


def differential(sd_us: float) -> float:
    """Two independent devices differenced: sd * sqrt(2). Every inter-device figure on the
    bench is a difference, so this factor is in all of them."""
    return sd_us * math.sqrt(2.0)


def consensus_sd_us(per_device_sd_us: float, n: int, shared_fraction: float = 0.0) -> float:
    """Consensus averaging drops the independent part of the mapping noise as sqrt(N); a
    shared component does not average at all. shared_fraction is the fraction of VARIANCE
    that every device sees identically."""
    var = per_device_sd_us ** 2
    return math.sqrt(var * shared_fraction + var * (1.0 - shared_fraction) / n)


def error_budget_us(placement_cm: float = 0.0) -> dict:
    """TIMING.md section 5, as arithmetic rather than a table."""
    terms = {
        "tsf read noise (per device)": P.SANDWICH_JITTER_US,
        "published mapping error": 0.0,                     # common-mode by construction
        "played-frame quantisation": quantisation_sd_us(),
        "servo residual (white)": 200.0 / math.sqrt(P.MEDIAN_WINDOW),
        "speaker placement": 29.0 * placement_cm,
    }
    indep = sum(v * v for k, v in terms.items() if "placement" not in k)
    terms["-- rss, one device"] = math.sqrt(indep)
    terms["-- rss, differential"] = differential(math.sqrt(indep))
    return terms


# ---------------------------------------------------------------- the instrument floor

def tracked_ratio(displacement_us: float, floor_us: float = 20.0) -> tuple:
    """What an instrument with an ADDITIVE floor reports as a fraction of a displacement.

    This is why "percent tracked" looked erratic: percent is the wrong statistic against a
    constant error. At 500 ms the floor is 0.002% and the ratio reads 1.0000; at 25 us the
    floor is 80-120% of the signal and the SIGN is arbitrary.
    """
    lo = (displacement_us - floor_us) / displacement_us
    hi = (displacement_us + floor_us) / displacement_us
    return lo, hi


def floor_frames(floor_us: float = 20.0) -> float:
    """The observed 10-30 us floor, in frames. Landing within one frame of 1.0 is the
    argument for a whole-frame accounting difference being the mechanism."""
    return floor_us / P.FRAME_US


if __name__ == "__main__":
    print(f"pivot lever            {pivot_lever_us()/1e6:.3f} s "
          f"-> {displacement_per_ppm():.2f} us/ppm  (bench joint fit 1.5-1.7, "
          f"firmware comment 3.15)")
    print(f"loop measurement lag   {loop_lag_us()/1e6:.3f} s   (TIMING.md: 0.85 s)")
    for kp in (0.05, 0.1, 0.25, 0.5, 1.0):
        print(f"  Kp {kp:4.2f}: tracking lag vs 100 us/s wander "
              f"{tracking_lag_us(100.0, kp):7.1f} us   phase margin "
              f"{phase_margin_deg(kp):5.1f} deg   Ki {P.trim_ki(kp):.4f}")
    print(f"trim clamp             {P.trim_clamp_ppm():.0f} ppm "
          f"(derived: Kp x converge_fine = {P.TRIM_KP_ACQUIRE*P.CONVERGE_FINE_US:.0f})")
    print(f"frame quantisation sd  {quantisation_sd_us():.2f} us per device, "
          f"{differential(quantisation_sd_us()):.2f} us differential")
    print(f"observed floor         {floor_frames(20.0):.2f} frames "
          f"(10-30 us = {floor_frames(10):.2f}-{floor_frames(30):.2f} frames)")
    print("error budget (us):")
    for k, v in error_budget_us().items():
        print(f"  {k:32s} {v:7.2f}")
    print(f"consensus mapping noise, per-device sd 150 us:")
    for n in (1, 2, 3, 4):
        print(f"  n={n}: fully independent {consensus_sd_us(150.0, n):6.1f} us   "
              f"half shared {consensus_sd_us(150.0, n, 0.5):6.1f} us")
    for d in (25.0, 100.0, 1000.0, 500000.0):
        lo, hi = tracked_ratio(d)
        print(f"instrument ratio at {d:9.0f} us displacement: {lo:+.4f}..{hi:+.4f}")
