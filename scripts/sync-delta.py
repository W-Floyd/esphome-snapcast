#!/usr/bin/env python3
"""Inter-device sync from any number of snapclient logs.

Each sync report's `median` is that device's playout error against the shared TSF
mapping, so relative sync between any two devices is the difference of their
medians at (nearly) the same wall-clock moment. Capture all logs on ONE machine so
the timestamp prefixes share a clock, e.g. one terminal per device:

    esphome logs example/esp32-s3-supermini.yaml --device <ip> | tee <name>.log

then:  python3 scripts/sync-delta.py a.log b.log c.log d.log

The first log is the time base: each of its reports is paired with the nearest
report of every other device within --window seconds (medians move slowly -- the
published mapping slews <= 50 us/s steady-state -- so pairing error contributes
little). Prints per-row medians + spread, then a pairwise mean/std matrix.
Works on growing files with --follow.
"""

import argparse
import itertools
import os
import re
import statistics
import sys
import time

# "[12:36:12.471]...median -204 us" (ms in the timestamp optional)
LINE_RE = re.compile(r"\[(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?\].*median (-?\d+)\s*us")


def parse(path):
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE_RE.search(line)
            if not m:
                continue
            h, mi, s, ms, med = m.groups()
            t = int(h) * 3600 + int(mi) * 60 + int(s) + (int(ms.ljust(3, "0")) / 1000 if ms else 0)
            out.append((t, int(med)))
    return out


def nearest(series, t):
    """Nearest (t, median) in a sorted series, or None if empty."""
    if not series:
        return None
    lo, hi = 0, len(series) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if series[mid][0] < t:
            lo = mid + 1
        else:
            hi = mid
    best = series[lo]
    if lo > 0 and abs(series[lo - 1][0] - t) < abs(best[0] - t):
        best = series[lo - 1]
    return best


def pair_and_report(names, all_series, window, quiet_rows):
    # Time base: the series with the most reports (a rotated/idle log must not
    # veto the whole fleet). Devices missing a match at a row show as "--".
    base_idx = max(range(len(all_series)), key=lambda i: len(all_series[i]))
    rows = []  # (t, [med or None per device])
    for t, _ in all_series[base_idx]:
        meds = []
        for series in all_series:
            hit = nearest(series, t)
            meds.append(hit[1] if hit is not None and abs(hit[0] - t) <= window else None)
        if sum(m is not None for m in meds) >= 2:
            rows.append((t, meds))

    if not quiet_rows:
        header = "  ".join(f"{n:>8s}" for n in names)
        print(f"{'t':>10s}  {header}  {'spread':>7s}")
        for t, meds in rows:
            cells = "  ".join(f"{m:+8d}" if m is not None else f"{'--':>8s}" for m in meds)
            present = [m for m in meds if m is not None]
            print(f"{t:10.3f}  {cells}  {max(present) - min(present):7d}")

    if not rows:
        print("no overlapping sync reports found", file=sys.stderr)
        return

    n = len(names)
    print(f"\npairs (mean/std of medianX - medianY, us; base: {names[base_idx]}):")
    width = max(len(x) for x in names) + 1
    for i, j in itertools.combinations(range(n), 2):
        deltas = [meds[i] - meds[j] for _, meds in rows if meds[i] is not None and meds[j] is not None]
        if not deltas:
            print(f"  {names[i]:>{width}s} - {names[j]:<{width}s}  (no overlap)")
            continue
        mean = statistics.fmean(deltas)
        std = statistics.pstdev(deltas) if len(deltas) > 1 else 0.0
        print(f"  {names[i]:>{width}s} - {names[j]:<{width}s}  n={len(deltas):4d}  mean={mean:+7.0f}  "
              f"std={std:6.0f}  min={min(deltas):+7d}  max={max(deltas):+7d}")
    spreads = [max(p) - min(p) for _, meds in rows if (p := [m for m in meds if m is not None])]
    print(f"  group spread: mean={statistics.fmean(spreads):.0f} us  max={max(spreads)} us  rows={len(rows)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="two or more log files; the first is the time base")
    ap.add_argument("--window", type=float, default=2.0, help="max pairing distance, seconds (default 2)")
    ap.add_argument("--follow", action="store_true", help="re-read and report every 10 s")
    ap.add_argument("--summary", action="store_true", help="skip per-row output, print only the pair stats")
    args = ap.parse_args()
    if len(args.logs) < 2:
        ap.error("need at least two logs")

    names = [os.path.splitext(os.path.basename(p))[0] for p in args.logs]
    while True:
        pair_and_report(names, [parse(p) for p in args.logs], args.window, args.summary)
        if not args.follow:
            break
        time.sleep(10)
        print("\n--- refresh ---")


if __name__ == "__main__":
    main()
