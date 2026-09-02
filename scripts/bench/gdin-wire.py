#!/usr/bin/env python3
"""STAGE 1 GRADER: does the raw pairwise phase delta track the wire?

    gdin-wire.py --log a.log --csv test.csv [--block 300] [--self-test]

PLAN-timing-v2 Stage 1 asks one question and blocks every later stage on it: the group delta the
firmware acts on is ~4x larger than the differential the analyser measures, and nothing downstream
can be graded until that factor is explained. GDIN publishes the pairing INPUTS -- `raw`, the
un-halved pairwise difference before self-inclusion -- so it can be regressed against the wire.

    PASS: raw tracks the rival-clean wire at slope 1.0 +- 0.15 over >= 6 disjoint 5-minute blocks.

WHAT EACH SIDE MEANS, because getting this backwards produces a confident wrong answer:

  raw (us)        peer.phase_us - mine, on the board whose log this is. On board A the peer is
                  B, so raw is "B minus A" -- the same orientation as the wire. On board B it is
                  "A minus B" and must be NEGATED; pass --negate for a b.log.
  offset_ns       i2s-skew's "board B minus board A, positive means B later".

A slope near -1 therefore does not mean the firmware is inverted: it means the two orientations
disagree, which is either the wrong --negate or an A/B assignment in boards.conf that does not
match the analyser clips. The grader says so rather than reporting |slope| and moving on.

METHOD. Theil-Sen, not least squares: a single whole-frame mislock or a reseed drags an OLS
slope bodily and CLAUDE.md has the scars (sd = 30343 us against a median of -18.5 us). The wire
is gated on rival AND pcm_coef for the reason wire-window.py documents -- rival alone misses the
35-36 whole-frame mislocks, and a real 49-197 us differential then reads as 813-3295 us.

Each GDIN sample is paired with the wire by nearest capture within --tol seconds; unmatched
samples are COUNTED AND REPORTED, never silently dropped, because a systematic pairing failure
is exactly the selection effect Stage 1 lists as a candidate cause.

The window must be clean. Capture gaps and reboots are refused up front: the firmware's view and
the wire's view of "the same moment" stop meaning the same thing across a gap.
"""

import argparse
import bisect
import csv
import datetime as dt
import re
import statistics
import sys

GDIN = re.compile(
    r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*?\bGDIN raw=([+-]\d+) gd=([+-]\d+) n=(\d+) "
    r"gap=([+-]\d+) drift=([+-]?\d+\.\d+) extrap=([+-]?\d+\.\d+)(?: t=(\d+))?"
)
STAMP = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]")
MIN_COEF = 0.99          # see wire-window.py
MAX_RIVAL = 0.5
SLOPE_TOL = 0.15         # the plan's pass band
MIN_BLOCKS = 6


def sod(h, m, s, ms=0):
    """Seconds of day. The logs carry no date, so everything is matched within one day and a
    window crossing midnight is refused rather than silently wrapped."""
    return int(h) * 3600 + int(m) * 60 + int(s) + int(ms) / 1000.0


def read_gdin(path, negate=False):
    out = []
    for line in open(path, errors="replace"):
        m = GDIN.search(line)
        if m:
            raw = int(m.group(5))
            out.append((sod(m.group(1), m.group(2), m.group(3), m.group(4)),
                        -raw if negate else raw, int(m.group(6)), int(m.group(7))))
    return out


def capture_breaks(path, gap_s=2.0):
    gaps, reboots, prev = [], [], None
    for line in open(path, errors="replace"):
        m = STAMP.match(line)
        if not m:
            continue
        t = sod(*m.groups())
        if prev is not None and 0 < t - prev > gap_s:
            gaps.append((m.group(0), round(t - prev, 1)))
        prev = t
        if "ESPHome version" in line or "Boot seems" in line:
            reboots.append(m.group(0))
    return gaps, reboots


