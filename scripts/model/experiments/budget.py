#!/usr/bin/env python3
"""Where the inter-device residual comes from, by knocking each source out.

Read the two columns together. The analytic budget says what each term contributes on paper;
the knockout says what the loop actually does with it, which is not the same thing -- the
loop can amplify a term (it chases noise into the output) or absorb it (a common-mode term
cancels in the difference).
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np

import analytic
import params as P
import sim
import stats

SEEDS = range(1, 9)


def run(duration=240.0, **over):
    sds, meds = [], []
    for seed in SEEDS:
        sp = sim.two_board_bench(duration_s=duration, seed=seed,
                                 n_devices=over.pop("n_devices", 2))
        for k, v in over.items():
            if hasattr(sp, k):
                setattr(sp, k, v)
            else:
                for d in sp.devices:
                    setattr(d, k, v)
        w = sim.skew(sim.Sim(sp).run())
        sds.append(w.std())
        meds.append(np.median(w))
    return float(np.mean(sds)), float(np.mean(meds))


def main():
    print("ANALYTIC BUDGET (us, one device unless stated)")
    for k, v in analytic.error_budget_us().items():
        print(f"  {k:34s} {v:7.2f}")

    print("\nKNOCKOUTS -- wire skew B-A over 4 min, mean of "
          f"{len(list(SEEDS))} seeds")
    base_sd, base_med = run()
    print(f"  {'baseline':38s} sd {base_sd:6.2f}  median {base_med:+7.2f}")
    cases = [
        ("no TSF read jitter", dict(sandwich_jitter_us=0.0)),
        ("no Kalman wander", dict(kalman_wander_sd_us=0.0)),
        ("no beacon loss", dict(beacon_loss=0.0)),
        ("perfect offset rate (no filter lag)", dict(offset_rate_err_ppm=0.0)),
        ("feedback every 1 ms (less quantisation)", dict(feedback_interval_us=1000.0)),
        ("feedback every 40 ms", dict(feedback_interval_us=40000.0)),
        ("identical crystals", dict(crystal_ppm=P.CRYSTAL_PPM["a"])),
        ("everything quiet", dict(sandwich_jitter_us=0.0, kalman_wander_sd_us=0.0,
                                  beacon_loss=0.0, offset_rate_err_ppm=0.0)),
        ("everything quiet + 1 ms feedback", dict(sandwich_jitter_us=0.0,
                                                  kalman_wander_sd_us=0.0, beacon_loss=0.0,
                                                  offset_rate_err_ppm=0.0,
                                                  feedback_interval_us=1000.0)),
    ]
    for label, over in cases:
        sd, med = run(**over)
        print(f"  {label:38s} sd {sd:6.2f}  median {med:+7.2f}   "
              f"({(sd - base_sd) / base_sd * 100:+5.0f}% sd)")

    print("\nREADING IT")
    print("  The residual that survives 'everything quiet' is DETERMINISTIC: it is the")
    print("  played-frames feedback quantised to whole frames (22.7 us) entering the pivot,")
    print("  the PI turning that into rate jitter, and the two boards' jitter being")
    print("  independent. That is why shortening the feedback interval helps and why")
    print(f"  TIMING.md's residual is white noise rather than lag. Analytic differential")
    print(f"  quantisation floor: {analytic.differential(analytic.quantisation_sd_us()):.2f} us.")


if __name__ == "__main__":
    main()
