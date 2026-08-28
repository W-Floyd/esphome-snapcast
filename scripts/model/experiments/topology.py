#!/usr/bin/env python3
"""Timebase topology: leader vs consensus, stepping vs slewing, group size, beacon loss.

This is the experiment with live bench questions attached:

  * "consensus is worse than a leader" is NOT established -- the 3.6 us baseline was a
    TWO-device group and the 8.06 us consensus window was THREE. The model can separate
    those two variables, which the bench window cannot.
  * the adoption slew measured 2.7x worse than stepping (9.72 vs 3.6). Stepping is safe
    BECAUSE it is deterministic: every device holding the same estimate set adopts the same
    mapping, so its error is exactly common-mode. A slew gives each device its own history.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np

import params as P
import sim
import stats

SEEDS = range(1, 11)


def run(n_devices=2, leaderless=True, adopt="step", loss=0.05, dur=240.0):
    sds, meds, mad = [], [], []
    for seed in SEEDS:
        sp = sim.two_board_bench(duration_s=dur, seed=seed, leaderless=leaderless,
                                 n_devices=n_devices)
        sp.map_adopt = adopt
        sp.beacon_loss = loss
        w = sim.skew(sim.Sim(sp).run())     # always the a-b pair, whatever else is present
        sds.append(w.std())
        meds.append(np.median(w))
        mad.append(stats.mad(w))
    return float(np.mean(sds)), float(np.mean(meds)), float(np.mean(mad))


def main():
    print("wire skew of the A-B PAIR, whatever else is in the group "
          f"(mean of {len(list(SEEDS))} seeds, 4 min each)\n")
    print("  topology                                 sd     median    MAD")
    rows = [
        ("leader, 2 devices, step", dict(n_devices=2, leaderless=False)),
        ("consensus, 2 devices, step", dict(n_devices=2, leaderless=True)),
        ("consensus, 3 devices, step", dict(n_devices=3, leaderless=True)),
        ("leader, 3 devices, step", dict(n_devices=3, leaderless=False)),
        ("consensus, 2 devices, SLEW", dict(n_devices=2, leaderless=True, adopt="slew")),
        ("consensus, 3 devices, SLEW", dict(n_devices=3, leaderless=True, adopt="slew")),
        ("consensus, 3 devices, 20% beacon loss", dict(n_devices=3, loss=0.2)),
        ("consensus, 3 devices, no loss", dict(n_devices=3, loss=0.0)),
    ]
    for label, kw in rows:
        sd, med, m = run(**kw)
        print(f"  {label:38s} {sd:6.2f}   {med:+7.2f}  {m:6.2f}")

    print("\nPREDICTIONS THIS MAKES, both falsifiable on the bench")
    print("  1. GROUP SIZE is not the explanation. Adding a third device moves the pair's sd")
    print("     by a few per cent in the model, not by 2x. So moving the observer off the")
    print("     stream should NOT recover the 3.6 us baseline; if it does, the mechanism is")
    print("     something the model omits -- airtime, CPU contention or the extra beacon")
    print("     traffic itself, none of which are in here.")
    print("  2. STEPPING vs SLEWING is nearly free in the model (a few per cent), against")
    print("     2.7x measured. The model's slew is applied to a scalar offset; the firmware")
    print("     slews a LINE and each device re-bases it at its own reference instant, with")
    print("     its own averaged DRIFT. That per-device drift path is what the model lacks,")
    print("     and it is where a 2.7x would have to come from -- worth logging the adopted")
    print("     drift per device and differencing it before accepting any other story.")


if __name__ == "__main__":
    main()
