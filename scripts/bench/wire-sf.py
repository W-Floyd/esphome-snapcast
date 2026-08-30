#!/usr/bin/env python3
"""Robust structure function of the wire A-B (test.csv, rival-gated < 0.3) over a local wall-clock span.
A swing growing as sqrt(tau) is a rate random walk (P-term noise); flat is white position noise.
Rewritten 2026-08-30 13:00: the previous pairing (nearest row within 0.2*tau) reported tau-10 s
sd of 10898 us on a window whose per-minute medians were inside +-40; pairs are now the first row at
least tau ahead and no more than 0.5 s late, and the span is matched on unix_s, not on time-of-day."""
import csv, statistics as st, datetime as dt, bisect, sys
def ts(h):
    t = dt.datetime.strptime(h, "%H:%M:%S").time()
    return dt.datetime.now().replace(hour=t.hour, minute=t.minute, second=t.second, microsecond=0).timestamp()
t0, t1 = ts(sys.argv[1]), ts(sys.argv[2])
if t1 < t0: t0 -= 86400
rows = []
for r in csv.DictReader(l for l in open("test.csv") if not l.startswith("#")):
    try: t = float(r["unix_s"]); o = float(r["offset_ns"]) / 1000; rv = float(r["rival"])
    except (ValueError, KeyError): continue
    if t0 <= t < t1 and rv < 0.3 and o == o: rows.append((t, o))
rows.sort(); T = [t for t, _ in rows]; X = [x for _, x in rows]
if len(X) < 100: print("only", len(X), "rows"); sys.exit(1)
def mad(v): m = st.median(v); return 1.4826 * st.median([abs(a - m) for a in v])
print(f"{sys.argv[1]}-{sys.argv[2]}: n={len(X)} span {T[-1]-T[0]:.0f}s median {st.median(X):+.2f} us robust-sd {mad(X):.2f} us "
      f"p2p {max(X)-min(X):.0f} us | inside 5/10/20 us: {sum(abs(x)<5 for x in X)/len(X):.0%}/{sum(abs(x)<10 for x in X)/len(X):.0%}/{sum(abs(x)<20 for x in X)/len(X):.0%}")
for tau in (1, 10, 30, 60, 120):
    d = []
    for i, t in enumerate(T):
        j = bisect.bisect_left(T, t + tau)
        if j < len(T) and T[j] - (t + tau) < 0.5: d.append(X[j] - X[i])
    if len(d) > 20: print(f"  tau {tau:4d}s robust sd(diff) {mad(d):7.2f} us  n={len(d)}")
blk = {}
for t, x in rows: blk.setdefault(int(t // 10), []).append(x)
means = [st.mean(v) for v in blk.values() if len(v) > 50]
if len(means) > 2: print(f"  10-s block means robust sd {mad(means):.2f} us ({len(means)} blocks)")
