#!/usr/bin/env python3
"""Grade GDIN raw against the analyser's wire: the Stage 1 pass condition, slope 1.0 +-0.15.

WHAT IS BEING COMPARED. GDIN raw is the UN-HALVED pairwise phase difference the group delta is
built from, before self-inclusion -- the same physical quantity, in the same units, that the
analyser measures on the wire. gd (halved, mean-relative) is NOT comparable to the wire and is
never used here; tsf_sync.cpp logs both side by side precisely so they cannot be confused.

SIGN. test.csv is "board B minus board A, positive means B later". On board a the peer is b, so
raw = phase_b - phase_a carries the wire's sign; on board b it is negated. Rather than assume the
phase field's polarity, BOTH boards are fit and reported: if b's slope comes back near -1 where
a's is near +1, the convention is confirmed by the data instead of by my reading of it.

PAIRING. Each GDIN sample is matched to the nearest rival-gated wire row within --tol seconds.
A GDIN line is ~1/s and captures are ~30/s, so the match is dense; rows outside the tolerance are
dropped and counted rather than stretched to fit.

DATES. Logs carry HH:MM:SS with no date and span days, so a window is given as a byte offset into
the log (--a-off/--b-off), never as a timestamp grep. Wall-clock times inside that tail are
resolved against --date (default: today), and a backwards jump is treated as the next day.
"""
import argparse
import math
import re
import statistics as st
import subprocess
import time

GDIN = re.compile(r"GDIN (.*)$")
FIELD = re.compile(r"(\w+)=([-+]?[0-9]+\.?[0-9]*)")
STAMP = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def gdin_samples(path, off, day_epoch):
    """[(unix_s, raw_us, gap_us, gd_us)] from a byte offset, with day rollover handled."""
    raw = subprocess.run(["tail", "-c", f"+{off}", path], capture_output=True).stdout
    text = ANSI.sub("", raw.decode("utf-8", "replace").replace("\x00", ""))
    out, prev, day = [], -1.0, day_epoch
    for line in text.splitlines():
        s = STAMP.match(line)
        if not s:
            continue
        secs = (int(s.group(1)) * 3600 + int(s.group(2)) * 60 + int(s.group(3))
                + int(s.group(4)) / 1000.0)
        if secs < prev - 3600:      # went backwards by an hour or more: next day
            day += 86400.0
        prev = secs
        m = GDIN.search(line)
        if not m:
            continue
        d = dict(FIELD.findall(m.group(1)))
        try:
            out.append((day + secs, float(d["raw"]), float(d["gap"]), float(d["gd"])))
        except (KeyError, ValueError):
            continue
    return out


def wire_rows(path, rival_max):
    hdr, out = None, []
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("#"):
            continue
        if hdr is None:
            hdr = line.split(",")
            idx = {k: n for n, k in enumerate(hdr)}
            continue
        r = line.split(",")

        def f(k):
            try:
                v = float(r[idx[k]])
                return None if math.isnan(v) else v
            except Exception:
                return None

        u, o, rv = f("unix_s"), f("offset_ns"), f("rival")
        if u is None or o is None or rv is None or rv >= rival_max:
            continue
        out.append((u, o / 1000.0))
    out.sort()
    return out


def theil_sen(xs, ys, cap=400):
    """Median of pairwise slopes -- immune to leverage points, which least squares is not.

    The first version of this grader used ONLY least squares and reported slope=+0.005 alongside
    r=+0.915, which cannot both be true: that combination requires sd(x) ~ 180x sd(y), i.e. a
    handful of huge GDIN raw values (resyncs and excursions) far outside the p10..p90 the same
    output printed as "comparable". Subsampled to `cap` points for O(cap^2) pairs.
    """
    import random
    n = len(xs)
    idx = list(range(n))
    if n > cap:
        random.seed(0)                  # deterministic: this is a grader, not a simulation
        idx = sorted(random.sample(idx, cap))
    slopes = []
    for ii in range(len(idx)):
        for jj in range(ii + 1, len(idx)):
            dx = xs[idx[jj]] - xs[idx[ii]]
            if abs(dx) < 1e-9:
                continue
            slopes.append((ys[idx[jj]] - ys[idx[ii]]) / dx)
    if not slopes:
        return None
    return st.median(slopes)


