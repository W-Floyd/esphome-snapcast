#!/usr/bin/env python3
"""Settle render_align's SIGN from a shadow window: for each 'RALIGN shadow' line on a board, pair its
group delta with the wire A-B (test.csv, rival-gated, +-2 s mean). If the channel's convention is
right, a board reading a NEGATIVE delta is LATE, i.e. for A: wire(B-A) NEGATIVE; for B: wire POSITIVE.
usage: align-shadow.py HH:MM:SS HH:MM:SS"""
import re, subprocess, csv, datetime as dt, statistics as st, math, bisect, sys
t0=dt.datetime.strptime(sys.argv[1],"%H:%M:%S").time(); t1=dt.datetime.strptime(sys.argv[2],"%H:%M:%S").time()
def secs(h,m,s,ms=0): return h*3600+m*60+s+ms/1000
lo=secs(t0.hour,t0.minute,t0.second); hi=secs(t1.hour,t1.minute,t1.second)
wire=[]
with open("test.csv") as f:
    for line in f:
        if line.startswith("#"): continue
        hdr=line.strip().split(","); break
    for r in csv.DictReader(f,fieldnames=hdr):
        try: t=float(r["unix_s"]); lt=dt.datetime.fromtimestamp(t); s=secs(lt.hour,lt.minute,lt.second,lt.microsecond/1000)
        except: continue
        if lo<=s<=hi and float(r["rival"])<=0.5: wire.append((s,float(r["offset_ns"])/1000))
wire.sort(); ws=[w[0] for w in wire]
def near(t):
    i=bisect.bisect_left(ws,t-2); j=bisect.bisect_right(ws,t+2); return st.mean(w[1] for w in wire[i:j]) if j-i>5 else None
def corr(x,y):
    mx,my=st.mean(x),st.mean(y); sx=math.sqrt(sum((p-mx)**2 for p in x)); sy=math.sqrt(sum((p-my)**2 for p in y)); return sum((p-mx)*(q-my) for p,q in zip(x,y))/(sx*sy) if sx and sy else float('nan')
# Convention as MEASURED on the wire (see PLAN 17:03-17:10), not derived: a POSITIVE delta = this board is EARLY. Wire is B-A, so A late <=> wire negative <=> r(delta_A, wire) POSITIVE;
# B late <=> wire positive <=> r(delta_B, wire) NEGATIVE.
# MEASURED 17:03-17:10: positive delta = this board is EARLY. A early <=> wire(B-A) POSITIVE <=> r(delta_A, wire) POSITIVE;
# B early <=> wire NEGATIVE <=> r(delta_B, wire) NEGATIVE.
for board,f,expect in (("A","a.log",+1),("B","b.log",-1)):
    out=subprocess.run(f"tail -c 40000000 {f} | grep -a 'RALIGN' | grep -oaE '^\\[[0-9:.]+\\]|group [-+0-9]+'",shell=True,capture_output=True,text=True).stdout.split("\n")
    x=[];y=[]
    for i in range(0,len(out)-1,2):
        m=re.match(r"\[(\d\d):(\d\d):(\d\d)\.(\d+)\]",out[i]); g=re.search(r"group ([-+]?\d+)",out[i+1])
        if not (m and g): continue
        t=secs(int(m[1]),int(m[2]),int(m[3]),int(m[4]))
        if not (lo<=t<=hi): continue
        d=int(g[1])
        if abs(d)>500: continue
        w=near(t)
        if w is not None: x.append(d); y.append(w)
    if len(x)<8: print(f"{board}: only {len(x)} pairs"); continue
    r=corr(x,y)
    print(f"{board}: n={len(x)} delta median {st.median(x):+.0f} us, wire median {st.median(y):+.1f} us, r(delta, wire B-A) = {r:+.2f}  -> convention {'RIGHT' if (r*expect)>0.3 else 'WRONG' if (r*expect)<-0.3 else 'UNDECIDED'} (expect sign {expect:+d})")
