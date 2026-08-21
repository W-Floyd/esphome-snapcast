#!/usr/bin/env python3
"""Inter-device sync from two snapclient logs.

Each sync report's `median` is that device's playout error against the shared TSF
mapping, so the pair's relative sync is median_A - median_B at (nearly) the same
wall-clock moment. Capture both logs on ONE machine so the timestamp prefixes share
a clock, e.g. in two terminals:

    esphome logs example/esp32-s3-supermini.yaml --device <ip-A> | tee a.log
    esphome logs example/esp32-s3-supermini.yaml --device <ip-B> | tee b.log

then:  python3 scripts/sync-delta.py a.log b.log

Pairs each A report with the nearest B report within --window seconds (medians move
slowly -- the published mapping slews <= 50 us/s -- so 2 s pairing error contributes
~100 us worst case, usually far less). Prints per-pair deltas and summary stats.
Works on growing files with --follow.
"""

import argparse
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


def pair_and_report(a, b, window):
    deltas = []
    bi = 0
    for ta, ma in a:
        # nearest B report in time
        while bi + 1 < len(b) and abs(b[bi + 1][0] - ta) <= abs(b[bi][0] - ta):
            bi += 1
        if not b or abs(b[bi][0] - ta) > window:
            continue
        tb, mb = b[bi]
        deltas.append(ma - mb)
        print(f"t={ta:9.3f}  A={ma:+6d} us  B={mb:+6d} us  A-B={ma - mb:+6d} us")
    if deltas:
        mean = statistics.fmean(deltas)
        std = statistics.pstdev(deltas) if len(deltas) > 1 else 0.0
        print(f"\npairs={len(deltas)}  mean={mean:+.0f} us  std={std:.0f} us  "
              f"min={min(deltas):+d}  max={max(deltas):+d} us")
    else:
        print("no overlapping sync reports found", file=sys.stderr)
    return deltas


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log_a")
    ap.add_argument("log_b")
    ap.add_argument("--window", type=float, default=2.0, help="max pairing distance, seconds (default 2)")
    ap.add_argument("--follow", action="store_true", help="re-read and report every 10 s")
    args = ap.parse_args()

    while True:
        pair_and_report(parse(args.log_a), parse(args.log_b), args.window)
        if not args.follow:
            break
        time.sleep(10)
        print("\n--- refresh ---")


if __name__ == "__main__":
    main()
