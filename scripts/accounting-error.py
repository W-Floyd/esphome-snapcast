#!/usr/bin/env python3
"""Measure the accounting error directly: what the boards believe, minus what they do.

The devices exchange a render phase over the shared TSF, and it is derived from
(pushed - played) -- so, as the code says at that site, "it inherits any accounting error...
it cannot say WHY two devices disagree, only that they do and by how much." That is why the
two boards' render phases agree to 4 us while their speakers sit hundreds of us apart: the
correction path is structurally blind to this class of fault.

The defect is therefore exactly:

    accounting_error = measured_skew - (render_phase_B - render_phase_A)

measured_skew comes from the logic analyser (ground truth, sub-us on an MLS stimulus); the
render phases come from the boards. Everything chased today has been a proxy for this. Emitting
it as a series makes it regressable against the accounting terms the firmware publishes.

    python3 scripts/accounting-error.py a.log b.log test.csv --out acct-err.csv
"""

import argparse
import bisect
import datetime
import math
import re
import statistics

RP = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*Render phase mine (-?\d+) leader (-?\d+) delta (-?\d+) us")


def load_phase(path, tail):
    """(sod, mine) -- file-position anchored, never matched on timestamp: these logs span days
    with no date in the line, which has silently returned a previous day's build before."""
    out = []
    for l in open(path, errors="replace").read().splitlines()[-tail:]:
        m = RP.match(l)
        if not m:
            continue
        t = (int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
             + int(m.group(4)) / 1000.0)
        out.append((t, int(m.group(5))))
    return out


def at(series, ts, t, tol):
    i = bisect.bisect_left(ts, t)
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(series) and abs(series[j][0] - t) <= tol:
            if best is None or abs(series[j][0] - t) < abs(series[best][0] - t):
                best = j
    return series[best][1] if best is not None else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a_log")
    ap.add_argument("b_log")
    ap.add_argument("csv")
    ap.add_argument("--tail", type=int, default=600000)
    ap.add_argument("--tol", type=float, default=2.0, help="max s between a skew row and a phase sample")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    A, B = load_phase(args.a_log, args.tail), load_phase(args.b_log, args.tail)
    print(f"render-phase samples: A={len(A)}  B={len(B)}")
    if not A or not B:
        raise SystemExit("no 'Render phase mine' lines found")
    ta, tb = [x[0] for x in A], [x[0] for x in B]

    rows = []
    for l in open(args.csv):
        f = l.split(",")
        if len(f) < 8 or l.startswith(("#", "elapsed")):
            continue
        try:
            unix, off = float(f[1]), float(f[2])
        except ValueError:
            continue
        if not math.isfinite(off):
            continue
        lt = datetime.datetime.fromtimestamp(unix)
        sod = lt.hour * 3600 + lt.minute * 60 + lt.second + lt.microsecond / 1e6
        pa, pb = at(A, ta, sod, args.tol), at(B, tb, sod, args.tol)
        if pa is None or pb is None:
            continue
        skew = off / 1000.0
        believed = float(pb - pa)          # what the devices think their offset is
        rows.append((sod, skew, believed, skew - believed))

    print(f"paired rows: {len(rows)}")
    if len(rows) < 50:
        raise SystemExit("not enough paired rows")

    skew = [r[1] for r in rows]
    bel = [r[2] for r in rows]
    err = [r[3] for r in rows]
    print(f"\n  measured skew (truth)      median {statistics.median(skew):+9.1f} us  sd {statistics.pstdev(skew):8.1f}")
    print(f"  believed (render phase B-A) median {statistics.median(bel):+9.1f} us  sd {statistics.pstdev(bel):8.1f}")
    print(f"  ACCOUNTING ERROR            median {statistics.median(err):+9.1f} us  sd {statistics.pstdev(err):8.1f}")

    # If the belief carried any information about the truth, this would be non-zero.
    n = len(skew)
    mx, my = sum(bel) / n, sum(skew) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(bel, skew))
    sxx = sum((x - mx) ** 2 for x in bel)
    syy = sum((y - my) ** 2 for y in skew)
    if sxx > 0 and syy > 0:
        print(f"\n  believed vs truth: r = {sxy/math.sqrt(sxx*syy):+.3f}  slope = {sxy/sxx:+.3f}")
        print("  (r -> 1 would mean the devices can already see their own offset)")

    if args.out:
        with open(args.out, "w") as f:
            f.write("sod,skew_us,believed_us,accounting_error_us\n")
            for r in rows:
                f.write(f"{r[0]:.3f},{r[1]:.3f},{r[2]:.3f},{r[3]:.3f}\n")
        print(f"\nwrote {args.out} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
