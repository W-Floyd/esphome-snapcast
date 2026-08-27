#!/usr/bin/env python3
"""Measure the analyser's own zero error and noise floor, so offsets can be corrected for it.

WHY THIS EXISTS. On 2026-08-27 the analyser insisted the two boards were 20-70 us apart while
raw-sync.py -- no probes, no prediction model in its path -- read them within +-3 us across five
independent windows. Swapping the two logic-analyser channels between the boards left the reading
at -23..-30 us: same sign, same magnitude, where a real difference must flip. So the analyser
carries a fixed ZERO ERROR between its channels, of the same order as the offsets being chased.

A step calibration cannot find this. Driving one board by a known 5 ms proved the SCALE to 1% and
said nothing about the bias, because a step is a difference and the bias cancels inside it.

TWO WAYS TO MEASURE IT, and the first is better.

  --common (default). Tie BOTH probes to the SAME signal -- one board's LRC line, both channels on
  the same pin. The true difference is then exactly zero BY CONSTRUCTION, so every microsecond the
  analyser reports is instrument: probe delay, channel skew, threshold asymmetry, sampling phase,
  the lot. No assumption about the boards is involved, and nothing has to hold still except the rig.

  It also yields something reversal cannot: with both channels on one wire the SPREAD of the
  readings is the analyser's own noise floor. That is the number that decides whether a sub-us
  claim can be verified at all, and it has never been measured.

  --reversal. In-situ, no rewiring: measure, swap the two probes between boards, measure again.

      probes normal:   R  = d + bias        bias = (R + R') / 2
      probes swapped:  R' = -d + bias       d    = (R - R') / 2

  Both fall out of the same two windows, but it assumes the boards' true offset d held still
  between them, and it cannot separate bias from noise.

USAGE
    python3 scripts/probe-cal.py                    # common-signal (recommended)
    python3 scripts/probe-cal.py --reversal         # in-situ, no rewiring
    python3 scripts/probe-cal.py --csv other.csv

Writes scripts/probe-cal.json, which i2s-skew.py subtracts from every subsequent offset.
"""

import argparse
import csv
import json
import math
import os
import statistics as st
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CAL_PATH = os.path.join(HERE, "probe-cal.json")
WIN_S = 60.0
MAX_MAD_US = 15.0
MAX_DRIFT_US = 6.0
SETTLE_TIMEOUT_S = 600.0


def read_offsets(path):
    out = []
    with open(path) as f:
        for row in csv.DictReader(l for l in f if not l.startswith("#")):
            try:
                u, v = float(row["unix_s"]), float(row["offset_ns"]) / 1000.0
            except (TypeError, ValueError, KeyError):
                continue
            if not math.isnan(v):
                out.append((u, v))
    return out


def settled_window(path, label, require_settled=True):
    """Stats over a settled window: (median, mad, sd, p2p, n).

    Gated on wall-clock freshness as well as spread -- a capture that has stopped writing would
    otherwise be graded on stale rows and read as beautifully quiet.
    """
    deadline = time.time() + SETTLE_TIMEOUT_S
    while time.time() < deadline:
        rows = read_offsets(path)
        now = time.time()
        if rows and rows[-1][0] >= now - 30:
            w = [v for u, v in rows if u >= now - WIN_S]
            if len(w) > 200:
                med = st.median(w)
                mad = st.median([abs(x - med) for x in w])
                half = len(w) // 2
                drift = abs(st.mean(w[half:]) - st.mean(w[:half]))
                print(f"    {label}: med {med:+8.2f} us  MAD {mad:5.2f}  sd {st.pstdev(w):5.2f}  "
                      f"drift {drift:5.2f}  n={len(w)}", flush=True)
                if not require_settled or (mad < MAX_MAD_US and drift < MAX_DRIFT_US):
                    return med, mad, st.pstdev(w), max(w) - min(w), len(w)
        time.sleep(15)
    return None


def save(cal):
    with open(CAL_PATH, "w") as f:
        json.dump(cal, f, indent=2)
        f.write("\n")
    print(f"\nwrote {CAL_PATH}")
    print("i2s-skew.py subtracts bias_us from every offset it reports from now on.")


