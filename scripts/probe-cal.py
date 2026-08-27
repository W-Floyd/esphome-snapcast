#!/usr/bin/env python3
"""Characterise the logic analyser itself: its zero error, its noise, and whether it WANDERS.

WHY THIS EXISTS. On 2026-08-27 the analyser insisted the two boards were 20-70 us apart while
raw-sync.py -- no probes, no prediction model in its path -- read them within +-3 us across five
independent windows. Swapping the two channels between the boards left the reading at -23..-30 us:
same sign, same magnitude, where a real difference must flip. So the rig carries a zero error of
the same order as the offsets being chased.

A step calibration cannot find it. Driving one board by a known 5 ms proved the SCALE to 1% and
said nothing about the bias, because a step is a difference and the bias cancels inside it.

THE MEASUREMENT. Tie EVERY probe to the SAME pin (one board's LRC line is ideal, since that is the
edge the offset is measured from). The true difference between channels is then exactly zero BY
CONSTRUCTION, so everything reported is instrument: probe delay, channel skew, threshold
asymmetry, sampling phase. Nothing has to hold still except the rig, and no assumption about the
boards enters.

Working on the RAW CAPTURE rather than i2s-skew.py's CSV, and on every channel pair at once:

  * The I2S decoder cannot be reused -- it needs BCLK, LRC and DIN per board, and with all pins on
    one wire there is no frame structure. Raw rising-edge timing is the right primitive and the more
    direct question: when did each channel see the same transition?
  * One capture gives every pair simultaneously, so nothing can drift between pairs.

STATIC OR WANDERING -- the question that decides whether the rig needs replacing. A static offset
is calibratable: measure once, subtract forever. A wandering one is not, and it puts a floor under
every offset this rig can ever report. Both timescales are measured:

  * WITHIN a capture, by splitting the edges into blocks -- fast wander and jitter.
  * ACROSS captures (--repeat), spaced by --interval -- slow drift, thermal, connector creep.

RESOLUTION. The analyser samples on a grid (41.7 ns at 24 MS/s), so one edge pair can only ever be
an integer number of samples apart. Sub-sample precision comes from the sampler NOT being
synchronous with the signal: the quantisation dithers across thousands of edges and the mean
converges well below one sample. The number of distinct sample-bins is reported per pair, because
if every edge lands in one bin the dither has failed and the mean is not sub-sample after all.

USAGE
    python3 scripts/probe-cal.py                              # one capture, all pairs
    python3 scripts/probe-cal.py --repeat 12 --interval 60    # is it static or does it wander?
    python3 scripts/probe-cal.py --reversal                   # in-situ, no rewiring, needs test.csv

Writes scripts/probe-cal.json, which i2s-skew.py subtracts from every subsequent offset.
"""

import argparse
import csv
import importlib.util
import json
import math
import os
import statistics as st
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CAL_PATH = os.path.join(HERE, "probe-cal.json")
# A channel needs this many rising edges before it counts as connected: an unconnected probe either
# floats or sits idle, and averaging it in would report the skew of noise.
MIN_EDGES = 200
# Edges dropped from each end: a capture boundary can clip one channel's edge and not another's,
# which would pair edge k on one channel with edge k+1 on the next.
EDGE_MARGIN = 5
# Blocks a capture is split into for intra-capture wander. Ten keeps each block well above
# MIN_EDGES for a default capture while still resolving movement within the capture.
BLOCKS = 10
# Reversal-mode window gating (CSV path only).
WIN_S, MAX_MAD_US, MAX_DRIFT_US, SETTLE_TIMEOUT_S = 60.0, 15.0, 6.0, 600.0


