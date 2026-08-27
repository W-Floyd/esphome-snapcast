#!/usr/bin/env python3
"""Is the planted displacement just the integral of the differential rate?

TODO.md's headline says the wire offset is the integral of (fs_b - fs_a) and nothing else,
established over quiet runs. If that also holds THROUGH a resync, then the ~130 us a resync
plants is not a mystery mechanism at all -- it is the servo's own trim excursion failing to net
to zero, and the question becomes why the PI's integral does not unwind.

That is testable against data already captured: the analyser records fs_a_hz and fs_b_hz per
capture alongside the measured skew.

    d(skew)/dt = -(fs_b - fs_a)/fs      [sign: B faster -> B plays earlier -> B-A decreases]

so integrating the rate difference across the transient should reproduce the observed skew
change. If it does, the residual is fully explained by rate history. If it does not, something
moves the audio that is not a rate.

    python3 scripts/rate-integral.py test.csv --from 30 --to 120
"""

import argparse
import math
import statistics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--from", dest="t0", type=float, default=None)
    ap.add_argument("--to", dest="t1", type=float, default=None)
    args = ap.parse_args()

    rows = []
    for l in open(args.csv):
        f = l.split(",")
        if len(f) < 10 or l.startswith(("#", "elapsed")):
            continue
        try:
            el, off, fa, fb = float(f[0]), float(f[2]), float(f[8]), float(f[9])
        except ValueError:
            continue
        # RATES FROM EVERY ROW, SKEW ONLY FROM ACCEPTED ONES. fs_a/fs_b are measured from LRC
        # edge timing and do not depend on the PCM correlation, so they stay valid on rows the
        # correlation rejected. Requiring a valid skew to use the rate threw away 11% of a
        # transient window -- precisely the part where the rate is changing fastest, which is
        # the part the integral most needs.
        if fa < 1000 or fb < 1000:
            continue
        rows.append((el, off / 1000.0 if math.isfinite(off) else float("nan"), fa, fb))
    if args.t0 is not None:
        rows = [r for r in rows if r[0] >= args.t0]
    if args.t1 is not None:
        rows = [r for r in rows if r[0] <= args.t1]
    if len(rows) < 50:
        raise SystemExit(f"only {len(rows)} usable rows in that window")

    print(f"{len(rows)} rows, elapsed {rows[0][0]:.1f}..{rows[-1][0]:.1f} s")

    # Integrate the differential rate. Gaps matter: the analyser rejects captures through a
    # transient, and a gap is exactly where the rate is changing fastest, so integrating across
    # one silently invents the part that matters most. Report the gap coverage rather than
    # hiding it.
    acc, gapped, covered = 0.0, 0.0, 0.0
    base = acc_rows[0][1] if (acc_rows := [r for r in rows if math.isfinite(r[1])]) else 0.0
    pred = [(rows[0][0], base)]
    for i in range(1, len(rows)):
        dt = rows[i][0] - rows[i - 1][0]
        if dt <= 0:
            continue
        if dt > 0.5:
            gapped += dt
            continue
        covered += dt
        fa, fb = rows[i][2], rows[i][3]
        acc += -(fb - fa) / ((fa + fb) / 2.0) * dt * 1e6
        pred.append((rows[i][0], base + acc))

    acc_rows = [r for r in rows if math.isfinite(r[1])]
    if len(acc_rows) < 20:
        raise SystemExit("too few accepted skew rows to compare against")
    measured = acc_rows[-1][1] - acc_rows[0][1]
    span = rows[-1][0] - rows[0][0]
    print(f"  time covered {covered:.1f} s, SKIPPED IN GAPS {gapped:.1f} s "
          f"({100*gapped/span:.0f}% of the window)")
    print(f"\n  measured skew change : {measured:+9.1f} us")
    print(f"  integral of rate diff: {acc:+9.1f} us")
    print(f"  unexplained residual : {measured - acc:+9.1f} us")

    # Track quality, not just endpoints: two curves can share endpoints and disagree throughout.
    ys = [p[1] for p in pred]
    idx = {round(p[0], 3): p[1] for p in pred}
    pairs = [(idx[round(r[0], 3)], r[1]) for r in acc_rows if round(r[0], 3) in idx]
    if len(pairs) > 20:
        xs = [a for a, _ in pairs]
        zs = [b for _, b in pairs]
        n = len(xs)
        mx, mz = sum(xs) / n, sum(zs) / n
        sxy = sum((x - mx) * (z - mz) for x, z in zip(xs, zs))
        sxx = sum((x - mx) ** 2 for x in xs)
        syy = sum((z - mz) ** 2 for z in zs)
        if sxx > 0 and syy > 0:
            print(f"\n  predicted vs measured TRACK: r = {sxy/math.sqrt(sxx*syy):+.4f}  "
                  f"slope = {sxy/sxx:+.3f}")
        resid = [z - x for x, z in pairs]
        print(f"  residual over the track: mean {statistics.mean(resid):+.1f} us  "
              f"sd {statistics.pstdev(resid):.1f}  max |{max(abs(v) for v in resid):.0f}|")


if __name__ == "__main__":
    main()
