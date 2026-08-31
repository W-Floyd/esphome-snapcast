#!/usr/bin/env python3
"""SF_d -- WS3.4's discriminating test: does the LOOP generate the differential rate wander, or does
it enter downstream of the command?

    d = fs_diff - trim_diff        the implied disturbance: what the differential rate would be at
                                   zero trim, i.e. the plant's own rate error

    d slow while trim_diff is broadband  =>  the loop is GENERATING the wander  => tau_s/ti_s sweep
    d broadband                          =>  the wander enters DOWNSTREAM of the command
                                             (rate-lock delivery / I2S driver / fs estimator)
                                             => a tau_s sweep reads null, five membership changes wasted

WHY SPECTRAL AND NOT CORRELATIONAL (R9.1, and the reviewer's own retraction of R6.3): in closed loop
fs = d + trim with trim ~ -G*d, so corr(fs, trim) -> -1 WHATEVER the actuator does. A command
anticorrelated with its own plant output is the feedback identity, not evidence of fidelity. The
correlation experiment was withdrawn; this is its replacement.

ESTIMATORS (R10.5). The achieved differential rate is taken from the WIRE SLOPE, not from fs_b-fs_a:
per-capture offset noise ~32 ns gives a 30 s slope ~1e-4 ppm, about 400x better than the frequency
columns (per-capture fs noise 0.9-1.1 ppm/board). `fs_*` is kept as the independent cross-check.

SIGN IDENTITY (R13.3), stated because an unstated sign is how a real inversion gets waved through:
`offset_ns` is B-A and positive means B later, so B running FASTER makes the offset FALL --

    wire_slope_ppm  ==  -fs_diff        by construction

so the cross-check PASSES when corr(slope, fs_diff) is near -1. A POSITIVE correlation is the alarm.
The plant itself is fs ~ crystal + trim (set_trim_ppm: positive = play faster); the minus sign in
corr(fs_diff, trim_diff) is the loop's, not the plant's.

USAGE
    python3 scripts/bench/sf-d.py [--csv test.csv] [--bin 2.5] [--max-rival 0.5]
                                  [--max-p2p 60] [--from-min N --to-min M]

The --max-p2p gate is NOT cosmetic and MUST be quoted with any result (R13.1/R13.2): the same
quantity reads 0.46 / 0.79 / 1.47 ppm at p2p gates of 30 / 60 / 200 us -- a 3x spread from the
choice of "quiet". The plan's central sizing fact is conditioned on p2p <= 60 us at tau = 30 s.
"""
import argparse
import bisect
import csv
import statistics as st
import sys


def load(path, max_rival, from_min, to_min):
    with open(path) as fh:
        rows = [r for r in fh if not r.startswith("#")]
    out, t0 = [], None
    for r in csv.DictReader(rows):
        try:
            t = float(r["unix_s"])
            off = float(r["offset_ns"]) / 1000.0
        except (ValueError, KeyError, TypeError):
            continue
        if off != off:
            continue
        if t0 is None:
            t0 = t
        rel = (t - t0) / 60.0
        if from_min is not None and rel < from_min:
            continue
        if to_min is not None and rel > to_min:
            continue
        try:
            if r.get("rival") and float(r["rival"]) > max_rival:
                continue
        except ValueError:
            pass

        def num(k):
            v = r.get(k)
            if v in (None, ""):
                return None
            try:
                return float(v)
            except ValueError:
                return None

        fa, fb = num("fs_a_hz"), num("fs_b_hz")
        ta, tb = num("trim_a_ppm"), num("trim_b_ppm")
        fs_diff = None
        if fa and fb and fa > 0:
            fs_diff = (fb - fa) / fa * 1e6
        trim_diff = (tb - ta) if (ta is not None and tb is not None) else None
        out.append((t, off, fs_diff, trim_diff))
    if not out:
        sys.exit(f"{path}: no usable rows")
    return out


def slope_ppm(ts, xs):
    """Least-squares slope of offset(us) vs t(s) == ppm. Returns None if degenerate."""
    n = len(ts)
    if n < 3:
        return None
    mt = sum(ts) / n
    mx = sum(xs) / n
    den = sum((t - mt) ** 2 for t in ts)
    if den <= 0:
        return None
    return sum((t - mt) * (x - mx) for t, x in zip(ts, xs)) / den


