#!/usr/bin/env python3
"""Bench watchdog: one line per event (5-min cooldown per kind), for a Monitor to relay.
Watches a.log/b.log/observer.log tails and test.csv. Events:
  TAGFAULT / PLAYER STALLED / reconnecting / 'no chunk records' (per board, immediate)
  ERR>5ms held 60 s (DLLOOP or OUT OF RANGE), SPLIT: |SHADOW diff| > 5 ms held 60 s
  WIRE: |A-B| > 200 us held 60 s; NOCORR: rival > 0.5 for 60 s; CSVSTALL: no new rows for 90 s
  LOGGAP: a log tail silent > 60 s (sleep / serial wedge)
"""
import os, re, time, collections
LOGS={"A":"a.log","B":"b.log","OBS":"observer.log"}; CSV="test.csv"
COOL=300; last={}
def emit(kind,msg):
    t=time.time()
    cool=1800 if kind.endswith("LOGGAP") else COOL   # a wedged tail is one event, not one per 5 min
    if t-last.get(kind,0)<cool: return
    last[kind]=t; print(time.strftime("%H:%M:%S"),kind,msg,flush=True)
pos={k:os.path.getsize(v) for k,v in LOGS.items()}
lastline={k:time.time() for k in LOGS}
errsince={}; splitsince={}
cpos=os.path.getsize(CSV); clast=time.time(); wire_bad_since=None; nocorr_since=None
TS=re.compile(r"^\[(\d\d):(\d\d):(\d\d)")
while True:
    now=time.time()
    for k,f in LOGS.items():
        try: sz=os.path.getsize(f)
        except OSError: continue
        if sz<pos[k]: pos[k]=0
        if sz>pos[k]:
            with open(f,"rb") as fh:
                fh.seek(pos[k]); data=fh.read(sz-pos[k]).decode("utf-8","ignore")
            pos[k]=sz; lastline[k]=now
            for l in data.split("\n"):
                if "TAGFAULT" in l: emit(f"{k}-TAGFAULT", l[:140])
                elif "PLAYER STALLED" in l: emit(f"{k}-STALLED", l[:120])
                elif "not catching up: reconnecting" in l: emit(f"{k}-BAILOUT", l[:120])
                elif "no chunk records for" in l: emit(f"{k}-STARVE", l[:100])
                elif "Boot seems successful" in l or "integral restored" in l: emit(f"{k}-BOOT", l[:100])
                m=re.search(r"DLLOOP err=([-+]?\d+) us",l)
                if m:
                    if abs(int(m[1]))>5000:
                        errsince.setdefault(k,now)
                        if now-errsince[k]>60: emit(f"{k}-ERR5MS", f"|err| > 5 ms for 60 s: {l[:90]}")
                    else: errsince.pop(k,None)
                m=re.search(r"SHADOW err_tag=([-+]?\d+) err_live=([-+]?\d+) diff=([-+]?\d+)",l)
                if m:
                    if abs(int(m[3]))>5000:
                        splitsince.setdefault(k,now)
                        if now-splitsince[k]>60: emit(f"{k}-SPLIT", f"tag/ledger split held 60 s: {m[0]}")
                    else: splitsince.pop(k,None)
        elif now-lastline[k]>60: emit(f"{k}-LOGGAP", f"{f} silent {int(now-lastline[k])} s (sleep? wedge?)")
    try: csz=os.path.getsize(CSV)
    except OSError: csz=cpos
    if csz < cpos:
        cpos, clast = csz, now  # recreated/truncated file: reset the baseline, not a stall
    elif csz > cpos:
        with open(CSV,"rb") as fh:
            fh.seek(cpos); rows=fh.read(csz-cpos).decode("utf-8","ignore").split("\n")
        cpos=csz; clast=now
        offs=[]; riv=[]
        for r in rows:
            p=r.split(",")
            if len(p)<8 or p[0].startswith("#") or p[0]=="elapsed_s": continue
            try: o=float(p[2])/1000; rv=float(p[6])
            except: continue
            riv.append(rv)
            if rv<=0.5: offs.append(o)
        if riv:
            if all(r>0.5 for r in riv):
                nocorr_since=nocorr_since or now
                if now-nocorr_since>60: emit("NOCORR", f"analyzer rival > 0.5 for 60 s (boards > 17 ms apart or no stimulus)")
            else: nocorr_since=None
        if offs:
            med=sorted(offs)[len(offs)//2]
            if abs(med)>200:
                wire_bad_since=wire_bad_since or now
                if now-wire_bad_since>60: emit("WIRE", f"|A-B| = {med:+.0f} us for 60 s")
            else: wire_bad_since=None
    elif now-clast>90: emit("CSVSTALL", f"test.csv not growing for {int(now-clast)} s")
    time.sleep(5)
