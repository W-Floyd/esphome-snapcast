#!/usr/bin/env python3
"""Structure function of the inter-device skew: sd of skew differences at lag tau.

JUDGE CHANGES ON THIS, NOT ON sd. Plain sd over a window conflates two different things and its
value depends on the window length, which made it useless for comparing changes: the same build
measured sd 3.15 over 17 s and 8.06 over 4 minutes. The structure function separates them --

    white noise      flat at sqrt(2)*sigma from the shortest lag
    random walk      grows without bound as sqrt(tau)
    bounded wander   grows, then PLATEAUS at the correlation time

BASE_NOW is build 88 (R5.2/R10.1): rival-gated (max-rival 0.5, matching wire-window.py), timestamp-
based lag matching (not index-stride -- rival gating drops rows unevenly, so a fixed index stride no
longer approximates a fixed time lag). Every quoted baseline carries its capture config (rows/s):
see WS0 in PLAN-sub-microsecond.md -- the row rate silently sets every n-dependent number here.

    BASE_PREKP   pre-TRIM_KP_RUN, KP 0.25 / 1 Hz beacons, no rival gate applied (n=60506, quiet.csv)
    BASE_NOW     build 88, rival-gated, RE-TAKEN 2026-08-31 00:2x on a 360 s hole-free window
                 (n=10012, 27.8 rows/s, test.csv 17:00-23:00 min-offset). Agrees with R5.2's two
                 900 s/3.3-rows/s windows to ~15% out to 10 s (1.36 vs 1.33-1.50 at 1s, 6.91 vs
                 5.63-5.83 at 10s); the 30s point (12.4) sits between R5.2's two windows (9.5/10.5)
                 despite the different capture rate, consistent with R5.2's finding that the
                 plateau/corner is a property of the loop, not the row rate. Row rate MUST be
                 quoted beside any ratio computed against this baseline (R10.1).

USAGE
    python3 scripts/bench/structure-function.py [--csv PATH] [--last SECONDS] [--base prekp|now]
                                                  [--max-rival 0.5] [--min-coef 0.99] [--tol 0.1]

EXCLUDE EVENTS FIRST. A window containing a resync or a disconnect inflates every lag: a 300 s
slice that happened to span two disconnects read 2.09x baseline where the clean 230 s inside it read
0.72x. Check a.log/b.log for `Hard resync`, `PLAYER STALLED` and `Disconnect` over the same span
before believing any number here.
"""
import argparse
import bisect
import csv
import statistics as st
import sys

# WHY pcm_coef IS GATED ALONGSIDE rival (measured 2026-08-31, PLAN-sub-microsecond).
# rival alone is NOT sufficient. During the ms-class reference steps the analyser's correlator
# locks 35-36 WHOLE FRAMES away and offset_ns reports it; rival stays at 0.03-0.07 on many such
# rows -- it does not catch this -- while pcm_coef falls to 0.46-0.95 against 0.999-1.000 when the
# lock is good. Ungated, a real 49-197 us differential reads as 813-3295 us, and a 9-33x
# "amplification mechanism" was briefly built on those rows before being retracted. Only 3.36 % of
# rows fall below 0.99, so the gate is nearly free. The tell was arithmetic: the excursions were
# exactly 794 and 816 us, i.e. 35 and 36 x 22.68 us.
MIN_COEF = 0.99

# tau seconds -> sd(diff) in us
BASE_PREKP = {0.1: 0.300, 0.5: 1.008, 1: 1.873, 2: 3.245, 5: 6.175, 10: 8.346, 30: 9.004, 60: 8.904}
BASE_NOW = {0.1: 0.193, 0.5: 0.766, 1: 1.364, 2: 2.249, 5: 4.105, 10: 6.908, 30: 12.437}