def fit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    syy = sum((y - my) ** 2 for y in ys)
    if sxx == 0 or syy == 0:
        return None
    slope = sxy / sxx
    inter = my - slope * mx
    r = sxy / math.sqrt(sxx * syy)
    resid = [y - (slope * x + inter) for x, y in zip(xs, ys)]
    return slope, inter, r, st.pstdev(resid), st.median(resid)


ap = argparse.ArgumentParser()
ap.add_argument("--bench", default="/home/william/Documents/git/esphome-snapcast")
ap.add_argument("--a-off", type=int, required=True)
ap.add_argument("--b-off", type=int, required=True)
ap.add_argument("--tol", type=float, default=0.5)
ap.add_argument("--rival-max", type=float, default=0.3)
ap.add_argument("--date", default=None, help="YYYY-MM-DD the tail starts on (default today)")
a = ap.parse_args()

if a.date:
    day = time.mktime(time.strptime(a.date, "%Y-%m-%d"))
else:
    t = time.localtime()
    day = time.mktime((t.tm_year, t.tm_mon, t.tm_mday, 0, 0, 0, 0, 0, -1))

wire = wire_rows(f"{a.bench}/test.csv", a.rival_max)
print(f"  wire: {len(wire)} rival-gated rows (<{a.rival_max})")
if len(wire) < 100:
    raise SystemExit("  not enough wire rows to grade against")
wt = [w[0] for w in wire]

for name, path, off, expect in (("a", "a.log", a.a_off, +1.0), ("b", "b.log", a.b_off, -1.0)):
    g = gdin_samples(f"{a.bench}/{path}", off, day)
    xs, ys, dropped = [], [], 0
    import bisect
    for ts, raw, gap, gd in g:
        i = bisect.bisect_left(wt, ts)
        best, bd = None, None
        for j in (i - 1, i):
            if 0 <= j < len(wire):
                dt = abs(wire[j][0] - ts)
                if bd is None or dt < bd:
                    bd, best = dt, wire[j]
        if best is None or bd > a.tol:
            dropped += 1
            continue
        xs.append(raw)
        ys.append(best[1])
    print(f"\n  board {name}: GDIN n={len(g)}  matched={len(xs)}  dropped(>±{a.tol}s)={dropped}")
    if len(xs) < 50:
        print("    too few matched samples to fit")
        continue
    res = fit(xs, ys)
    if res is None:
        print("    degenerate fit (no variance)")
        continue
    slope, inter, r, rsd, rmed = res
    ts_slope = theil_sen(xs, ys)
    xq = st.quantiles(xs, n=100)
    print(f"    least squares: slope={slope:+.3f} intercept={inter:+.1f} us r={r:+.3f} "
          f"resid sd={rsd:.1f} med={rmed:+.1f} us")
    print(f"    GDIN raw x: sd={st.pstdev(xs):9.1f}  p1={xq[0]:+.0f} p99={xq[97]:+.0f} "
          f"min={min(xs):+.0f} max={max(xs):+.0f} us")
    print(f"    wire     y: sd={st.pstdev(ys):9.1f}  p10..p90={st.quantiles(ys,n=10)[0]:+.0f}.."
          f"{st.quantiles(ys,n=10)[8]:+.0f} us")
    if st.pstdev(xs) > 3 * st.pstdev(ys):
        print(f"    -> x carries {st.pstdev(xs)/max(st.pstdev(ys),1e-9):.0f}x the spread of y: "
              f"least squares is leverage-dragged, read Theil-Sen")
    # Robust: median of pairwise slopes, plus a least-squares refit on the p1..p99 core of x.
    core = [(x, y) for x, y in zip(xs, ys) if xq[0] <= x <= xq[97]]
    if ts_slope is not None:
        v = "PASS" if abs(abs(ts_slope) - 1.0) <= 0.15 else "FAIL"
        print(f"    THEIL-SEN slope={ts_slope:+.3f} (expect ~{expect:+.1f})  |1.0|±0.15 -> {v}")
    if len(core) > 50:
        cres = fit([c[0] for c in core], [c[1] for c in core])
        if cres:
            cs, ci, cr, crsd, _ = cres
            v = "PASS" if abs(abs(cs) - 1.0) <= 0.15 else "FAIL"
            print(f"    p1..p99 core (n={len(core)}): slope={cs:+.3f} r={cr:+.3f} "
                  f"resid sd={crsd:.1f} us  -> {v}")