def binned(rows, width, max_p2p):
    """Per-bin achieved rate (wire slope), fs_diff and trim_diff, gated on in-bin p2p."""
    t0 = rows[0][0]
    buckets = {}
    for t, off, fsd, trd in rows:
        buckets.setdefault(int((t - t0) // width), []).append((t, off, fsd, trd))
    series = []
    for k in sorted(buckets):
        b = buckets[k]
        if len(b) < 8:
            continue
        offs = [x[1] for x in b]
        if max_p2p and (max(offs) - min(offs)) > max_p2p:
            continue
        sl = slope_ppm([x[0] for x in b], offs)
        if sl is None:
            continue
        fsd = [x[2] for x in b if x[2] is not None]
        trd = [x[3] for x in b if x[3] is not None]
        series.append({
            "t": t0 + (k + 0.5) * width,
            "slope": sl,
            "achieved": -sl,          # R13.3: wire_slope == -fs_diff
            "fs_diff": st.mean(fsd) if fsd else None,
            "trim_diff": st.mean(trd) if trd else None,
        })
    return series


def sf(series, key, tau, tol=0.25):
    ts = [s["t"] for s in series]
    vals = [s[key] for s in series]
    d = []
    for i, t in enumerate(ts):
        j = bisect.bisect_left(ts, t + tau)
        for k in (j - 1, j):
            if 0 <= k < len(ts) and k != i and abs((ts[k] - t) - tau) <= tol * tau:
                if vals[i] is not None and vals[k] is not None:
                    d.append(vals[k] - vals[i])
                break
    return (st.stdev(d), len(d)) if len(d) > 8 else (None, len(d))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--csv", default="test.csv")
    p.add_argument("--bin", type=float, default=2.5, help="rate-estimate bin width in seconds")
    p.add_argument("--max-rival", type=float, default=0.5)
    p.add_argument("--max-p2p", type=float, default=60.0,
                   help="drop bins whose in-bin offset p2p exceeds this (us); quote it with any result")
    p.add_argument("--from-min", type=float, default=None)
    p.add_argument("--to-min", type=float, default=None)
    a = p.parse_args()

    rows = load(a.csv, a.max_rival, a.from_min, a.to_min)
    span = rows[-1][0] - rows[0][0]
    rate = len(rows) / span if span > 0 else 0
    series = binned(rows, a.bin, a.max_p2p)
    if len(series) < 20:
        sys.exit(f"only {len(series)} clean {a.bin}s bins -- need ~20+; loosen --max-p2p or widen the window")

    have = [s for s in series if s["fs_diff"] is not None and s["trim_diff"] is not None]
    for s in have:
        s["d"] = s["fs_diff"] - s["trim_diff"]
        s["d_wire"] = s["achieved"] - s["trim_diff"]

    print(f"{a.csv}: n={len(rows)} rows, {rate:.1f} rows/s, span {span:.0f} s")
    print(f"  bins {len(series)} x {a.bin}s (gate: rival<={a.max_rival}, in-bin p2p<={a.max_p2p} us), "
          f"{len(have)} with both fs and trim")

    sl = [s["achieved"] for s in have]
    fs = [s["fs_diff"] for s in have]
    tr = [s["trim_diff"] for s in have]
    if len(have) > 8:
        mfs, msl = st.mean(fs), st.mean(sl)
        num = sum((x - msl) * (y - mfs) for x, y in zip(sl, fs))
        den = (sum((x - msl) ** 2 for x in sl) * sum((y - mfs) ** 2 for y in fs)) ** 0.5
        r = num / den if den > 0 else float("nan")
        # both are B-A rate differences, so agreement is POSITIVE here; the -1 identity of R13.3
        # is between the raw wire SLOPE and fs_diff, and `achieved` has already negated the slope.
        flag = "OK" if r > 0.3 else "*** ALARM: estimators disagree in sign ***"
        print(f"  cross-check corr(wire-slope achieved, fs_diff) = {r:+.3f}   {flag}")
        print(f"  sd: achieved(wire) {st.stdev(sl):.3f}  fs_diff {st.stdev(fs):.3f}  "
              f"trim_diff {st.stdev(tr):.3f} ppm")

    print(f"\n  {'tau':>6}  {'SF(d_wire)':>11} {'SF(d_fs)':>10} {'SF(trim_diff)':>14} "
          f"{'SF(achieved)':>13}   ratio d/trim")
    for tau in (5, 10, 30, 60):
        if tau >= span / 3:
            break
        dw, _ = sf(have, "d_wire", tau)
        df, _ = sf(have, "d", tau)
        tt, n = sf(have, "trim_diff", tau)
        ac, _ = sf(have, "achieved", tau)
        if tt is None:
            print(f"  {tau:5d}s   -- too few matched pairs (n={n}) --")
            continue
        ratio = dw / tt if (dw and tt) else float("nan")
        f = lambda v: f"{v:.3f}" if v is not None else "--"
        print(f"  {tau:5d}s  {f(dw):>11} {f(df):>10} {f(tt):>14} {f(ac):>13}   {ratio:6.2f}  (n={n})")

    print("\n  READING IT: d flat across tau while trim_diff grows => loop generates the wander")
    print("              (tau_s/ti_s sweep is the lever). d growing like trim_diff => downstream")
    print("              of the command; the sweep would read null.")


if __name__ == "__main__":
    main()