def common_mode(args):
    print("=" * 78)
    print("COMMON-SIGNAL CALIBRATION")
    print("Tie BOTH analyser probes to the SAME pin -- one board's LRC line is ideal, since that")
    print("is the edge the offset is measured from. Ground both as usual. The true difference is")
    print("then exactly zero, so everything the analyser reports is its own error.")
    input("press enter once both probes are on the same signal> ")

    got = settled_window(args.csv, "common  ", require_settled=False)
    if got is None:
        sys.exit("no usable window -- is the capture running and seeing the signal on both channels?")
    med, mad, sd, p2p, n = got

    print("\n" + "=" * 78)
    print(f"  ZERO ERROR (bias)     {med:+8.2f} us     <- subtracted from every future reading")
    print(f"  NOISE FLOOR  MAD      {mad:8.2f} us     <- the finest difference this rig can resolve")
    print(f"               sd       {sd:8.2f} us")
    print(f"               p2p      {p2p:8.2f} us     over n={n}")
    print()
    print(f"  A claim smaller than ~{max(mad, 0.01):.2f} us cannot be verified with this rig as it stands.")
    if abs(med) > 1.0:
        print(f"  The {med:+.1f} us is a genuine channel asymmetry -- probe/cable/threshold, not the boards.")

    print("\nNow restore the probes to their normal orientation (channel A on board A).")
    input("press enter once restored> ")
    save({
        "bias_us": med,
        "noise_mad_us": mad,
        "noise_sd_us": sd,
        "noise_p2p_us": p2p,
        "samples": n,
        "when": time.strftime("%Y-%m-%d %H:%M:%S"),
        "method": "common signal: both probes on one pin, so the true difference is zero",
        "note": "Re-run after ANY change to probes, cabling, channel assignment or threshold.",
    })


def reversal_mode(args):
    print("=" * 78)
    print("REVERSAL CALIBRATION -- probes in their NORMAL orientation, both boards playing.")
    input("press enter when ready> ")
    first = settled_window(args.csv, "normal  ")
    if first is None:
        sys.exit("never settled -- the pair is moving, so the two halves would not be comparable")

    print("\n" + "=" * 78)
    print("Now SWAP THE TWO PROBES between the boards. Change nothing else.")
    input("press enter once swapped> ")
    second = settled_window(args.csv, "swapped ")
    if second is None:
        sys.exit("never settled after the swap -- rerun when the pair is quiet")

    r, rp = first[0], second[0]
    bias, true = (r + rp) / 2.0, (r - rp) / 2.0
    noise = max(first[1], second[1])

    print("\n" + "=" * 78)
    print(f"  normal   R  = {r:+9.2f} us")
    print(f"  swapped  R' = {rp:+9.2f} us")
    print(f"  ANALYSER BIAS = (R + R')/2 = {bias:+9.2f} us")
    print(f"  TRUE OFFSET   = (R - R')/2 = {true:+9.2f} us   (the boards, at that moment)")

    if abs(r - rp) < 3 * noise:
        # R and R' agreeing is ambiguous: either the pair really is aligned (d ~ 0, so reversal
        # changes nothing) or the probes were never swapped. Only a KNOWN offset separates them.
        print("\n  WARNING: R and R' agree to within the noise. Either the boards are genuinely")
        print("  aligned, or the probes were not actually swapped. Put a KNOWN offset on one board")
        print("  first (its server_latency number moves the deadline by exactly that many ms) and")
        print("  rerun: with 5 ms applied the readings must differ by ~10 ms. NOT saved.")
        return

    print("\nSwap the probes BACK to their normal orientation.")
    input("press enter once restored> ")
    save({
        "bias_us": bias,
        "true_offset_at_cal_us": true,
        "normal_us": r,
        "swapped_us": rp,
        "noise_mad_us": noise,
        "when": time.strftime("%Y-%m-%d %H:%M:%S"),
        "method": "probe reversal: bias=(R+R')/2, true=(R-R')/2",
        "note": "Re-run after ANY change to probes, cabling, channel assignment or threshold.",
    })


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", default=os.path.join(os.path.dirname(HERE), "test.csv"),
                    help="the live capture i2s-skew.py is writing (default: ./test.csv)")
    ap.add_argument("--reversal", action="store_true",
                    help="in-situ two-window reversal instead of the common-signal measurement")
    args = ap.parse_args()
    if not os.path.exists(args.csv):
        sys.exit(f"no capture at {args.csv} -- start i2s-skew.py first, this reads its live output")
    (reversal_mode if args.reversal else common_mode)(args)


if __name__ == "__main__":
    main()
