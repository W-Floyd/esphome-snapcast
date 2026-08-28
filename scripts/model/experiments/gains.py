#!/usr/bin/env python3
"""Does the shipped tuning sit where the measurements say it should?

Three claims from TIMING.md, each of which the model can either reproduce or fail to:

  1. "raising gain makes it worse" -- the residual is variance, not lag, so a faster loop
     chases noise into the output.
  2. gain is bounded BELOW by disturbance tracking, not by settling: an integrator plant
     trails a ramp by rate/Kp.
  3. more averaging is free when there is no ramp to trail.

Together those predict an interior optimum in Kp, and the shipped value should be near it.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np

import analytic
import params as P
import sim

SEEDS = range(1, 9)


def run(dur=240.0, kp=None, window=None, fb=None):
    if window:
        saved, P.MEDIAN_WINDOW = P.MEDIAN_WINDOW, window
    sds, meds = [], []
    try:
        for seed in SEEDS:
            sp = sim.two_board_bench(duration_s=dur, seed=seed)
            for d in sp.devices:
                if kp:
                    d.kp = kp
                if fb:
                    d.feedback_interval_us = fb
            out = sim.Sim(sp).run()
            w = sim.skew(out)
            sds.append(w.std())
            meds.append(np.median(np.abs(out["a"]["median_us"])))
    finally:
        if window:
            P.MEDIAN_WINDOW = saved
    return float(np.mean(sds)), float(np.mean(meds))


def main():
    print("Kp sweep (shipped: %.2f run / %.2f acquire)" % (P.TRIM_KP_RUN, P.TRIM_KP_ACQUIRE))
    print("    Kp   wire sd   |sync median|   tracking lag   phase margin")
    for kp in (0.02, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0):
        sd, med = run(kp=kp)
        print(f"  {kp:5.2f}   {sd:7.2f}   {med:11.1f}   {analytic.tracking_lag_us(100.0, kp):11.0f} us"
              f"   {analytic.phase_margin_deg(kp):8.1f} deg")

    print("\nmedian window sweep (shipped: %d)" % P.MEDIAN_WINDOW)
    for w in (7, 15, 31, 63, 127):
        sd, med = run(window=w)
        print(f"  window {w:4d}   wire sd {sd:6.2f}   |sync median| {med:7.1f}")

    print("\nfeedback interval sweep (measured on the boards: 10 ms, 441 frames)")
    for fb in (1000.0, 5000.0, 10000.0, 20000.0, 40000.0):
        sd, med = run(fb=fb)
        lever = analytic.pivot_lever_us(257000.0, P.FB_ALPHA, fb) / 1e6
        print(f"  {fb/1000:5.1f} ms   wire sd {sd:6.2f}   |sync median| {med:7.1f}"
              f"   pivot lever {lever:5.2f} s -> {lever:5.2f} us/ppm")

    print("\nREADING IT")
    print("  An interior optimum in Kp is the model agreeing with the bench about WHY the")
    print("  gain is what it is: too low and the loop trails the timebase wander, too high")
    print("  and it converts feedback quantisation into differential rate jitter.")
    print("  The feedback interval matters twice over: it sets the quantisation rate AND the")
    print("  pivot lever, and the lever is what turns a ppm of trim into us of displacement")
    print("  that no on-device instrument can see.")


if __name__ == "__main__":
    main()