def read_wire(path):
    """(seconds_of_day, offset_us) for gate-passing rows."""
    rows, dropped = [], 0
    with open(path, errors="replace") as fh:
        head = None
        for line in fh:
            if line.startswith("#"):
                continue
            head = next(csv.reader([line]))
            break
        if not head:
            sys.exit(f"{path}: no header row")
        idx = {n: i for i, n in enumerate(h.strip() for h in head)}
        for need in ("unix_s", "offset_ns", "rival", "pcm_coef"):
            if need not in idx:
                sys.exit(f"{path}: no {need!r} column (have {list(idx)[:8]}...)")
        for r in csv.reader(fh):
            if len(r) <= idx["pcm_coef"]:
                continue
            try:
                u = float(r[idx["unix_s"]]); off = float(r[idx["offset_ns"]])
                riv = float(r[idx["rival"]]); coef = float(r[idx["pcm_coef"]])
            except ValueError:
                continue                      # nan / blank: not a measurement
            if riv > MAX_RIVAL or coef < MIN_COEF:
                dropped += 1
                continue
            lt = dt.datetime.fromtimestamp(u)
            rows.append((sod(lt.hour, lt.minute, lt.second, lt.microsecond // 1000),
                         off / 1000.0))
    rows.sort()
    return rows, dropped


def theil_sen(xs, ys, cap=40000):
    """Median of pairwise slopes. Robust to the whole-frame outliers this bench produces."""
    n = len(xs)
    if n < 2:
        return float("nan")
    slopes = []
    step = max(1, (n * (n - 1) // 2) // cap)
    k = 0
    for i in range(n - 1):
        for j in range(i + 1, n):
            k += 1
            if k % step:
                continue
            dx = xs[j] - xs[i]
            if dx:
                slopes.append((ys[j] - ys[i]) / dx)
    return statistics.median(slopes) if slopes else float("nan")


def pair(gdin, wire, tol):
    """Nearest wire capture within tol seconds. Returns (pairs, unmatched)."""
    times = [w[0] for w in wire]
    pairs, unmatched = [], 0
    for t, raw, gd, n in gdin:
        i = bisect.bisect_left(times, t)
        best, bestd = None, tol
        for j in (i - 1, i, i + 1):
            if 0 <= j < len(wire):
                d = abs(wire[j][0] - t)
                if d <= bestd:
                    best, bestd = wire[j], d
        if best is None:
            unmatched += 1
        else:
            pairs.append((t, raw, best[1], gd, n))
    return pairs, unmatched


def grade(pairs, block_s, min_n):
    """Per-block Theil-Sen slope of wire against raw."""
    if not pairs:
        return []
    t0 = pairs[0][0]
    blocks = {}
    for t, raw, wire_us, gd, n in pairs:
        blocks.setdefault(int((t - t0) // block_s), []).append((raw, wire_us))
    out = []
    for b in sorted(blocks):
        pts = blocks[b]
        if len(pts) < min_n:
            out.append((b, len(pts), float("nan"), float("nan")))
            continue
        xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
        slope = theil_sen(xs, ys)
        resid = statistics.median([abs(y - slope * x) for x, y in zip(xs, ys)])
        out.append((b, len(pts), slope, resid))
    return out


def self_test():
    """The grader must recover a slope it was given, and must NOT be fooled by outliers."""
    import random
    random.seed(7)
    wire, gdin = [], []
    for i in range(1800):                      # 30 min at 1 Hz
        t = 3600.0 + i
        true_raw = 200.0 * ((i % 300) / 300.0 - 0.5)      # a slow sweep, not a constant
        gdin.append((t, true_raw, true_raw / 2, 2))
        wire.append((t + 0.05, true_raw * 1.0))           # slope exactly 1
    pairs, un = pair(gdin, wire, 0.5)
    assert un == 0, un
    rows = grade(pairs, 300, 20)
    slopes = [r[2] for r in rows if r[1] >= 20]
    assert len(slopes) >= 6, len(slopes)
    assert all(abs(s - 1.0) < 0.01 for s in slopes), slopes
    print(f"  slope-1 synthetic: {len(slopes)} blocks, slopes {min(slopes):.3f}..{max(slopes):.3f}")

    # 4x, the failure Stage 1 exists to detect
    wire4 = [(t, v / 4.0) for t, v in wire]
    rows4 = grade(pair(gdin, wire4, 0.5)[0], 300, 20)
    s4 = [r[2] for r in rows4 if r[1] >= 20]
    assert all(abs(s - 0.25) < 0.01 for s in s4), s4
    print(f"  4x-disagreement synthetic: slopes {min(s4):.3f}..{max(s4):.3f} (detected, not averaged away)")

    # whole-frame mislocks on 10 % of rows must not move the verdict
    dirty = [(t, v + (22680.0 if random.random() < 0.10 else 0.0)) for t, v in wire]
    rowsd = grade(pair(gdin, dirty, 0.5)[0], 300, 20)
    sd = [r[2] for r in rowsd if r[1] >= 20]
    assert all(abs(s - 1.0) < 0.05 for s in sd), sd
    print(f"  10% whole-frame outliers: slopes {min(sd):.3f}..{max(sd):.3f} (Theil-Sen holds)")

    # an inverted orientation must be reported as such, not as |slope|
    inv = [(t, -v) for t, v in wire]
    si = [r[2] for r in grade(pair(gdin, inv, 0.5)[0], 300, 20) if r[1] >= 20]
    assert all(abs(s + 1.0) < 0.01 for s in si), si
    print(f"  inverted orientation: slopes {min(si):.3f}..{max(si):.3f} (sign preserved)")
    print("self-test OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", help="a.log (or b.log with --negate)")
    ap.add_argument("--csv", default="test.csv")
    ap.add_argument("--negate", action="store_true",
                    help="the log is board B's: its raw is A-B, the wire is B-A")
    ap.add_argument("--block", type=float, default=300.0, help="block seconds (plan: 300)")
    ap.add_argument("--tol", type=float, default=0.5, help="max pairing distance, seconds")
    ap.add_argument("--min-n", type=int, default=20, help="minimum pairs for a block to count")
    ap.add_argument("--allow-dirty", action="store_true",
                    help="grade even with capture gaps or reboots in the window")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return 0
    if not args.log:
        ap.error("--log is required unless --self-test is given")

    gaps, reboots = capture_breaks(args.log)
    if gaps or reboots:
        print(f"{args.log}: {len(gaps)} capture gap(s)"
              + (f" totalling {sum(g[1] for g in gaps):.0f}s" if gaps else "")
              + f", {len(reboots)} reboot(s)")
        for g in gaps[:5]:
            print(f"   gap {g[1]:.0f}s ending {g[0]}")
        for r in reboots[:5]:
            print(f"   reboot at {r}")
        if not args.allow_dirty:
            print("REFUSING: across a gap the firmware's clock and the wire's stop describing the "
                  "same moment.\n  Slice a clean span (grep -abo for byte offsets) or pass "
                  "--allow-dirty and discount the result.")
            return 2

    gdin = read_gdin(args.log, args.negate)
    if not gdin:
        print(f"{args.log}: no GDIN lines. Emitted at DEBUG since 2026-09-02; a board with no "
              f"render phase of its own (the observer) emits none by design.")
        return 2
    wire, dropped = read_wire(args.csv)
    if not wire:
        print(f"{args.csv}: no rows passed the gate (rival <= {MAX_RIVAL}, pcm_coef >= {MIN_COEF})")
        return 2

    pairs, unmatched = pair(gdin, wire, args.tol)
    print(f"{args.log} vs {args.csv}")
    print(f"  GDIN {len(gdin)} samples, wire {len(wire)} gated rows ({dropped} dropped by the gate)")
    print(f"  paired {len(pairs)} within {args.tol}s; {unmatched} GDIN samples unmatched"
          + (" -- a systematic pairing failure is itself a Stage 1 candidate" if unmatched > len(gdin) * 0.2 else ""))
    if not pairs:
        return 2

    rows = grade(pairs, args.block, args.min_n)
    print(f"\n  {'block':>5} {'n':>6} {'slope':>8} {'|resid| med':>12}")
    good = []
    for b, n, slope, resid in rows:
        mark = ""
        if n >= args.min_n:
            mark = "  <-- outside 1.0 +-{:.2f}".format(SLOPE_TOL) if abs(slope - 1.0) > SLOPE_TOL else ""
            good.append(slope)
        print(f"  {b:>5} {n:>6} {slope:>8.3f} {resid:>12.1f}{mark}")

    print()
    if len(good) < MIN_BLOCKS:
        print(f"INCONCLUSIVE: {len(good)} usable blocks, the plan wants >= {MIN_BLOCKS} "
              f"disjoint {args.block:.0f}s blocks. Longer clean window needed.")
        return 1
    med = statistics.median(good)
    inband = [s for s in good if abs(s - 1.0) <= SLOPE_TOL]
    print(f"median slope {med:+.3f} over {len(good)} blocks; "
          f"{len(inband)}/{len(good)} inside 1.0 +-{SLOPE_TOL}")
    if med < 0:
        print("ORIENTATION MISMATCH, not a firmware result: raw and the wire disagree in SIGN.\n"
              "  Either --negate is wrong for this log, or boards.conf's A/B does not match which\n"
              "  board carries the analyser's _ONE clips. Fix that before reading the magnitude.")
        return 1
    if len(inband) == len(good):
        print(f"PASS: raw tracks the wire at slope 1.0 +-{SLOPE_TOL} across every block.")
        return 0
    print(f"FAIL: {len(good) - len(inband)} block(s) outside the band. "
          f"A slope of ~0.25 is the 4x the stage is looking for; ~0.5 is a stray halving.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
