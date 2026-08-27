#!/usr/bin/env python3
"""Systematic regression of the measured skew against every recorded quantity, plus an event
study over the device logs.

WHY THIS EXISTS. Every correlation run against this data so far tested ONE hypothesis at a
time -- accounting split, render phase, anchor-at-unmute -- and each time the answer arrived
with a plausible story attached. That is a good way to find the thing you went looking for and
miss the thing that is actually there. This tests everything at once and ranks by effect size,
so a relationship has to survive being compared against all its rivals.

TWO THINGS IT DOES DIFFERENTLY, both because of mistakes made on this data:

  * It reports correlations on FIRST DIFFERENCES as well as levels. Both the skew and most of
    the firmware's series drift over an afternoon, and two drifting series correlate strongly
    while sharing no mechanism at all. The differenced correlation asks whether they MOVE
    together, which is the question that matters.

  * Log events are placed by FILE POSITION, never by timestamp. The logs span several days and
    their lines carry no date, so a timestamp match silently returns a line from a previous day
    and a previous build. That produced a confident wrong answer twice in one afternoon.

    python3 scripts/skew-regress.py                 # everything in test.csv
    python3 scripts/skew-regress.py --minutes 60    # just the last hour
"""

import argparse
import bisect
import datetime
import math
import re
import statistics

CSV = "test.csv"
TS = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\]")

# Log events worth attributing a skew change to. Kept coarse on purpose: a category with two
# occurrences tells you nothing, and splitting hairs here just produces more of those.
EVENTS = (
    ("lock",           re.compile(r"Sync locked")),
    ("lock/waited",    re.compile(r"anchor still reads")),
    ("mute",           re.compile(r"Muting: hard resync")),
    ("resync/unmuted", re.compile(r"correcting audibly, staying unmuted")),
    ("starvation",     re.compile(r"Injected starvation window ended")),
    ("splitinject",    re.compile(r"SPLITINJECT")),
    ("reanchor",       re.compile(r"Re-anchoring after re-lock")),
    ("stall",          re.compile(r"PLAYER STALLED")),
    ("clampdbg",       re.compile(r"CLAMPDBG")),
)


def pearson(xs, ys):
    n = len(xs)
    if n < 8:
        return float("nan")
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0 or syy <= 0:
        return float("nan")
    return sxy / math.sqrt(sxx * syy)


