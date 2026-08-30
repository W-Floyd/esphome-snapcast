#!/usr/bin/env python3
"""Robust structure function of the wire A-B (test.csv, rival-gated) over a local wall-clock span.
A swing that grows as sqrt(tau) is a rate random walk (P-term noise); flat is white position noise."""
import csv, statistics as st, datetime as dt, bisect, sys
t0=dt.datetime.strptime(sys.argv[1],"%H:%M:%S").time(); t1=dt.datetime.strptime(sys.argv[2],"%H:%M:%S").time()
rows=[]
with open("test.csv") as f:
    for line in f:
        if line.startswith("#"): continue
        hdr=line.strip().split(","); break
    for r in csv.DictReader(f, fieldnames=hdr):
        try: t=float(r["unix_s"]); o=float(r["offset_ns"]); rv=float(r["rival"])
        except: continue
        lt=dt.datetime.fromtimestamp(t).time()
        if t0<=lt<=t1 and rv<0.3: rows.append((t,o/1000))  # 0.3: rows at 0.3-0.5 during events inflated robust sd to 138 us (2026-08-30 11:22-12:02)
rows.sort(); ts=[r[0] for r in rows]; xs=[r[1] for r in rows]
if len(xs)<100: print("only",len(xs),"rows"); sys.exit(1)
def mad(v): m=st.median(v); return 1.4826*st.median([abs(a-m) for a in v])
print(f"{sys.argv[1]}-{sys.argv[2]}: n={len(xs)} span {ts[-1]-ts[0]:.0f}s median {st.median(xs):+.2f} us robust-sd {mad(xs):.2f} us p2p {max(xs)-min(xs):.1f} us")
for tau in (1,10,30,60,120):
    d=[]
    for i,t in enumerate(ts):
        j=bisect.bisect_left(ts,t+tau)
        if j<len(ts) and abs(ts[j]-t-tau)<tau*0.2: d.append(xs[j]-xs[i])
    if len(d)>20: print(f"  tau {tau:5.0f}s  robust sd(diff) {mad(d):7.3f} us  n={len(d)}")
blk={}
for t,x in rows: blk.setdefault(int(t//10),[]).append(x)
means=[st.mean(v) for v in blk.values() if len(v)>50]
if len(means)>2: print(f"  10-s block means robust sd {mad(means):.2f} us ({len(means)} blocks); within-block {st.median([mad(v) for v in blk.values() if len(v)>50]):.2f} us")
