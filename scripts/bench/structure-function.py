#!/usr/bin/env python3
"""Structure function of the inter-device skew: sd of skew differences at lag tau.

JUDGE CHANGES ON THIS, NOT ON sd. Plain sd over a window conflates two different things and its
value depends on the window length, which made it useless for comparing changes: the same build
measured sd 3.15 over 17 s and 8.06 over 4 minutes. The structure function separates them --

    white noise      flat at sqrt(2)*sigma from the shortest lag
    random walk      grows without bound as sqrt(tau)
    bounded wander   grows, then PLATEAUS at the correlation time

-- and the residual jitter here is the third kind. Measured 2026-08-28: 0.30 us at tau 0.1 s rising
to a plateau of 9.0 us for tau >= 30 s, with a corner at 10-30 s that coincides with the trim loop's
~24 s limit cycle and its 0.79 loop gain. So the PLATEAU is the number that moves when the loop
changes, and the short lags say whether high-frequency tracking was harmed.

Baselines built in below. Both are pre-`TRIM_KP_RUN` experiments at KP 0.25 / 1 Hz beacons:

    BASE_PREKP   the original 1.5 h event-free window (n=60506, saved as quiet.csv)
    BASE_NOW     the same config after the session's other fixes -- deterministic consensus, the
                 DMA-span fixes, reanchor off, the depth gate -- measured on a clean 230 s stretch

Compare against BASE_NOW for anything after 2026-08-28 11:24; BASE_PREKP is kept because it is what
the KP prediction was originally sized against.

USAGE
    python3 scripts/bench/structure-function.py [--last SECONDS] [--csv PATH] [--base prekp|now]

EXCLUDE EVENTS FIRST. A window containing a resync or a disconnect inflates every lag: a 300 s
slice that happened to span two disconnects read 2.09x baseline where the clean 230 s inside it read
0.72x. Check a.log/b.log for `Hard resync`, `PLAYER STALLED` and `Disconnect` over the same span
before believing any number here.
"""
import argparse
import statistics as st
import sys

# tau seconds -> sd(diff) in us
BASE_PREKP = {0.1: 0.300, 0.5: 1.008, 1: 1.873, 2: 3.245, 5: 6.175, 10: 8.346, 30: 9.004, 60: 8.904}
BASE_NOW = {0.1: 0.148, 0.5: 0.757, 1: 1.439, 2: 2.678, 5: 5.060, 10: 6.665, 30: 6.476}


def load(path, last):
    t, s = [], []
    for line in open(path):
        if line.startswith(("#", "elapsed")):
            continue
        f = line.split(",")
        if len(f) < 8:
            continue
        try:
            el, off = float(f[0]), float(f[2]) / 1000.0
        except ValueError:
            continue
        if off != off:          # nan: the analyser lost PCM lock, usually at an event
            continue
        t.append(el)
        s.append(off)
    if not t:
        sys.exit(f"{path}: no usable rows")
    if last:
        hi = t[-1]
        keep = [i for i, x in enumerate(t) if x >= hi - last]
        t, s = [t[i] for i in keep], [s[i] for i in keep]
    return t, s


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--csv", default="test.csv")
    p.add_argument("--last", type=float, default=0, help="use only the last N seconds")
    p.add_argument("--base", choices=("prekp", "now"), default="now")
    a = p.parse_args()

    t, s = load(a.csv, a.last)
    if len(s) < 500:
        sys.exit(f"only {len(s)} clean samples -- need ~500+ for the 30 s lag to mean anything")
    dt = (t[-1] - t[0]) / len(t)
    med = st.median(s)
    print(f"n={len(s)}  span {t[-1]-t[0]:.0f} s  spacing {dt:.4f} s")
    print(f"  skew median {med:+.2f} us   sd {st.stdev(s):.3f}   "
          f"MAD {st.median([abs(x-med) for x in s]):.3f}")
    base = BASE_PREKP if a.base == "prekp" else BASE_NOW
    print(f"\n  {'tau':>7} {'sd(diff)':>9} {'base':>7}  ratio   (baseline: {a.base})")
    for tau, b in base.items():
        lag = max(1, int(tau / dt))
        if lag >= len(s) // 3:
            print(f"  {tau:6.1f}s   -- window too short for this lag --")
            break
        d = [s[i + lag] - s[i] for i in range(0, len(s) - lag, max(1, lag // 4))]
        if len(d) < 8:
            break
        v = st.stdev(d)
        print(f"  {tau:6.1f}s {v:9.3f} {b:7.3f}  {v/b:5.2f}x  (n={len(d)})")


if __name__ == "__main__":
    main()
