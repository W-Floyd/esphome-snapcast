#!/usr/bin/env python3
"""Test the padding-displacement prediction stated in snapcast_client.cpp.

The claim, from the comment at `dbg_padded_frames`:

    "a padded frame moves the audio one frame later while every metric still agrees with
     itself. Two devices differing by N frames of padding should sit N * (1e6 / rate) us
     apart on a logic analyser, which is the prediction to test."

This is that test. Padding is silence the SINK inserts in front of our audio when the ring
underruns; the accounting counts only real frames, so padded frames displace the output while
leaving every internal metric self-consistent -- which is exactly the signature measured on
these boards: acoustic offset stable to sd 5.8 us while both boards' own reported error
wanders +-60 us.

Requires the PADDISP log line (pad= is the last field of RECON, which the logger truncates
mid-number -- it reads "pad=882" for a counter in the tens of millions).

    python3 scripts/padding-test.py a.log b.log test.csv
    python3 scripts/padding-test.py a.log b.log test.csv --tail 400000
"""

import argparse
import bisect
import datetime
import math
import re
import statistics

PAD = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*PADDISP pad=(\d+) clamp=(-?\d+)")


def load_pad(path, tail):
    """(second_of_day, pad_frames, clamp_frames), file-position anchored.

    Never matched on timestamp: these logs span days with no date in the line, and timestamp
    matching has silently returned a previous day's build more than once.
    """
    out = []
    for l in open(path, errors="replace").read().splitlines()[-tail:]:
        m = PAD.match(l)
        if not m:
            continue
        t = (int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
             + int(m.group(4)) / 1000.0)
        out.append((t, int(m.group(5)), int(m.group(6))))
    return out


def at(series, t, tol=4.0):
    ts = [x[0] for x in series]
    i = bisect.bisect_left(ts, t)
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(series) and abs(series[j][0] - t) <= tol:
            if best is None or abs(series[j][0] - t) < abs(series[best][0] - t):
                best = j
    return series[best] if best is not None else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a_log")
    ap.add_argument("b_log")
    ap.add_argument("csv")
    ap.add_argument("--tail", type=int, default=600000)
    ap.add_argument("--rate", type=float, default=44100.0)
    args = ap.parse_args()

    A, B = load_pad(args.a_log, args.tail), load_pad(args.b_log, args.tail)
    print(f"PADDISP samples: A={len(A)}  B={len(B)}")
    if not A or not B:
        raise SystemExit("no PADDISP lines -- is the firmware carrying that log line?")

    frame_us = 1e6 / args.rate
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
        pa, pb = at(A, sod), at(B, sod)
        if pa and pb:
            rows.append((sod, off / 1000.0, pb[1] - pa[1], pb[2] - pa[2]))

    print(f"paired rows: {len(rows)}")
    if len(rows) < 30:
        raise SystemExit("not enough paired rows")

    skew = [r[1] for r in rows]
    dpad = [r[2] for r in rows]
    pred = [d * frame_us for d in dpad]
    resid = [s - p for s, p in zip(skew, pred)]

    print(f"\nframe = {frame_us:.3f} us")
    print(f"  measured skew      mean {statistics.mean(skew):+9.1f} us  sd {statistics.pstdev(skew):7.1f}")
    print(f"  pad_B - pad_A      mean {statistics.mean(dpad):+9.1f} frames "
          f"= {statistics.mean(pred):+.1f} us")
    print(f"  residual           mean {statistics.mean(resid):+9.1f} us  sd {statistics.pstdev(resid):7.1f}")

    n = len(skew)
    mx, my = statistics.mean(pred), statistics.mean(skew)
    sxy = sum((x - mx) * (y - my) for x, y in zip(pred, skew))
    sxx = sum((x - mx) ** 2 for x in pred)
    syy = sum((y - my) ** 2 for y in skew)
    if sxx > 0 and syy > 0:
        r = sxy / math.sqrt(sxx * syy)
        print(f"\n  predicted vs measured:  r = {r:+.4f}   slope = {sxy/sxx:+.4f}")
        print("  the prediction is slope +1.0 and r -> 1; a slope near 0 refutes it")
    else:
        print("\n  padding difference is CONSTANT over this window -- it cannot explain a "
              "changing skew, but a constant offset is still consistent with it. Re-run "
              "across a resync, where padding should step.")


if __name__ == "__main__":
    main()
