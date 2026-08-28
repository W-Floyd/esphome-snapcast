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
import shutil
import statistics
import sys
import time

# "[12:36:12.471]...median -204 us" (ms in the timestamp optional)
LINE_RE = re.compile(r"\[(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?\].*median (-?\d+)\s*us")
# "...pipeline 171 ms" on the same report line
DEPTH_RE = re.compile(r"pipeline (-?\d+) ms")
# Deviation from the group median that gets flagged. Compared per-device MEDIAN to
# group median, healthy devices sit within a few ms of each other (measured: -2, +2,
# +5 across three boards) while the audibly-late outlier sat at -79 with its entire
# range, 156-200, clear of the fleet's 242-292. So the separation is ~15x the healthy
# scatter and the threshold belongs well below the observed case, not just under it.
# (Do not derive this from raw min-max ranges: depth sawtooths ~45 ms per device, so
# the ranges overlap far more than the medians do -- an 80 ms threshold read off the
# raw spread let the real -79 ms case slip through unflagged.)
DEPTH_OUTLIER_MS = 40
# Depth swings legitimately while a device refills after a starvation; only the tail
# of the series is compared so a recovery minutes ago does not read as an offset now.
DEPTH_TAIL = 12


REFRESH_S = 10.0


def rows_that_fit(n_names):
    """How many per-row lines leave the pair summary on screen."""
    pairs = n_names * (n_names - 1) // 2
    # header + elision note + blank + "pairs (...)" + one line per pair
    # + group spread + refresh footer (blank + line) + prompt slack
    overhead = 2 + 2 + pairs + 1 + 2 + 2
    return max(1, shutil.get_terminal_size((80, 24)).lines - overhead)


def parse(path):
    """Timestamps are wall-clock time-of-day with no date, so an overnight log
    wraps at midnight. nearest() binary-searches and the pair maths differences
    these values, so the series MUST stay monotonic: unwrap each backwards jump
    into the following day instead of letting it rewind ~86400 s."""
    out = []
    depth = []
    day = 0
    prev = None
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE_RE.search(line)
            if not m:
                continue
            h, mi, s, ms, med = m.groups()
            t = int(h) * 3600 + int(mi) * 60 + int(s) + (int(ms.ljust(3, "0")) / 1000 if ms else 0)
            # A large step backwards is midnight; a small one is just two log
            # lines racing within the same second, which must not add a day.
            if prev is not None and t + 43200 < prev - day * 86400:
                day += 1
            prev = t + day * 86400
            out.append((prev, int(med)))
            d = DEPTH_RE.search(line)
            if d:
                depth.append(int(d.group(1)))
    return out, depth[-DEPTH_TAIL:]


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


def pair_and_report(names, parsed, window, quiet_rows, max_rows=None):
    all_series = [ser for ser, _ in parsed]
    depths = {n: d for n, (_, d) in zip(names, parsed)}
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
        # A redrawn screen holds only so many rows; show the newest that fit, since
        # the summary below them is what a follow session is watching.
        shown = rows if max_rows is None else rows[-max_rows:]
        header = "  ".join(f"{n:>8s}" for n in names)
        if len(shown) < len(rows):
            print(f"({len(rows) - len(shown)} earlier rows above the screen)")
        print(f"{'t':>10s}  {header}  {'spread':>7s}")
        for t, meds in shown:
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
    report_depth(names, depths)


def report_depth(names, depths):
    """Playout pipeline depth vs the group median.

    The pair stats above CANNOT see an absolute playout offset. Each device's median
    is measured against its own predicted playout, so if its frame accounting is off
    by F the prediction is off by F too and the error reads ~0 while the audio is
    physically F late. Depth (pushed-but-unplayed audio) is the one exposed number
    that moves with F: measured twice on hardware, a device sitting shallower than the
    fleet was audibly BEHIND by roughly the deficit (-150 ms deficit -> ~150 ms late,
    -105 ms -> audibly late again), with textbook-clean medians throughout.

    The group median is the reference because no single device is authoritative -- a device
    that hears no beacons cannot self-check on-device at all.
    """
    have = {k: v for k, v in depths.items() if v}
    if len(have) < 2:
        return
    # Per-device MEDIAN of the tail, not its latest sample: depth sawtooths by
    # ~45 ms between reports, so one instant can sit at a peak and understate a real
    # separation (observed: an outlier reading -52 vs median from its latest sample
    # while its whole range, 157-201, lay clear of the fleet's 243-291).
    typical = {k: statistics.median(v) for k, v in have.items()}
    med = statistics.median(typical.values())
    print(f"\nplayout depth vs group median ({med:.0f} ms) -- shallower reads LATE:")
    width = max(len(x) for x in names) + 1
    for k in names:
        if k not in typical:
            continue
        series = have[k]
        d = typical[k] - med
        flag = "  <-- OFFSET?" if abs(d) >= DEPTH_OUTLIER_MS else ""
        print(f"  {k:>{width}s}  {typical[k]:4.0f} ms  {d:+5.0f} vs median   "
              f"(range {min(series)}-{max(series)} over {len(series)}){flag}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="two or more log files; the first is the time base")
    ap.add_argument("--window", type=float, default=2.0, help="max pairing distance, seconds (default 2)")
    ap.add_argument("--follow", action="store_true",
                    help="re-read and redraw the screen every 10 s")
    ap.add_argument("--summary", action="store_true", help="skip per-row output, print only the pair stats")
    args = ap.parse_args()
    if len(args.logs) < 2:
        ap.error("need at least two logs")

    names = [os.path.splitext(os.path.basename(p))[0] for p in args.logs]
    tty = sys.stdout.isatty()
    while True:
        if args.follow and tty:
            # Home, erase screen, erase scrollback: each pass overwrites the last
            # rather than growing the buffer.
            print("\033[H\033[2J\033[3J", end="")
        pair_and_report(names, [parse(p) for p in args.logs], args.window, args.summary,
                        max_rows=rows_that_fit(len(names)) if args.follow and tty else None)
        if not args.follow:
            break
        if tty:
            print(f"\n[{time.strftime('%H:%M:%S')}] refreshing every {REFRESH_S:.0f} s -- Ctrl-C to stop",
                  flush=True)
        else:
            print("\n--- refresh ---", flush=True)
        time.sleep(REFRESH_S)


if __name__ == "__main__":
    main()
