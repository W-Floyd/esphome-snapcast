#!/usr/bin/env python3
"""Grade a span against the plan's definition of done (PLAN-sub-microsecond.md, R11.2).

The DoD's two statistics want OPPOSITE normalisations and are therefore computed separately -- this
is R2.4/R10.1/R11.2's whole history in one function. Both are drawn from the SAME span:

  p2p      N-FIXED    >= 6 disjoint 1000-sample rival-clean blocks; median block-p2p <= 1 us AND
                      worst <= 2 us. n-fixed because p2p is an extreme-value statistic that grows
                      with sample count -- fixing n is what makes it comparable to every earlier
                      measurement in the plan.
  mean/SE  TIME-FIXED >= 6 disjoint blocks of >= 5 minutes spanning >= 30 minutes; |mean| <= 0.2 us
                      with SE <= 0.1 us computed from the BLOCK MEANS' variance, not sd/sqrt(n).
                      Time-fixed because the wander's correlation time is 60-120 s: six blocks that
                      are all one draw of the wander report a small SE and FALSE-PASS the gate.

SE CAVEAT (R8.6): from six blocks the SE sits on 5 df (~+-30% relative), so SE <= 0.1 us is
INDICATIVE at six blocks and binding only at >= 20.

CAPTURE CONFIG IS PART OF THE RESULT (R10.1): rows/s is printed with every number, because the row
rate silently sets every n-dependent statistic. A 1000-sample block is 5 minutes at 3.3 rows/s and
26 seconds at 38 rows/s -- the same words, a different test.

USAGE
    python3 scripts/bench/dod-grade.py [--csv test.csv] [--from-min N --to-min M] [--max-rival 0.5]
"""
import argparse
import csv
import statistics as st
import sys


def load(path, max_rival, from_min, to_min):
    out, t0 = [], None
    with open(path) as fh:
        rows = (r for r in fh if not r.startswith("#"))
        for r in csv.DictReader(rows):
            try:
                t = float(r["unix_s"])
                o = float(r["offset_ns"]) / 1000.0
            except (ValueError, KeyError, TypeError):
                continue
            if o != o:
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
            out.append((t, o))
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--csv", default="test.csv")
    p.add_argument("--from-min", type=float, default=None)
    p.add_argument("--to-min", type=float, default=None)
    p.add_argument("--max-rival", type=float, default=0.5)
    p.add_argument("--block-n", type=int, default=1000)
    p.add_argument("--block-s", type=float, default=300.0)
    a = p.parse_args()

    d = load(a.csv, a.max_rival, a.from_min, a.to_min)
    if len(d) < a.block_n * 6:
        sys.exit(f"only {len(d)} rival-clean samples; need >= {a.block_n*6}")
    span = d[-1][0] - d[0][0]
    rate = len(d) / span
    vals = [x for _, x in d]
    print(f"{a.csv}: n={len(d)} rival-clean (<= {a.max_rival}), span {span/60:.1f} min, "
          f"{rate:.1f} rows/s")
    print(f"  whole-span: median {st.median(vals):+.2f}  mean {st.mean(vals):+.2f}  "
          f"p2p {max(vals)-min(vals):.1f} us   [p2p quoted for context only -- not a gate]")

    # ---- p2p, N-FIXED ----
    nb = len(d) // a.block_n
    p2ps = []
    for i in range(nb):
        v = [x for _, x in d[i*a.block_n:(i+1)*a.block_n]]
        p2ps.append(max(v) - min(v))
    med_p2p, worst = st.median(p2ps), max(p2ps)
    blk_s = a.block_n / rate
    ok_p2p = nb >= 6 and med_p2p <= 1.0 and worst <= 2.0
    print(f"\n  P2P (n-fixed, {nb} disjoint {a.block_n}-sample blocks = {blk_s:.0f} s each)")
    print(f"    median block-p2p {med_p2p:8.2f} us   (gate <= 1.0)")
    print(f"    worst  block-p2p {worst:8.2f} us   (gate <= 2.0)")
    lo = sorted(vals)[int(0.005*len(vals))]
    hi = sorted(vals)[int(0.995*len(vals))]
    print(f"    p0.5/p99.5 {lo:+.2f} / {hi:+.2f} us  (spread {hi-lo:.2f})")
    print(f"    -> {'PASS' if ok_p2p else 'FAIL'}")

    # ---- mean/SE, TIME-FIXED ----
    tb, cur, start = [], [], d[0][0]
    for t, x in d:
        if t - start >= a.block_s:
            if len(cur) >= 50:
                tb.append(st.mean(cur))
            cur, start = [], t
        cur.append(x)
    if len(cur) >= 50:
        tb.append(st.mean(cur))
    print(f"\n  MEAN/SE (time-fixed, {len(tb)} disjoint {a.block_s/60:.0f}-min blocks "
          f"spanning {span/60:.1f} min)")
    if len(tb) < 2:
        print("    -- too few time blocks --")
        return
    gmean = st.mean(tb)
    se = st.stdev(tb) / len(tb) ** 0.5
    ok_mean = len(tb) >= 6 and span >= 1800 and abs(gmean) <= 0.2 and se <= 0.1
    print(f"    block means: {' '.join(f'{x:+.2f}' for x in tb)}")
    print(f"    mean of blocks {gmean:+.3f} us   (gate |mean| <= 0.2)")
    print(f"    SE (block-means variance, {len(tb)-1} df) {se:.3f} us   (gate <= 0.1)")
    if len(tb) < 20:
        print(f"    NOTE: SE on {len(tb)-1} df is INDICATIVE only (R8.6); binding at >= 20 blocks")
    print(f"    -> {'PASS' if ok_mean else 'FAIL'}")

    print(f"\n  DoD (this span): {'PASS' if (ok_p2p and ok_mean) else 'FAIL'}"
          f"   [needs to hold twice, on different days]")


if __name__ == "__main__":
    main()