def load_csv(minutes):
    head = open(CSV).readline()
    names = None
    rows = []
    for line in open(CSV):
        if line.startswith("#"):
            continue
        f = line.rstrip("\n").split(",")
        if names is None:
            if f[0] == "elapsed_s":
                names = f
            continue
        if len(f) < len(names) - 1:
            continue
        try:
            unix = float(f[1])
            off = float(f[2])
        except ValueError:
            continue
        if not math.isfinite(off):
            continue
        rows.append((unix, f))
    if not rows:
        return names, []
    if minutes:
        cutoff = rows[-1][0] - minutes * 60
        rows = [r for r in rows if r[0] >= cutoff]
    return names, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=0, help="only the last N minutes")
    ap.add_argument("--logs", nargs="*", default=["a.log", "b.log"])
    ap.add_argument("--tail", type=int, default=600000, help="log lines to consider (file position)")
    args = ap.parse_args()

    names, rows = load_csv(args.minutes)
    if not rows:
        print("no valid rows")
        return
    span = (rows[-1][0] - rows[0][0]) / 60.0
    print(f"{len(rows)} valid rows over {span:.1f} min\n")

    # ---- correlations, levels and first differences -------------------------------------
    off = [float(r[1][2]) / 1000.0 for r in rows]
    doff = [off[i] - off[i - 1] for i in range(1, len(off))]
    print(f"skew: median {statistics.median(off):+.1f} us  sd {statistics.pstdev(off):.1f}\n")
    print(f"{'column':>16} {'r(level)':>10} {'r(diff)':>9} {'sd':>12}  note")
    results = []
    for idx, name in enumerate(names):
        if idx < 2 or name in ("offset_ns", "reason"):
            continue
        try:
            col = [float(r[1][idx]) for r in rows]
        except (ValueError, IndexError):
            continue
        if not all(math.isfinite(c) for c in col):
            continue
        dcol = [col[i] - col[i - 1] for i in range(1, len(col))]
        rl, rd = pearson(col, off), pearson(dcol, doff)
        results.append((abs(rd) if math.isfinite(rd) else 0, name, rl, rd, statistics.pstdev(col)))
    for _, name, rl, rd, sd in sorted(results, reverse=True):
        note = ""
        if math.isfinite(rd) and abs(rd) > 0.3:
            note = "<-- moves WITH the skew"
        elif math.isfinite(rl) and abs(rl) > 0.7 and (not math.isfinite(rd) or abs(rd) < 0.1):
            note = "level-only: shared drift, not a mechanism"
        print(f"{name:>16} {rl:+10.3f} {rd:+9.3f} {sd:12.3f}  {note}")

    # ---- event study --------------------------------------------------------------------
    ts = [r[0] for r in rows]
    base = datetime.datetime.fromtimestamp(rows[-1][0])
    day0 = base.replace(hour=0, minute=0, second=0, microsecond=0).timestamp()

    def med(lo, hi):
        i, j = bisect.bisect_left(ts, lo), bisect.bisect_right(ts, hi)
        v = off[i:j]
        return (statistics.median(v), len(v)) if len(v) > 40 else (float("nan"), len(v))

    # PLACEBO CONTROL. Without it this table is worthless: the skew trended down across the
    # whole window, so EVERY event category -- including events on the undisturbed board --
    # showed a negative shift, and each one looked like a finding. An event has to beat the
    # change seen at randomly chosen times over the same span before it means anything.
    import random
    rng = random.Random(12345)
    placebo = []
    for _ in range(400):
        t = rng.uniform(ts[0] + 200, ts[-1] - 220)
        a, _n1 = med(t - 120, t - 20)
        b, _n2 = med(t + 60, t + 200)
        if math.isfinite(a) and math.isfinite(b):
            placebo.append(b - a)
    pl_med = statistics.median(placebo) if placebo else float("nan")
    pl_sd = statistics.pstdev(placebo) if len(placebo) > 1 else float("nan")
    print(f"\nplacebo (random times, n={len(placebo)}): median {pl_med:+.1f} us, sd {pl_sd:.1f}"
          f"  <-- the background trend every event below is sitting on")

    print(f"\n{'event':>16} {'log':>6} {'n':>4} {'median dskew':>13} {'vs placebo':>11} {'z':>6}")
    for label, pat in EVENTS:
        for path in args.logs:
            hits = []
            try:
                lines = open(path, errors="replace").read().splitlines()[-args.tail:]
            except OSError:
                continue
            for l in lines:
                if not pat.search(l):
                    continue
                m = TS.match(l)
                if not m:
                    continue
                sod = (int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
                       + int(m.group(4)) / 1000.0)
                hits.append(day0 + sod)
            deltas = []
            for t in hits:
                a, na = med(t - 120, t - 20)
                b, nb = med(t + 60, t + 200)
                if math.isfinite(a) and math.isfinite(b):
                    deltas.append(b - a)
            if deltas:
                m = statistics.median(deltas)
                excess = m - pl_med
                # How many placebo-sds the excess is, scaled for the sample size. Crude, but it
                # is the difference between "this event does something" and "this event happened
                # while the skew was drifting anyway".
                z = (excess / (pl_sd / math.sqrt(len(deltas)))) if pl_sd > 0 else float("nan")
                flag = "  <--" if abs(z) > 2 else ""
                print(f"{label:>16} {path[0]:>6} {len(deltas):4d} "
                      f"{m:+13.1f} {excess:+11.1f} {z:+6.1f}{flag}")


if __name__ == "__main__":
    main()
