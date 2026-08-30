#!/opt/homebrew/Cellar/esphome/2026.8.1/libexec/bin/python
"""Deterministic resync test: inject a starvation of N ms on one board over the native API, then time
how long the wire (test.csv, rival-gated) takes to come back inside a band and hold it.
usage: resync-test.py HOST MS [--band-us 100] [--hold-s 5] [--timeout-s 90]
Prints the disturbance peak and the time from injection to the first hold, plus 1-s medians."""
import asyncio, sys, time, csv, statistics as st, argparse
from aioesphomeapi import APIClient
ap=argparse.ArgumentParser(); ap.add_argument("host"); ap.add_argument("ms",type=int)
ap.add_argument("--band-us",type=float,default=100); ap.add_argument("--hold-s",type=float,default=5); ap.add_argument("--timeout-s",type=float,default=90)
a=ap.parse_args()
async def inject():
    c=APIClient(a.host,6053,None); await c.connect(login=True)
    ents,svcs=await c.list_entities_services()
    svc=[s for s in svcs if s.name=="inject_starvation"]
    if not svc: print("no inject_starvation service"); return None
    c.execute_service(svc[0],{"ms":a.ms}); await asyncio.sleep(0.3); await c.disconnect(); return time.time()
t0=asyncio.run(inject())
if t0 is None: sys.exit(1)
print(f"injected {a.ms} ms starvation on {a.host} at {time.strftime('%H:%M:%S',time.localtime(t0))}")
pos=0; per={}; first=None; peak=0.0
with open("test.csv") as f:
    f.seek(0,2); pos=f.tell()
while time.time()-t0<a.timeout_s:
    time.sleep(1)
    with open("test.csv") as f:
        f.seek(pos); rows=f.read(); pos=f.tell()
    for r in rows.split("\n"):
        p=r.split(",")
        if len(p)<8: continue
        try: t=float(p[1]); o=float(p[2])/1000; rv=float(p[6])
        except: continue
        if t<t0: continue
        k=int(t-t0); per.setdefault(k,[]).append(o if rv<=0.5 else None)
    hold=0
    for k in sorted(per):
        v=[x for x in per[k] if x is not None]
        m=st.median(v) if v else None
        if m is not None: peak=max(peak,abs(m))
        hold = hold+1 if (m is not None and abs(m)<a.band_us) else 0
        if hold>=a.hold_s and first is None: first=k-a.hold_s+1
    if first is not None and time.time()-t0>first+a.hold_s+3: break
print("1-s medians:", " ".join(f"{k}:{'x' if not [x for x in per[k] if x is not None] else f'{st.median([x for x in per[k] if x is not None]):+.0f}'}" for k in sorted(per)))
print(f"peak |wire| {peak:.0f} us; |wire| < {a.band_us:.0f} us held {a.hold_s:.0f} s from +{first} s" if first is not None else f"peak |wire| {peak:.0f} us; NOT back inside {a.band_us:.0f} us within {a.timeout_s:.0f} s")