def load(path, last, max_rival, min_coef=MIN_COEF):
    """Rival-gated load, keyed on unix_s (real timestamps, not row index)."""
    with open(path) as fh:
        rows = [r for r in fh if not r.startswith("#")]
    reader = csv.DictReader(rows)
    cols = reader.fieldnames or []
    skew_col = next((c for c in cols if c.startswith("offset") or "skew" in c), None)
    rival_col = next((c for c in cols if "rival" in c), None)
    if skew_col is None or "unix_s" not in cols:
        sys.exit(f"{path}: could not find a skew column / unix_s in {cols}")
    unit = "ns" if skew_col.endswith("_ns") else ("us" if skew_col.endswith("_us") else "")
    scale = 1e-3 if unit == "ns" else 1.0

    t, s, dropped, n_all, dropped_coef = [], [], 0, 0, 0
    for r in reader:
        try:
            ts = float(r["unix_s"])
        except (ValueError, KeyError):
            continue
        n_all += 1
        if rival_col and r.get(rival_col) not in (None, ""):
            try:
                if float(r[rival_col]) > max_rival:
                    dropped += 1
                    continue
            except ValueError:
                pass
        try:
            if r.get("pcm_coef") and float(r["pcm_coef"]) < min_coef:
                dropped_coef += 1
                continue
        except ValueError:
            pass
        try:
            x = float(r[skew_col]) * scale
        except (ValueError, KeyError):
            continue
        if x != x:  # NaN: PCM lock lost, usually at an event
            dropped += 1
            continue
        t.append(ts)
        s.append(x)
    if not t:
        sys.exit(f"{path}: no usable rows")
    if last:
        hi = t[-1]
        keep = [i for i, x in enumerate(t) if x >= hi - last]
        t, s = [t[i] for i in keep], [s[i] for i in keep]
    return t, s, dropped, n_all, dropped_coef


def diffs_at_lag(t, s, tau, tol):
    """Pair samples ~tau apart by real timestamp (not index stride), within tol*tau."""
    n = len(t)
    if n < 3:
        return []
    span = t[-1] - t[0]
    avg_dt = span / n if n else 0
    step = max(1, int(tau / avg_dt / 4)) if avg_dt > 0 else 1
    diffs = []
    for i in range(0, n, step):
        target = t[i] + tau
        j = bisect.bisect_left(t, target)
        best, best_err = None, None
        for k in (j - 1, j):
            if 0 <= k < n and k != i:
                err = abs((t[k] - t[i]) - tau)
                if err <= tol * tau and (best_err is None or err < best_err):
                    best, best_err = k, err
        if best is not None:
            diffs.append(s[best] - s[i])
    return diffs


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--csv", default="test.csv")
    p.add_argument("--last", type=float, default=0, help="use only the last N seconds")
    p.add_argument("--base", choices=("prekp", "now"), default="now")
    p.add_argument("--max-rival", type=float, default=0.5, help="drop rows above this (matches wire-window.py)")
    p.add_argument("--min-coef", type=float, default=MIN_COEF,
                   help="drop rows whose pcm_coef is below this -- catches whole-frame mislocks rival misses")
    p.add_argument("--tol", type=float, default=0.1, help="lag-match tolerance as a fraction of tau")
    a = p.parse_args()

    t, s, dropped, n_all, dcoef = load(a.csv, a.last, a.max_rival, a.min_coef)
    if len(s) < 500:
        sys.exit(f"only {len(s)} rival-clean samples -- need ~500+ for the 30 s lag to mean anything")
    span = t[-1] - t[0]
    rate = len(s) / span if span > 0 else 0
    med = st.median(s)
    print(f"n={len(s)} (rival-gated {dropped}/{n_all} max-rival {a.max_rival}; "
          f"coef-gated {dcoef} below {a.min_coef})  "
          f"span {span:.0f} s  rate {rate:.2f} rows/s")
    print(f"  skew median {med:+.2f} us   sd {st.stdev(s):.3f}   "
          f"MAD {st.median([abs(x-med) for x in s]):.3f}")
    base = BASE_PREKP if a.base == "prekp" else BASE_NOW
    print(f"\n  {'tau':>7} {'sd(diff)':>9} {'base':>7}  ratio   (baseline: {a.base}, tol ±{a.tol*100:.0f}%)")
    for tau, b in base.items():
        if tau >= span / 3:
            print(f"  {tau:6.1f}s   -- window too short for this lag --")
            break
        d = diffs_at_lag(t, s, tau, a.tol)
        if len(d) < 8:
            print(f"  {tau:6.1f}s   -- too few timestamp-matched pairs (n={len(d)}) --")
            continue
        v = st.stdev(d)
        print(f"  {tau:6.1f}s {v:9.3f} {b:7.3f}  {v/b:5.2f}x  (n={len(d)})")


if __name__ == "__main__":
    main()
