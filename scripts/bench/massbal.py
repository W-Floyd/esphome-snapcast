#!/usr/bin/env python3
"""Grade the ring's mass balance from MASSBAL lines.

Nothing in the servo enforces audio-in == audio-out: the loop closes on the deadline error and
buffer occupancy is never a setpoint. Chasing a drifting deadline IS balancing the buffer, but
only while the deadline moves because the clocks differ -- a bias change, a mapping re-anchor or
a resync repair moves it for other reasons, and the loop then commands a rate with nothing to
balance while the ring drifts at exactly that rate.

    MASSBAL dt=<ms> in=<us> out=<us> disc=<us> d=<us> ring=<us> rate=<ppm>

THE IDENTITY COMES FIRST. d = in - out - disc must equal the change in ring depth over the same
window. If it does not, there is a path into or out of the ring that is not counted, and every
number below it is void. This script reports that before it reports anything else, because a
balance computed from an incomplete accounting is not a weak measurement -- it is not one at all.

Usage:
    scripts/bench/massbal.py a.log [b.log ...]
    ssh 192.168.1.230 'tail -c 40M a.log' | scripts/bench/massbal.py -
"""

import re
import sys

# No trailing field is required beyond the last one we read, and every field is matched by NAME.
# A regex that depends on field order or on a trailing token drops whole lines when the format
# grows -- which on this bench turned a formatting limit into silent data loss.
LINE = re.compile(
    r"MASSBAL\s+dt=(-?\d+)\s+in=(-?\d+)\s+out=(-?\d+)\s+disc=(-?\d+)\s+"
    r"d=(-?\d+)\s+ring=(-?\d+)\s+rate=([-+]?[\d.]+)"
)


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return float("nan")
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def parse(paths):
    rows = []
    for p in paths:
        f = sys.stdin if p == "-" else open(p, errors="replace")
        for line in f:
            m = LINE.search(line)
            if m:
                dt, i, o, d, dd, ring, rate = m.groups()
                rows.append(
                    {
                        "dt_ms": int(dt),
                        "in": int(i),
                        "out": int(o),
                        "disc": int(d),
                        "d": int(dd),
                        "ring": int(ring),
                        "rate": float(rate),
                        "src": p,
                    }
                )
        if f is not sys.stdin:
            f.close()
    return rows


def grade(rows, label):
    print(f"=== {label}: {len(rows)} windows ===")
    if len(rows) < 3:
        print("  too few windows to say anything")
        return

    # 1. THE ACCOUNTING IDENTITY, before any interpretation.
    mismatch = []
    for a, b in zip(rows, rows[1:]):
        if b["src"] != a["src"]:
            continue
        mismatch.append(b["d"] - (b["ring"] - a["ring"]))
    if mismatch:
        bad = sum(1 for m in mismatch if abs(m) > 2000)  # 2 ms of slack for sampling skew
        print(f"  identity  d vs dring: median {median(mismatch):+.0f} us, "
              f"{bad}/{len(mismatch)} windows off by >2 ms")
        if bad > len(mismatch) // 10:
            print("  *** IDENTITY FAILS: bytes are entering or leaving the ring uncounted.")
            print("  *** Everything below is void until that path is found.")

    # 2. The balance itself. Expressed as ppm, which is the unit the servo commands in, so the
    #    imbalance and the thing that would have to cause it are directly comparable.
    tot_dt = sum(r["dt_ms"] for r in rows) / 1000.0
    tot_d = sum(r["d"] for r in rows)
    tot_disc = sum(r["disc"] for r in rows)
    if tot_dt > 0:
        print(f"  balance   {tot_d:+d} us over {tot_dt:.0f} s  ->  {tot_d / tot_dt:+.1f} ppm net")
        # As a share of GROSS OUTFLOW, which is the only denominator that is always positive and
        # always meaningful. A share of the net balance is not: the net crosses zero, so near
        # balance the ratio explodes and prints a number that looks like a measurement.
        tot_out = sum(r["out"] for r in rows)
        gross = tot_out + tot_disc
        if tot_disc:
            print(f"  discards  {tot_disc} us total, {tot_disc / tot_dt:+.1f} ppm equivalent "
                  f"({100.0 * tot_disc / gross:.2f}% of everything read out)")
        else:
            print("  discards  none")

    # 3. Ring depth trend: the integral, regressed on time. Slope zero means in == out on average.
    t, y = 0.0, []
    xs = []
    for r in rows:
        xs.append(t)
        y.append(float(r["ring"]))
        t += r["dt_ms"] / 1000.0
    n = len(xs)
    mx = sum(xs) / n
    my = sum(y) / n
    den = sum((x - mx) ** 2 for x in xs)
    slope = sum((x - mx) * (v - my) for x, v in zip(xs, y)) / den if den else float("nan")
    # us/s IS ppm, so the trend and the balance are directly comparable: they are two independent
    # measurements of the same imbalance, one from the counters and one from the integral. They
    # should agree, and disagreeing is itself the finding.
    print(f"  ring      median {median([r['ring'] for r in rows]):.0f} us, "
          f"trend {slope:+.1f} us/s (= ppm)")

    # 4. Attribution, which is the whole reason discards are counted apart from playout.
    deficit = [r for r in rows if r["d"] < -1000]
    if deficit:
        supply = sum(1 for r in deficit if r["disc"] < 1000)
        print(f"  deficits  {len(deficit)} windows lost >1 ms: "
              f"{supply} with no discards (SUPPLY shortfall), "
              f"{len(deficit) - supply} with discards (CORRECTION drained it)")
    else:
        print("  deficits  none beyond 1 ms")

    print(f"  rate      median {median([r['rate'] for r in rows]):+.2f} ppm")


def main():
    paths = sys.argv[1:]
    if not paths:
        print(__doc__)
        return 1
    for p in paths:
        rows = parse([p])
        grade(rows, p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
