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

    vals, dropped, n_all = [], 0, 0
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
    print(f"{a.csv} {a.t0}-{a.t1}: n={len(v)} (rival-gated {dropped}/{n_all})  "
          f"mean={statistics.mean(v):+.1f} median={med:+.1f} MAD={mad:.1f} sd={statistics.pstdev(v):.1f} "
          f"p2p={max(v)-min(v):.1f} us  [{skew_col}]")


if __name__ == "__main__":
    main()
