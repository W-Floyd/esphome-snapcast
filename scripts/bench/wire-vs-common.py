#!/usr/bin/env python3
"""Does the wire A-B wander follow the COMMON delay-loop error (gain/phase mismatch between the two
loops) or not (per-board measurement noise)? 5-s block means over a local wall-clock span of
test.csv, rival-gated. usage: wire-vs-common.py HH:MM:SS HH:MM:SS"""
import csv, datetime as dt, statistics as st, math, sys
t0=dt.datetime.strptime(sys.argv[1],"%H:%M:%S").time(); t1=dt.datetime.strptime(sys.argv[2],"%H:%M:%S").time()
wire={}; err={}; rate={}
with open("test.csv") as f:
    for line in f:
        if line.startswith("#"): continue
        hdr=line.strip().split(","); break
    for r in csv.DictReader(f, fieldnames=hdr):
        try: t=float(r["unix_s"]); lt=dt.datetime.fromtimestamp(t).time()
        except: continue
        if not (t0<=lt<=t1): continue
        try:
            if float(r["rival"])>0.5: continue
            k=int(t//5); wire.setdefault(k,[]).append(float(r["offset_ns"])/1000)
            rate.setdefault(k,[]).append((float(r["fs_b_hz"])-float(r["fs_a_hz"]))/44100*1e6)
        except: continue
        try: err.setdefault(k,[]).append((float(r["dl_err_a_us"]),float(r["dl_err_b_us"])))
        except: pass
ks=sorted(k for k in wire if len(wire[k])>30 and k in err and len(err[k])>3)
o=[st.mean(wire[k]) for k in ks]; ea=[st.mean(e[0] for e in err[k]) for k in ks]; eb=[st.mean(e[1] for e in err[k]) for k in ks]
dfs=[st.mean(rate[k]) for k in ks]
common=[(a+b)/2 for a,b in zip(ea,eb)]; diff=[a-b for a,b in zip(ea,eb)]
do=[o[i+1]-o[i] for i in range(len(o)-1)]; dc=[common[i+1]-common[i] for i in range(len(common)-1)]
def corr(x,y):
    mx,my=st.mean(x),st.mean(y); sx=math.sqrt(sum((a-mx)**2 for a in x)); sy=math.sqrt(sum((b-my)**2 for b in y))
    return sum((a-mx)*(b-my) for a,b in zip(x,y))/(sx*sy) if sx and sy else float('nan')
print(f"{sys.argv[1]}-{sys.argv[2]}: {len(ks)} blocks of 5 s; common err {min(common):+.0f}..{max(common):+.0f} us; wire {min(o):+.1f}..{max(o):+.1f} us")
print(f"  r(wire change, common-err change) = {corr(do,dc):+.2f}   gain/phase mismatch if high")
print(f"  r(wire change, common-err level)  = {corr(do,common[:-1]):+.2f}")
print(f"  r(wire level, err_a - err_b)      = {corr(o,diff):+.2f}   on-device differential sees the wire if high")
print(f"  r(wire change, B-A rate ppm)      = {corr(do,dfs[:-1]):+.2f}   sanity")
print(f"  B-A rate robust sd {1.4826*st.median([abs(x-st.median(dfs)) for x in dfs]):.3f} ppm; err_a-err_b robust sd {1.4826*st.median([abs(x-st.median(diff)) for x in diff]):.1f} us")
