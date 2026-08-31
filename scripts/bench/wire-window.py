#!/usr/bin/env python3
"""Grade a wall-clock span of the i2s-skew CSV (the wire truth) the same way dl-window.py grades
the device logs.

    wire-window.py [--csv test.csv] --from HH:MM:SS --to HH:MM:SS [--max-rival 0.5]

Reads the named header row, selects rows whose unix_s falls in [from, to] on TODAY's date, drops
rows failing the rival gate (CLAUDE.md: MLS44 gives ~0.03; a run at 0.94 is whole-frame errors
masquerading as findings), and prints n, mean, median, MAD, sd, p2p of the skew column plus
how many rows the gate removed. Units are taken from the column name.
"""
import argparse
import csv
import datetime as dt
import statistics

# WHY pcm_coef IS GATED ALONGSIDE rival (measured 2026-08-31, PLAN-sub-microsecond).
# rival alone is NOT sufficient. During the ms-class reference steps the analyser's correlator
# locks 35-36 WHOLE FRAMES away and offset_ns reports it; rival stays at 0.03-0.07 on many such
# rows -- it does not catch this -- while pcm_coef falls to 0.46-0.95 against 0.999-1.000 when the
# lock is good. Ungated, a real 49-197 us differential reads as 813-3295 us, and a 9-33x
# "amplification mechanism" was briefly built on those rows before being retracted. Only 3.36 % of
# rows fall below 0.99, so the gate is nearly free. The tell was arithmetic: the excursions were
# exactly 794 and 816 us, i.e. 35 and 36 x 22.68 us.
MIN_COEF = 0.99


def hms_today(s: str) -> float:
    h, m, sec = (int(x) for x in s.split(":"))
    now = dt.datetime.now()
    return now.replace(hour=h, minute=m, second=sec, microsecond=0).timestamp()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="test.csv")
    ap.add_argument("--from", dest="t0", required=True)
    ap.add_argument("--to", dest="t1", required=True)
    ap.add_argument("--max-rival", type=float, default=0.5)
    ap.add_argument("--min-coef", type=float, default=MIN_COEF,
                    help="drop rows below this pcm_coef -- whole-frame mislocks rival misses")
    a = ap.parse_args()
    t0, t1 = hms_today(a.t0), hms_today(a.t1)

    with open(a.csv) as fh:
        rows = [r for r in fh if not r.startswith("#")]
    reader = csv.DictReader(rows)
    cols = reader.fieldnames or []
    skew_col = next((c for c in cols if c.startswith("offset") or "skew" in c), None)
    rival_col = next((c for c in cols if "rival" in c), None)
    if skew_col is None or "unix_s" not in cols:
        print("columns:", cols)
        raise SystemExit("could not find a skew column / unix_s")

    vals, dropped, n_all, dcoef = [], 0, 0, 0
    for r in reader:
        try:
            t = float(r["unix_s"])
        except (ValueError, KeyError):
            continue
        if not (t0 <= t <= t1):
            continue
        n_all += 1
        if rival_col and r.get(rival_col) not in (None, ""):
            try:
                if float(r[rival_col]) > a.max_rival:
                    dropped += 1
                    continue
            except ValueError:
                pass
        try:
            if r.get("pcm_coef") and float(r["pcm_coef"]) < a.min_coef:
                dcoef += 1
                continue
        except ValueError:
            pass
        try:
            x = float(r[skew_col])
        except ValueError:
            continue
        if x != x or x in (float("inf"), float("-inf")):
            dropped += 1  # PCM lock lost: the analyser writes NaN rather than a number
            continue
        vals.append(x)
    if len(vals) < 5:
        raise SystemExit(f"only {len(vals)} rows in span (of {n_all}); nothing to grade")
    unit = "ns" if skew_col.endswith("_ns") else ("us" if skew_col.endswith("_us") else "")
    scale = 1e-3 if unit == "ns" else 1.0
    v = [x * scale for x in vals]
    med = statistics.median(v)
    mad = statistics.median(abs(x - med) for x in v)
    print(f"{a.csv} {a.t0}-{a.t1}: n={len(v)} (rival-gated {dropped}/{n_all}, coef-gated {dcoef})  "
          f"mean={statistics.mean(v):+.1f} median={med:+.1f} MAD={mad:.1f} sd={statistics.pstdev(v):.1f} "
          f"p2p={max(v)-min(v):.1f} us  [{skew_col}]")


if __name__ == "__main__":
    main()
