#!/usr/bin/env python3
"""The open question: why does the on-device differential phase ANTI-correlate with the wire?

Bench: r = -0.92..-0.98 at comparable amplitude, replicated across bucket widths and windows
(and again here, at r = -0.96 over 38 one-minute buckets, by check_real.py).

The arithmetic demands +1. If board B renders a given server frame late by d, its
(pushed - played) is larger by d*rate, so render_server is smaller by d, so
phase = render_tsf - render_server is larger by d. The model computes exactly that
expression from exactly those terms, and reproduces +1.

So one of three things is true, and this experiment is about telling them apart:

  A. the instrument carries an error PROPORTIONAL to the device's own displacement, with a
     gain near -2. Nothing else turns +1 into -1 at equal amplitude.
  B. the instrument carries a large INDEPENDENT error (the "additive floor"). This destroys
     the correlation toward 0 -- it cannot make it negative. Whatever the floor explains, it
     does not explain a reproducible r = -0.96.
  C. the WIRE's differential carries the inverted term, and the on-device series is right.

This scans A and B in the model and prints what each does to (r, slope, amplitude ratio), so
the bench numbers can be matched against a shape rather than a story.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np

import params as P
import sim
import stats

SEEDS = range(1, 7)
BUCKET_S = 30.0


def measure(gain=0.0, err_sd=0.0, dur=600.0):
    rs, slopes, ratios = [], [], []
    for seed in SEEDS:
        sp = sim.two_board_bench(duration_s=dur, seed=seed)
        for d in sp.devices:
            d.phase_error_gain = gain
            d.phase_error_sd = err_sd
        out = sim.Sim(sp).run()
        t = out["a"]["t_s"]
        wire = sim.skew(out)
        dev = sim.skew(out, field="phase_us")
        _, wb = stats.bucket_medians(t, wire, BUCKET_S)
        _, db = stats.bucket_medians(t, dev, BUCKET_S)
        n = min(wb.size, db.size)
        wb, db = wb[:n] - np.median(wb[:n]), db[:n] - np.median(db[:n])
        sl, _, _, _ = stats.robust_fit(wb, db)
        rs.append(stats.corr(wb, db))
        slopes.append(sl)
        ratios.append(db.std() / max(wb.std(), 1e-9))
    return float(np.mean(rs)), float(np.mean(slopes)), float(np.mean(ratios))


def main():
    print(f"{BUCKET_S:.0f} s bucket medians, B - A, mean of {len(list(SEEDS))} seeds, 10 min each")
    print("\nA. error proportional to own displacement (gain g on each device's own truth)")
    print("      g        r     slope   amplitude ratio")
    for g in (0.0, -0.5, -1.0, -1.5, -2.0, -3.0):
        r, sl, ratio = measure(gain=g)
        print(f"  {g:+5.1f}   {r:+6.3f}   {sl:+6.3f}   {ratio:8.2f}")

    print("\nB. independent additive error (the floor), no proportional term")
    print("   sd (us)     r     slope   amplitude ratio")
    for sd_ in (0.0, 5.0, 10.0, 20.0, 40.0):
        r, sl, ratio = measure(err_sd=sd_)
        print(f"  {sd_:7.1f}   {r:+6.3f}   {sl:+6.3f}   {ratio:8.2f}")

    print("\nBENCH, for comparison (check_real.py on a 45 min window, 2026-08-28)")
    print("   60 s buckets, n=38:   r -0.965   slope -0.582   wire sd 43.3   on-device sd 28.6")
    print("   30 s buckets, n=75:   r -0.637   slope -0.752   wire sd 48.8   on-device sd 56.6")
    print("   sub-frame residue of the wire (offset - frame_lag*22.676):")
    print("                          r +0.01    sd 1.3-1.8 us   <- carries none of it")

    print("\nWHAT THAT LAST LINE MEANS")
    print("  The wire's wander in that window is ENTIRELY its whole-frame term (frame_lag),")
    print("  and the sub-frame edge measurement -- the part that could plausibly carry a sign")
    print("  error -- is flat at 1.3-1.8 us. So the anti-correlation is between two WHOLE-FRAME")
    print("  quantities: the analyser's correlation lag and the firmware's (pushed - played).")
    print("  Both count frames of the same physical skew. A shape that produces r = -1 between")
    print("  two frame counters is one counting what the other has already removed, which is")
    print("  what hypothesis A's g = -2 is describing. The next measurement is therefore not")
    print("  another correlation: it is to log, per board, the frame counts each instrument")
    print("  used for the SAME chunk and difference them directly.")


if __name__ == "__main__":
    main()