def load_skew_module():
    spec = importlib.util.spec_from_file_location("skew", os.path.join(HERE, "i2s-skew.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def edges_per_channel(skew, buf):
    """{channel: rising-edge sample indices} for channels carrying a usable signal."""
    out = {}
    for ch in range(8):
        e = skew.rising(skew.bit(buf, ch))
        if e.size >= MIN_EDGES:
            out[ch] = e
    return out


def aligned(edges):
    """Trim every channel's edge series to the same length, dropping the ends.

    The same signal is on every channel, so the k-th rising edge is the same physical transition --
    but only once the ends are dropped, where a capture boundary can clip one channel and not
    another.
    """
    n = min(e.size for e in edges.values()) - 2 * EDGE_MARGIN
    if n < MIN_EDGES // 2:
        return None, 0
    return {c: e[EDGE_MARGIN:EDGE_MARGIN + n].astype(float) for c, e in edges.items()}, n


def pair_stats(np, a_edges, b_edges, ns_per_sample):
    d = (a_edges - b_edges) * ns_per_sample
    bins = len(np.unique(np.round(d / ns_per_sample)))
    return {
        "mean": float(np.mean(d)),
        "median": float(np.median(d)),
        "sd": float(np.std(d)),
        "bins": bins,
        "blocks": [float(np.mean(b)) for b in np.array_split(d, BLOCKS)],
    }


def one_capture(skew, np, args, quiet=False):
    """Capture once and return (pairwise stats, channels, edges used, ns per sample)."""
    ns_per_sample = 1e9 / skew.args_rate_hz(args)
    buf = skew.capture_logic(args)
    edges = edges_per_channel(skew, buf)
    if len(edges) < 2:
        sys.exit("fewer than two channels carried a usable signal -- are all probes on the pin?")
    trimmed, n = aligned(edges)
    if trimmed is None:
        sys.exit("not enough common edges after trimming -- capture longer with --samples")
    chans = sorted(trimmed)
    stats = {}
    for i, a in enumerate(chans):
        for b in chans[i + 1:]:
            stats[(a, b)] = pair_stats(np, trimmed[a], trimmed[b], ns_per_sample)
    if not quiet:
        print(f"  channels {chans}, {n} common edges, {ns_per_sample:.1f} ns/sample")
    return stats, chans, n, ns_per_sample, trimmed


def print_matrix(stats, chans):
    print("\n  pairwise skew, ns (row - column, mean over all edges):")
    print("        " + "".join(f"{'D%d' % c:>11}" for c in chans))
    for a in chans:
        row = f"    D{a}  "
        for b in chans:
            if a == b:
                row += f"{'-':>11}"
            else:
                s = stats.get((a, b))
                v = s["mean"] if s else -stats[(b, a)]["mean"]
                row += f"{v:>+10.1f} "
        print(row)


def verdict(np, series, ns_per_sample, label):
    """Static or wandering? Reports the numbers that decide it, then says which."""
    mean = float(np.mean(series))
    sd = float(np.std(series))
    half = len(series) // 2
    drift = abs(float(np.mean(series[half:])) - float(np.mean(series[:half])))
    print(f"    {label}: mean {mean:+8.2f} ns   sd {sd:7.2f}   drift {drift:7.2f}   "
          f"range {min(series):+.1f}..{max(series):+.1f}")
    return mean, sd, drift


def matrix_mode(args):
    skew = load_skew_module()
    import numpy as np

    print("=" * 78)
    print("CHANNEL-SKEW MATRIX -- tie EVERY probe you use to the SAME pin, grounds as usual.")
    print("Every difference measured below is then the instrument, not the boards.")
    input("press enter once all probes are on the same signal> ")

    runs = []
    for i in range(args.repeat):
        print(f"\ncapture {i + 1}/{args.repeat} ({args.samples} samples at {args.samplerate})...")
        stats, chans, n, nsps, _ = one_capture(skew, np, args)
        runs.append(stats)
        if i == 0:
            print_matrix(stats, chans)
            print("\n  per-pair detail (mean is the population; median is what one edge resolves):")
            for (a, b), s in stats.items():
                flag = "" if s["bins"] > 1 else "   <- ONE sample bin: no dither, mean is not sub-sample"
                print(f"    D{a}-D{b}  mean {s['mean']:+8.2f}  median {s['median']:+8.2f}  "
                      f"sd {s['sd']:7.2f}  bins {s['bins']}{flag}")
            print("\n  within this capture, per block (fast wander):")
            for (a, b), s in stats.items():
                verdict(np, s["blocks"], nsps, f"D{a}-D{b}")
        if i + 1 < args.repeat:
            time.sleep(args.interval)

    print("\n" + "=" * 78)
    pairs = sorted(runs[0])
    summary = {}
    if len(runs) > 1:
        print(f"ACROSS {len(runs)} captures spaced {args.interval}s (slow drift):")
        for p in pairs:
            series = [r[p]["mean"] for r in runs if p in r]
            mean, sd, drift = verdict(np, series, nsps, f"D{p[0]}-D{p[1]}")
            summary[f"D{p[0]}-D{p[1]}"] = {"mean_ns": mean, "sd_ns": sd, "drift_ns": drift}
    else:
        print("single capture -- rerun with --repeat to separate a static offset from wander")
        for p in pairs:
            summary[f"D{p[0]}-D{p[1]}"] = {"mean_ns": runs[0][p]["mean"],
                                           "sd_ns": runs[0][p]["sd"], "drift_ns": None}

    # The verdict that matters: can this rig resolve the offsets being chased? A static offset is
    # calibratable at any size; wander is not, and it becomes the floor under every reading.
    worst_sd = max((v["sd_ns"] for v in summary.values()), default=0.0)
    worst_mean = max((abs(v["mean_ns"]) for v in summary.values()), default=0.0)
    print(f"\n  worst channel pair:  offset {worst_mean:.1f} ns ({worst_mean / 1000:.3f} us)"
          f"   wander sd {worst_sd:.1f} ns ({worst_sd / 1000:.3f} us)")
    if len(runs) > 1:
        if worst_sd < max(0.25 * worst_mean, nsps):
            print("  VERDICT: STATIC -- the offset is a fixed property of the rig, so subtracting it")
            print("  is legitimate, and the residual floor is the wander above.")
        else:
            print("  VERDICT: WANDERING -- the offset moves by as much as it is, so no constant can")
            print("  cancel it. Every absolute reading carries the wander, and a better probe or")
            print("  cabling is the only fix. Calibration saved anyway, but treat it as provisional.")

    print("\nRestore the probes to their normal positions before measuring boards again.")
    input("press enter once restored> ")

    # Save the bias for the pair i2s-skew.py actually uses. Its channel map is not a constant --
    # it comes from the PulseView session file, so read it the same way rather than assuming.
    try:
        chan_map = skew.parse_pvs(args.pvs or skew.DEFAULT_PVS)
    except Exception as e:  # a missing or unreadable .pvs must not lose the measurement above
        print(f"\n  could not read the channel map ({e}) -- bias NOT saved; read it from the matrix")
        return
    bias_us, used = 0.0, None
    if isinstance(chan_map, dict) and "LRC_ONE" in chan_map and "LRC_TWO" in chan_map:
        one, two = chan_map["LRC_ONE"], chan_map["LRC_TWO"]
        key = (min(one, two), max(one, two))
        if key in summary or f"D{key[0]}-D{key[1]}" in summary:
            s = summary[f"D{key[0]}-D{key[1]}"]
            # i2s-skew reports board TWO minus board ONE; flip if the pair is stored the other way.
            sign = 1.0 if key == (one, two) else -1.0
            bias_us = sign * s["mean_ns"] / 1000.0
            used = f"D{one} (board one) / D{two} (board two)"
    if used is None:
        print("\n  could not identify the LRC pair from i2s-skew.py's channel map -- bias NOT saved;")
        print("  read the pair you use from the matrix above and apply it by hand if needed.")
        return
    print(f"\n  LRC pair {used}: bias {bias_us:+.3f} us")
    save({
        "bias_us": bias_us,
        "wander_sd_ns": worst_sd,
        "ns_per_sample": nsps,
        "captures": len(runs),
        "interval_s": args.interval,
        "pairs_ns": summary,
        "when": time.strftime("%Y-%m-%d %H:%M:%S"),
        "method": "all probes on one signal; pairwise rising-edge timing on the raw capture",
        "note": "Re-run after ANY change to probes, cabling, channel assignment or threshold.",
    })


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


def settled_window(path, label):
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
                print(f"    {label}: med {med:+8.2f} us  MAD {mad:5.2f}  drift {drift:5.2f}  n={len(w)}",
                      flush=True)
                if mad < MAX_MAD_US and drift < MAX_DRIFT_US:
                    return med, mad
        time.sleep(15)
    return None


def reversal_mode(args):
    """In-situ, no rewiring: bias=(R+R')/2 and true=(R-R')/2 from two windows either side of a swap."""
    print("=" * 78)
    print("REVERSAL CALIBRATION -- probes in their NORMAL orientation, both boards playing.")
    input("press enter when ready> ")
    first = settled_window(args.csv, "normal  ")
    if first is None:
        sys.exit("never settled -- the pair is moving, so the two halves would not be comparable")
    print("\nNow SWAP THE TWO PROBES between the boards. Change nothing else.")
    input("press enter once swapped> ")
    second = settled_window(args.csv, "swapped ")
    if second is None:
        sys.exit("never settled after the swap -- rerun when the pair is quiet")

    r, rp = first[0], second[0]
    bias, true = (r + rp) / 2.0, (r - rp) / 2.0
    noise = max(first[1], second[1])
    print(f"\n  normal R = {r:+9.2f} us   swapped R' = {rp:+9.2f} us")
    print(f"  BIAS = (R+R')/2 = {bias:+9.2f} us     TRUE = (R-R')/2 = {true:+9.2f} us")
    if abs(r - rp) < 3 * noise:
        print("\n  WARNING: R and R' agree within the noise, which reads equally as 'the boards are")
        print("  aligned' and 'the probes were never swapped'. Put a known 5 ms on one board via its")
        print("  server_latency number and rerun: the readings must then differ by ~10 ms. NOT saved.")
        return
    print("\nSwap the probes BACK to their normal orientation.")
    input("press enter once restored> ")
    save({
        "bias_us": bias,
        "true_offset_at_cal_us": true,
        "noise_mad_us": noise,
        "when": time.strftime("%Y-%m-%d %H:%M:%S"),
        "method": "probe reversal: bias=(R+R')/2, true=(R-R')/2",
        "note": "Re-run after ANY change to probes, cabling, channel assignment or threshold.",
    })


def save(cal):
    with open(CAL_PATH, "w") as f:
        json.dump(cal, f, indent=2)
        f.write("\n")
    print(f"\nwrote {CAL_PATH}")
    print("i2s-skew.py subtracts bias_us from every offset it reports from now on.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reversal", action="store_true",
                    help="in-situ two-window reversal from i2s-skew.py's CSV instead of capturing")
    ap.add_argument("--csv", default=os.path.join(os.path.dirname(HERE), "test.csv"),
                    help="live capture for --reversal (default: ./test.csv)")
    ap.add_argument("--repeat", type=int, default=1,
                    help="captures to take; >1 separates a static offset from wander")
    ap.add_argument("--interval", type=float, default=60.0,
                    help="seconds between captures (default 60)")
    # Capture settings mirror i2s-skew.py so the calibration measures the SAME rig.
    ap.add_argument("--samples", type=int, default=2_400_000)
    ap.add_argument("--samplerate", default="24M")
    ap.add_argument("--driver", default="fx2lafw")
    ap.add_argument("--conn", default=None)
    ap.add_argument("--sigrok-cli", dest="sigrok_cli", default="sigrok-cli")
    ap.add_argument("--timeout", type=float, default=120.0)
    # The channel map lives in the PulseView session file, same as for i2s-skew.py. Default is
    # resolved from that module at run time so the two cannot drift apart.
    ap.add_argument("--pvs", default=None)
    # i2s-skew.py's capture path reads these; harmless here.
    ap.add_argument("--bits", type=int, default=32)
    ap.add_argument("--bit-delay", type=int, default=1)
    args = ap.parse_args()

    if args.reversal:
        if not os.path.exists(args.csv):
            sys.exit(f"no capture at {args.csv} -- start i2s-skew.py first")
        reversal_mode(args)
    else:
        matrix_mode(args)


if __name__ == "__main__":
    main()
