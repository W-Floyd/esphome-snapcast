#!/usr/bin/env python3
"""Does GDIN raw's spread GROW with the pairing gap? Reads GDIN lines on stdin.

A linear bias from the gap is what corr(raw, gap) tests, and it is absent (r=0.098/0.009). A
RANDOM WALK is different: it shows up as MAD proportional to sqrt|gap|, not as a slope, so it
survives that test. This bins by |gap| and prints the sqrt prediction beside the measurement.

Answer on 2026-09-03: FLAT. MAD 14-17 us across |gap| 0.6 -> 104 ms, a 170x range, where sqrt
accumulation predicts 15 -> 195. The noise is not accumulated over the gap.

The same table shows raw MAD (15 us) against raw sd (48-269 us): the core is as tight as the
wire and the excess is entirely tails. Grade the core with MAD; sd here measures the outliers.
"""
import sys, re, statistics as st, math
rows = []
for l in sys.stdin:
    if "GDIN " not in l: continue
    d = dict(re.findall(r"(\w+)=([-+]?[0-9]+\.?[0-9]*)", l.split("GDIN ", 1)[1]))
    try:
        raw = float(d["raw"]); gap = abs(float(d["gap"]))
    except (KeyError, ValueError):
        continue
    if abs(raw) < 5000:      # excursions graded separately; they are not the noise floor
        rows.append((gap, raw))
if len(rows) < 200:
    print("    only %d rows" % len(rows)); sys.exit()
rows.sort()
print("    n=%d (|raw|<5000 only)" % len(rows))
print("      %14s %6s %9s %9s %14s" % ("|gap| med ms", "n", "raw MAD", "raw sd", "sqrt pred"))
nb = 6; per = len(rows) // nb; base = None
for i in range(nb):
    ch = rows[i*per:(i+1)*per] if i < nb-1 else rows[(nb-1)*per:]
    g = [c[0] for c in ch]; v = [c[1] for c in ch]
    med = st.median(v); mad = st.median([abs(x-med) for x in v]); gm = st.median(g)/1000.0
    if base is None: base = (gm, mad)
    pred = base[1]*math.sqrt(gm/base[0]) if base[0] > 0 else float("nan")
    print("      %14.1f %6d %9.1f %9.1f %14.1f" % (gm, len(ch), mad, st.pstdev(v), pred))
print("    (random walk over the gap => MAD proportional to sqrt|gap|; flat MAD => not accumulated)")
