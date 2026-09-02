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
    r"gap=([+-]\d+) drift=([+-]?\d+\.\d+) extrap=([+-]?\d+\.\d+)"
    # steady= postdates the first GDIN builds and is optional, like t=. Absent means "not
    # reported", NOT "steady": a log without it is graded ungated and says so.
    r"(?: steady=([01]))?(?: t=(\d+))?"
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
    """(sec, raw, gd, n, steady) -- steady is True/False, or None when the build predates it."""
    out = []
    for line in open(path, errors="replace"):
        m = GDIN.search(line)
        if m:
            raw = int(m.group(5))
            st = m.group(11)
            out.append((sod(m.group(1), m.group(2), m.group(3), m.group(4)),
                        -raw if negate else raw, int(m.group(6)), int(m.group(7)),
                        None if st is None else st == "1"))
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


def ols(xs, ys):
    """Least-squares slope of y on x."""
    n = len(xs)
    if n < 2:
        return float("nan")
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else float("nan")


def bracket(xs, ys):
    """(low, high) bounds on the slope under errors-in-variables, plus Pearson r.

    THE NOISE IS IN x, NOT IN y: `raw` disagrees with the wire by rare large excursions, while
    the wire is gated to a clean lock. Every single-estimator slope is therefore biased, and the
    direction depends on which variable you regress on:

        OLS(y|x)      attenuated toward 0 by the noise in x  -> LOWER bound
        1/OLS(x|y)    inflated by the noise in y             -> UPPER bound
        Theil-Sen     somewhere between, by no fixed rule

    Reporting one number is how this grader printed FAIL (Theil-Sen 0.93, 4/6 blocks out of band)
    on data whose slope is 1.0: the bracket was 0.58 .. 1.04, and the upper bound sat on 1.0 in
    every block on both boards, which is the signature of y = x + noise. Quote the bracket.
    """
    n = len(xs)
    if n < 3:
        return float("nan"), float("nan"), float("nan")
    fwd = ols(xs, ys)
    rev = ols(ys, xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    sx = (sum((x - mx) ** 2 for x in xs) / n) ** 0.5
    sy = (sum((y - my) ** 2 for y in ys) / n) ** 0.5
    r = (sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / n / (sx * sy)) if sx and sy else float("nan")
    hi = (1.0 / rev) if rev else float("nan")
    lo = fwd
    if lo == lo and hi == hi and lo > hi:      # both finite and inverted (negative slopes)
        lo, hi = hi, lo
    return lo, hi, r


def theil_sen(xs, ys, cap=40000):
    """Median of pairwise slopes. Robust to the whole-frame outliers this bench produces.

    Kept as a third view, NOT as the verdict: it is biased under errors-in-variables and this
    bench is exactly that case. See bracket().
    """
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
    for t, raw, gd, n, _steady in gdin:
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


def hms(t):
    return f"{int(t // 3600) % 24:02d}:{int(t % 3600 // 60):02d}:{int(t % 60):02d}"


def grade(pairs, block_s, min_n):
    """Per-block Theil-Sen slope of wire against raw.

    Blocks are anchored on the first PAIRED sample, not the first GDIN line -- with the wire
    absent for a stretch (a wedged analyser) those differ by however long the outage lasted, and
    a block index read against the wrong anchor attributes events to the wrong block. Hence the
    wall-clock column: the mapping is printed, never inferred.
    """
    if not pairs:
        return []
    t0 = pairs[0][0]
    blocks = {}
    for t, raw, wire_us, gd, n in pairs:
        blocks.setdefault(int((t - t0) // block_s), []).append((raw, wire_us))
    out = []
    for b in sorted(blocks):
        pts = blocks[b]
        span = (hms(t0 + b * block_s), hms(t0 + (b + 1) * block_s))
        if len(pts) < min_n:
            out.append((b, len(pts), float("nan"), float("nan"), span))
            continue
        xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
        lo, hi, r = bracket(xs, ys)
        ts = theil_sen(xs, ys)
        # Residual about the bracket's midpoint, in MAD not sd: this bench's disagreement is rare
        # large excursions, and sd overstates it by ~8x.
        mid = (lo + hi) / 2 if lo == lo and hi == hi else ts
        res = [y - mid * x for x, y in zip(xs, ys)]
        rmed = statistics.median(res)
        resid = statistics.median([abs(v - rmed) for v in res])
        out.append((b, len(pts), lo, hi, r, ts, resid, span))
    return out


def verdict(lo, hi, tol=SLOPE_TOL, uninformative=1.0):
    """What a block's bracket actually licenses you to say."""
    if lo != lo or hi != hi:
        return "no data"
    if hi - lo > uninformative:
        return "uninformative"          # too little leverage to constrain the slope at all
    if hi < 1.0 - tol or lo > 1.0 + tol:
        return "EXCLUDES 1.0"           # a real disagreement, not an estimator artefact
    return "ok"


def self_test():
    """The grader must recover a slope it was given, AND must not be fooled by the shape of this
    bench's real error.

    THE ORIGINAL SELF-TEST COULD NOT FAIL. It built the wire as an exact function of `raw`, i.e.
    with no noise in x -- the one case where every estimator agrees and the errors-in-variables
    bias vanishes. It passed while the grader printed FAIL on real data whose slope is 1.0. The
    x-noise cases below are the ones that matter; keep them.
    """
    import random
    random.seed(7)

    def synth(k=1.0, xnoise=0.0, ynoise=0.0, outlier=0.0, n=1800, amp=200.0):
        wire, gdin = [], []
        for i in range(n):
            t = 3600.0 + i
            truth = amp * ((i % 300) / 300.0 - 0.5)          # a sweep, not a constant
            raw = truth + random.gauss(0, xnoise)
            w = k * truth + random.gauss(0, ynoise)
            if outlier and random.random() < outlier:
                w += 22680.0                                  # one whole frame
            gdin.append((t, raw, raw / 2, 2, True))
            wire.append((t + 0.05, w))
        return gdin, wire

    def run(**kw):
        gdin, wire = synth(**kw)
        pairs, un = pair(gdin, wire, 0.5)
        assert un == 0, un
        rows = grade(pairs, 300, 20)
        assert all(len(r) == 8 for r in rows), "grade() rows carry bracket, r, T-S, resid, span"
        return [(r[2], r[3]) for r in rows if r[1] >= 20]

    b = run()
    assert len(b) >= 6 and all(lo <= 1.0 <= hi for lo, hi in b), b
    print(f"  clean slope 1: {len(b)} blocks, every bracket contains 1.0")

    # THE CASE THE OLD TEST MISSED: noise in x only. Single estimators are biased low here;
    # the bracket must still contain the truth.
    b = run(xnoise=55.0)
    # The bracket bounds the errors-in-variables BIAS; it is not a confidence interval, so with
    # ~300 samples a bound can land a few percent the wrong side of the truth. What must hold is
    # the operational property: the forward estimator is visibly attenuated, the reverse bound
    # sits near 1.0, and no block is called a failure.
    assert all(lo < 0.95 for lo, hi in b), f"forward estimator should be attenuated: {b}"
    assert all(hi > 0.85 for lo, hi in b), f"reverse bound should sit near 1.0: {b}"
    assert all(verdict(lo, hi) == "ok" for lo, hi in b), [verdict(*x) for x in b]
    print(f"  noise in x (55 us, the real shape): brackets {min(l for l,_ in b):.2f}.."
          f"{max(h for _,h in b):.2f}, all read 'ok' (single estimators would say FAIL)")

    # A REAL 4x must still be caught, and must not be excused as noise.
    b = run(k=0.25, xnoise=20.0)
    assert all(verdict(lo, hi) == "EXCLUDES 1.0" for lo, hi in b), [verdict(*x) for x in b]
    print(f"  true 4x with x-noise: every block EXCLUDES 1.0 (not averaged away)")

    # Whole-frame mislocks on 10 % of rows must not move the verdict.
    b = run(outlier=0.10)
    assert all(verdict(lo, hi) in ("ok", "uninformative") for lo, hi in b), [verdict(*x) for x in b]
    print(f"  10% whole-frame outliers: no block falsely excludes 1.0")

    # No leverage: a block where the differential barely moves must say so, not guess.
    b = run(amp=4.0, xnoise=20.0)
    assert all(verdict(lo, hi) in ("uninformative", "EXCLUDES 1.0") for lo, hi in b), b
    unin = sum(1 for lo, hi in b if verdict(lo, hi) == "uninformative")
    assert unin >= 4, f"expected most no-leverage blocks to be called uninformative, got {unin}"
    print(f"  no leverage (4 us sweep): {unin}/{len(b)} blocks reported uninformative")

    # Orientation: an inverted wire must read as inverted, not as |slope|.
    gdin, wire = synth()
    inv = [(t, -v) for t, v in wire]
    b = [(r[2], r[3]) for r in grade(pair(gdin, inv, 0.5)[0], 300, 20) if r[1] >= 20]
    assert all(hi < 0 for lo, hi in b), b
    print(f"  inverted orientation: every bracket is negative (sign preserved)")
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
    ap.add_argument("--include-transient", action="store_true",
                    help="grade samples the board itself flags as non-steady (steady=0). Off by "
                         "default: while its resync window is open a board keeps measuring its "
                         "phase locally although its audio is being stepped, so those samples "
                         "cannot agree with the wire and grading them measures the transient.")
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
    # STEADY GATE. A board in transient stops beaconing but keeps using its phase locally, by
    # design (snapcast_client.cpp:4468) -- so these samples are computed from a phase whose audio
    # is being stepped. Absent flag means "not reported", never "steady": an older log is graded
    # ungated and told so, rather than silently dropping every sample or silently keeping them.
    flagged = [g for g in gdin if g[4] is not None]
    n_trans = sum(1 for g in flagged if g[4] is False)
    if not flagged:
        print(f"  NOTE: no steady= field in this log (build predates it) -- grading UNGATED. "
              f"Read the verdict together with the window's hard-resync count.")
    elif args.include_transient:
        print(f"  including {n_trans} transient sample(s) on request (--include-transient)")
    else:
        gdin = [g for g in gdin if g[4] is not False]
        print(f"  steady gate: {n_trans} of {len(flagged)} samples dropped as non-steady "
              f"({100.0 * n_trans / len(flagged):.1f}%)")
        if not gdin:
            print("  nothing left after the steady gate: the board was in transient for the "
                  "whole window, which is itself the finding.")
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
    print(f"\n  {'block':>5} {'window':>19} {'n':>6} {'slope bracket':>16} {'r':>6} "
          f"{'T-S':>6} {'MAD':>7}  verdict")
    good, verdicts = [], []
    for b, n, lo, hi, r, ts, resid, span in rows:
        v = verdict(lo, hi) if n >= args.min_n else "thin"
        if n >= args.min_n:
            good.append((lo, hi)); verdicts.append(v)
        print(f"  {b:>5} {span[0]}..{span[1]} {n:>6} {lo:>7.3f}..{hi:<8.3f} {r:>6.3f} "
              f"{ts:>6.3f} {resid:>7.1f}  {v}")

    print()
    if len(good) < MIN_BLOCKS:
        print(f"INCONCLUSIVE: {len(good)} usable blocks, the plan wants >= {MIN_BLOCKS} "
              f"disjoint {args.block:.0f}s blocks. Longer clean window needed.")
        return 1

    # MAGNITUDE, independent of any regression. Ratio of medians needs no model, no leverage and
    # no assumption about where the noise lives -- if raw were 4x the wire this alone would say so.
    raws = [abs(p[1]) for p in pairs]
    wires = [abs(p[2]) for p in pairs]
    mw = statistics.median(wires)
    ratio = statistics.median(raws) / mw if mw else float("nan")
    resid = [p[1] - p[2] for p in pairs]
    rmed = statistics.median(resid)
    rmad = statistics.median([abs(v - rmed) for v in resid])
    print(f"magnitude: median |raw| / median |wire| = {ratio:.2f} "
          f"({statistics.median(raws):.1f} / {mw:.1f} us)")
    print(f"residual raw-wire: median {rmed:+.1f}  MAD {rmad:.1f}  sd "
          f"{statistics.pstdev(resid):.1f} us   (MAD is the honest one; sd here is tail-driven)")

    if all(hi < 0 for lo, hi in good):
        print("\nORIENTATION MISMATCH, not a firmware result: raw and the wire disagree in SIGN.\n"
              "  Either --negate is wrong for this log, or boards.conf's A/B does not match which\n"
              "  board carries the analyser's _ONE clips. Fix that before reading the magnitude.")
        return 1
    excl = [v for v in verdicts if v == "EXCLUDES 1.0"]
    unin = [v for v in verdicts if v == "uninformative"]
    print(f"\n{len(good)} blocks: {len(verdicts) - len(excl) - len(unin)} consistent with 1.0, "
          f"{len(excl)} excluding it, {len(unin)} uninformative")
    if excl:
        print(f"FAIL: {len(excl)} block(s) EXCLUDE slope 1.0 -- a real disagreement, not an "
              f"estimator artefact.\n  ~0.25 would be the 4x; ~0.5 a stray halving.")
        return 1
    if len(verdicts) - len(unin) < MIN_BLOCKS:
        print(f"INCONCLUSIVE: only {len(verdicts) - len(unin)} blocks carry enough leverage to "
              f"constrain the slope.\n  A block where the differential barely moves cannot grade "
              f"a slope, however many samples it has.")
        return 1
    print(f"PASS: every block is consistent with raw tracking the wire at 1.0 +-{SLOPE_TOL}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
