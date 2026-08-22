#!/usr/bin/env python3
"""True inter-device playout offset, from raw observations only.

Every timing metric the firmware reports is measured against that device's own
*predicted* playout, so a modelling error displaces the audio and the metric
together and reads as zero. That blind spot produced three wrong diagnoses of an
audible offset: each looked consistent on-device while the speakers were plainly
not aligned. This script avoids the model entirely.

It consumes the `RAW` lines the client emits once per sync report:

    RAW s_ts=<server chunk us> pushed=<frames> played=<frames>
        played_ts=<local us> tsf=<us> tsf_local=<us> sw=<us> rate=<Hz>

and uses only direct observations:

  * (played, played_ts) is ground truth from the DAC feedback -- that many frames
    HAD rendered at that local time.
  * (s_ts, pushed) anchors the frame count to server audio time, which is the same
    number on every device for the same audio.
  * (tsf, tsf_local) converts local time into the AP's TSF counter, the only clock
    the devices provably share.

Giving, per sample:

    server_time_of_last_rendered_frame = s_ts - (pushed - played) * 1e6 / rate
    tsf_time_of_that_frame             = played_ts + (tsf - tsf_local)

Two devices rendering the same audio in sync satisfy the same linear relation
between those two quantities. Fitting each device and differencing at a common
server time gives the real offset, in microseconds, with the servo and the
prediction model out of the measurement.

    python3 scripts/raw-sync.py a.log b.log c.log d.log

`sw` (the sandwich bracket width) bounds the confidence of each sample: half of it
is the uncertainty in the local<->TSF pairing. Samples with a wide bracket are
reported separately rather than silently averaged in.
"""

import argparse
import itertools
import os
import re
import statistics
import sys

RAW_RE = re.compile(
    r"RAW s_ts=(-?\d+) pushed=(-?\d+) played=(-?\d+) played_ts=(-?\d+) "
    r"tsf=(-?\d+) tsf_local=(-?\d+) sw=(-?\d+) rate=(\d+)"
)

# The bracket floor is the deterministic cost of esp_wifi_get_tsf_time() -- measured at
# 42 us median, 42-49 us range across four devices -- so a threshold near that excludes
# everything. What matters is not the width but its CONSISTENCY: a fixed bracket puts a
# fixed bias on the midpoint, which cancels between identical devices, while a widened
# bracket means interference landed mid-read and that sample really is noisy.
TRUST_SW_US = 200


def parse(path):
    """-> list of (server_us, tsf_us, sandwich_us), oldest first."""
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = RAW_RE.search(line)
            if not m:
                continue
            s_ts, pushed, played, played_ts, tsf, tsf_local, sw, rate = (int(x) for x in m.groups())
            if rate <= 0:
                continue
            # Server time of the frame that had just rendered at played_ts
            in_flight = pushed - played
            server_us = s_ts - in_flight * 1000000 // rate
            # ...and that instant expressed in the shared TSF clock
            tsf_of_frame = played_ts + (tsf - tsf_local)
            out.append((server_us, tsf_of_frame, sw))
    return out


# Discontinuities, not noise, are the main contaminant: every starvation re-baselines
# the frame accounting, which steps `pushed - played` and shifts that sample's computed
# server time. A plain least-squares fit lets a handful of those steps tilt the whole
# line -- observed as 440-1220 us residuals on a relation that should be near-perfectly
# linear, worst on the device taking the most starvations. So reject them iteratively
# rather than averaging them in.
OUTLIER_SIGMA = 2.5
OUTLIER_PASSES = 5
MIN_FIT_SAMPLES = 6


def _lsq(samples):
    n = len(samples)
    if n < 2:
        return None
    mx = sum(s for s, _, _ in samples) / n
    my = sum(t for _, t, _ in samples) / n
    sxx = sum((s - mx) ** 2 for s, _, _ in samples)
    sxy = sum((s - mx) * (t - my) for s, t, _ in samples)
    if sxx == 0:
        return None
    b = sxy / sxx
    a = my - b * mx
    resid = [t - (a + b * s) for s, t, _ in samples]
    return a, b, (statistics.pstdev(resid) if len(resid) > 1 else 0.0), resid


def fit(samples):
    """Robust least-squares tsf = a + b*server.

    -> (intercept, slope, residual sigma, n_used, n_dropped) or None.
    b - 1 is the device's clock rate against the server. Sigma is reported AFTER
    rejection, so it describes the steady-state relation rather than the disturbances.
    """
    kept = list(samples)
    dropped = 0
    for _ in range(OUTLIER_PASSES):
        r = _lsq(kept)
        if r is None:
            return None
        a, b, sd, resid = r
        if sd == 0.0:
            break
        keep = [smp for smp, e in zip(kept, resid) if abs(e) <= OUTLIER_SIGMA * sd]
        if len(keep) == len(kept) or len(keep) < MIN_FIT_SAMPLES:
            break
        dropped += len(kept) - len(keep)
        kept = keep
    r = _lsq(kept)
    if r is None:
        return None
    a, b, sd, _ = r
    return a, b, sd, len(kept), dropped


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="two or more logs captured on ONE machine")
    args = ap.parse_args()
    if len(args.logs) < 2:
        ap.error("need at least two logs")

    names = [os.path.splitext(os.path.basename(p))[0] for p in args.logs]
    data = {}
    for name, path in zip(names, args.logs):
        all_s = parse(path)
        good = [s for s in all_s if s[2] <= TRUST_SW_US]
        data[name] = good
        wide = len(all_s) - len(good)
        sw = [s[2] for s in all_s]
        print(f"{name:>10}: {len(all_s):4} raw samples, {wide} wide (sw>{TRUST_SW_US}us)"
              + (f", sandwich median={statistics.median(sw):.0f}us max={max(sw)}us" if sw else ""))

    usable = {k: v for k, v in data.items() if len(v) >= 4}
    if len(usable) < 2:
        print("\nnot enough trusted samples yet -- let it run longer", file=sys.stderr)
        return 1

    print("\nper-device fit of shared-clock time against server audio time:")
    fits = {}
    for name, s in usable.items():
        f = fit(s)
        if f is None:
            continue
        a, b, sd, used, dropped = f
        fits[name] = (a, b, sd, used)
        # b-1 is the device's clock rate versus the server's, in ppm
        print(f"  {name:>10}  rate offset {(b - 1) * 1e6:+8.1f} ppm   residual {sd:7.1f} us   "
              f"n={used} (dropped {dropped} as steps)")

    if len(fits) < 2:
        return 1

    # Compare at a server time all devices actually cover, so no extrapolation
    lo = max(min(s for s, _, _ in v) for v in usable.values())
    hi = min(max(s for s, _, _ in v) for v in usable.values())
    if hi <= lo:
        print("\nno overlapping server-time range across these logs", file=sys.stderr)
        return 1
    mid = (lo + hi) / 2

    print(f"\ntrue relative offset at the midpoint of the shared window ({(hi - lo) / 1e6:.0f} s of overlap):")
    print("  positive = the first device renders that audio LATER\n")
    for x, y in itertools.combinations(fits, 2):
        ax, bx, sdx, nx = fits[x]
        ay, by, sdy, ny = fits[y]
        delta = (ax + bx * mid) - (ay + by * mid)
        # Fit residuals are independent, so combine in quadrature over the used counts
        conf = ((sdx ** 2 / nx) + (sdy ** 2 / ny)) ** 0.5
        verdict = "" if abs(delta) > 2 * conf else "   (not significant)"
        print(f"  {x:>10} - {y:<10} {delta:+9.1f} us   +-{conf:5.1f} us{verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
