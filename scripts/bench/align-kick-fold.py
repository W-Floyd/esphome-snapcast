#!/usr/bin/env python3
"""Epoch-fold the wire on ACTUAL RALIGN event times: does an align kick move the wire, and how much?

WHY NOT AUTOCORRELATION. The first attempt at the "10 s stairstep" used the autocorrelation of the
1 s wire series at lag 10 s. On a series whose autocorrelation is already +0.87 at 10 s from ordinary
wander, a staircase -- a ramp plus a step -- is smeared into that decay and does not appear as a
peak. The measurement returned a smooth monotone decay and would have been quoted as "no stairstep".
Folding on the known event times instead puts the signal in phase and averages the wander away, and
it is ~sqrt(N_events) more sensitive.

SIGN, stated so an inversion cannot pass (R13.3's rule):
  offset_ns is B - A, positive = B later.
  A's bias UP  => A plays later => B-A falls => offset DOWN.
  B's bias UP  => B plays later => offset UP.
So folds on A and B events must come out OPPOSITE in sign. Same sign on both is the alarm -- it
means the fold is picking up something common to both boards (a shared disturbance, or the align
cadence coinciding with another 10 s process), not the kick.

Each epoch is detrended on its OWN pre-event baseline, so the slow wander contributes a random
offset that averages out rather than a bias. The null is the same fold on times shifted by half a
period: if the "response" survives that, it is not locked to the events.

USAGE
    align-kick-fold.py [--from-min N --to-min M] [--pre 3] [--post 8] [--min-step 1]
"""
import argparse, csv, re, statistics as st, subprocess, sys, bisect, datetime as dt

# WHY pcm_coef IS GATED ALONGSIDE rival (measured 2026-08-31, PLAN-sub-microsecond).
# rival alone is NOT sufficient. During the ms-class reference steps the analyser's correlator
# locks 35-36 WHOLE FRAMES away and offset_ns reports it; rival stays at 0.03-0.07 on many such
# rows -- it does not catch this -- while pcm_coef falls to 0.46-0.95 against 0.999-1.000 when the
# lock is good. Ungated, a real 49-197 us differential reads as 813-3295 us, and a 9-33x
# "amplification mechanism" was briefly built on those rows before being retracted. Only 3.36 % of
# rows fall below 0.99, so the gate is nearly free. The tell was arithmetic: the excursions were
# exactly 794 and 816 us, i.e. 35 and 36 x 22.68 us.
MIN_COEF = 0.99

RALIGN = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?RALIGN group ([+-]?\d+) -> bias ([+-]?\d+) us")


def events(log, t_lo, t_hi):
    """(unix_ts, bias, d_bias) for RALIGN lines in range. Byte-anchored tail, per CLAUDE.md."""
    out = []
    day = dt.datetime.fromtimestamp(t_lo).replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
    raw = subprocess.run(["tail", "-c", "120000000", log], capture_output=True).stdout
    prev = None
    for line in raw.split(b"\n"):
        m = RALIGN.match(line.decode("utf-8", "replace"))
        if not m:
            continue
        h, mi, s, frac, _grp, bias = m.groups()
        ts = day + int(h) * 3600 + int(mi) * 60 + int(s) + int(frac) / 10 ** len(frac)
        bias = int(bias)
        if prev is not None and t_lo <= ts <= t_hi:
            out.append((ts, bias, bias - prev))
        prev = bias
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--csv", default="test.csv")
    p.add_argument("--from-min", type=float); p.add_argument("--to-min", type=float)
    p.add_argument("--pre", type=float, default=3.0)
    p.add_argument("--post", type=float, default=8.0)
    p.add_argument("--min-step", type=float, default=1.0, help="ignore events whose bias moved less than this (us)")
    a = p.parse_args()

    T, X, t0 = [], [], None
    with open(a.csv) as fh:
        it = (r for r in fh if not r.startswith("#"))
        for r in csv.DictReader(it):
            try:
                t = float(r["unix_s"])
                if float(r["rival"]) > 0.5:
                    continue
                if r.get("pcm_coef") and float(r["pcm_coef"]) < MIN_COEF:
                    continue
                x = float(r["offset_ns"]) / 1000.0
            except (ValueError, KeyError, TypeError):
                continue
            if x != x:
                continue
            if t0 is None:
                t0 = t
            rel = (t - t0) / 60.0
            if a.from_min is not None and rel < a.from_min: continue
            if a.to_min is not None and rel > a.to_min: continue
            T.append(t); X.append(x)
    if len(T) < 500:
        sys.exit(f"only {len(T)} rows")
    print(f"wire n={len(T)}  span {(T[-1]-T[0])/60:.1f} min  {len(T)/(T[-1]-T[0]):.1f} rows/s")

    def fold(evs, shift=0.0):
        acc = {}
        used = 0
        for ts, _bias, d in evs:
            if abs(d) < a.min_step:
                continue
            ts += shift
            i0 = bisect.bisect_left(T, ts - a.pre); i1 = bisect.bisect_right(T, ts + a.post)
            if i1 - i0 < 20:
                continue
            base = [X[i] for i in range(i0, bisect.bisect_left(T, ts))]
            if len(base) < 5:
                continue
            b = st.mean(base)
            sgn = 1.0 if d > 0 else -1.0          # normalise by the step's DIRECTION
            for i in range(i0, i1):
                acc.setdefault(round(T[i] - ts, 1), []).append((X[i] - b) * sgn)
            used += 1
        return acc, used

    for name, log in (("A e985e8", "a.log"), ("B f04d74", "b.log")):
        evs = events(log, T[0] - 60, T[-1] + 60)
        acc, used = fold(evs)
        nacc, _ = fold(evs, shift=5.0)             # null: half a period off
        if used < 5:
            print(f"{name}: only {used} usable events (of {len(evs)})"); continue
        def at(d, lo, hi):
            v = [x for k, vs in d.items() if lo <= k <= hi for x in vs]
            return st.mean(v) if len(v) > 10 else float("nan")
        print(f"\n{name}: {used} events (|d bias| >= {a.min_step} us), sign-normalised to bias-UP")
        print(f"   pre  (-3..0 s)  {at(acc,-3,0):+7.3f} us   [null {at(nacc,-3,0):+7.3f}]")
        for lo, hi in ((0, 2), (2, 4), (4, 6), (6, 8)):
            print(f"   +{lo}..{hi} s      {at(acc,lo,hi):+7.3f} us   [null {at(nacc,lo,hi):+7.3f}]")
    print("\n  A bias UP should move the wire DOWN; B bias UP should move it UP.")
    print("  Same sign on both boards = the fold is catching something shared, not the kick.")


if __name__ == "__main__":
    main()
