#!/usr/bin/env python3
"""Time-to-converge after a flash/boot, per board and on the wire.

usage: converge-time.py --a-off N --b-off N [--wire-from HH:MM:SS] [--band-us 75] [--hold-s 20]
Per board (from the log tail after the byte offset): first DLLOOP, first 'engaged', and the first
time |err| stays inside --band-us for --hold-s. On the wire (test.csv, rival-gated, from --wire-from):
first time |offset| stays inside --wire-band-us for --hold-s. Reports elapsed seconds from the first
log line after the offset (the OTA reboot)."""
import argparse, re, subprocess, csv, datetime as dt, sys
ap=argparse.ArgumentParser()
ap.add_argument("--a-off",type=int,required=True); ap.add_argument("--b-off",type=int,required=True)
ap.add_argument("--wire-from"); ap.add_argument("--band-us",type=float,default=75); ap.add_argument("--wire-band-us",type=float,default=20)
ap.add_argument("--hold-s",type=float,default=20); ap.add_argument("--csv",default="test.csv")
a=ap.parse_args()
TS=re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\]")
def secs(m): return int(m[1])*3600+int(m[2])*60+int(m[3])+int(m[4])/1000
def board(f,off):
    # Start 3 MB before the offset and anchor t0 on the LAST boot marker ('integral restored' / 'Boot
    # seems successful' / first 'Delay loop: setpoint changed') at or before the first post-offset line,
    # so an offset taken a minute after the flash still measures from the reboot.
    start=max(0,off-3000000)
    out=subprocess.run(f"tail -c +{start+1} {f}",shell=True,capture_output=True,text=True).stdout.split("\n")
    pre=out[:]; pos=0; acc=start
    boot=None; boot_kind=None; boot_idx=0
    for i,l in enumerate(out):
        acc+=len(l.encode('utf-8','ignore'))+1
        if acc>off: pos=i; break
    # A PRE-flash offset (the normal case: recorded before the OTA) has the boot AFTER it: take the
    # first marker within the 4 MB after the offset if there is one, else the last one before it.
    for i in range(pos, min(len(out), pos+60000)):
        l=out[i]
        if 'integral restored' in l or 'Boot seems successful' in l:
            m=TS.match(l)
            if m:
                boot=secs(m); boot_kind=('restored' if 'restored' in l else 'safe_mode')+' (after offset)'; boot_idx=i
                break
    if boot is None:
      for i,l in enumerate(out[:pos]):
          # 'integral restored' fires ~1 s after reset; 'Boot seems successful' ~57 s later. Prefer the
          # former as the anchor; a later 'Boot seems successful' must not overwrite it.
          if 'integral restored' in l:
              m=TS.match(l)
              if m: boot=secs(m); boot_kind='restored'; boot_idx=i
          elif 'Boot seems successful' in l and boot_kind!='restored':
              m=TS.match(l)
              if m: boot=secs(m); boot_kind='safe_mode'; boot_idx=i
    out=out[boot_idx:] if boot is not None else out[pos:]  # events from the reboot itself, not from the offset
    t0=boot; eng=None; first_dl=None; inside_since=None; conv=None; last=None
    for l in out:
        m=TS.match(l)
        if not m: continue
        t=secs(m); last=t
        if t0 is None: t0=t
        if t0 is not None and t<t0: continue
        if eng is None and "Delay loop: engaged" in l: eng=t
        e=re.search(r"DLLOOP err=([-+]?\d+) us",l)
        if e:
            if first_dl is None: first_dl=t
            if abs(int(e[1]))<=a.band_us:
                if inside_since is None: inside_since=t
                if conv is None and t-inside_since>=a.hold_s: conv=inside_since
            else: inside_since=None
        if "OUT OF RANGE" in l: inside_since=None
    def rel(x): return "n/a" if x is None or t0 is None else f"{x-t0:6.0f} s"
    print(f"{f}: boot marker {boot_kind or 'NOT found (t0 = first line after offset)'}")
    print(f"{f}: boot +0 | first DLLOOP {rel(first_dl)} | engaged {rel(eng)} | |err|<={a.band_us:.0f} us held {a.hold_s:.0f} s from {rel(conv)} | log spans {rel(last)}")
    return t0
ta=board("a.log",a.a_off); tb=board("b.log",a.b_off)
if a.wire_from:
    t0=dt.datetime.strptime(a.wire_from,"%H:%M:%S").time(); t0s=t0.hour*3600+t0.minute*60+t0.second
    rows=[]
    with open(a.csv) as f:
        for line in f:
            if line.startswith("#"): continue
            hdr=line.strip().split(","); break
        for r in csv.DictReader(f,fieldnames=hdr):
            try: t=float(r["unix_s"]); lt=dt.datetime.fromtimestamp(t); s=lt.hour*3600+lt.minute*60+lt.second+lt.microsecond/1e6
            except: continue
            if s<t0s: continue
            try:
                if float(r["rival"])>0.5: rows.append((s,None)); continue
                rows.append((s,float(r["offset_ns"])/1000))
            except: rows.append((s,None))
    inside=None; conv=None; first=None
    for s,o in rows:
        if first is None: first=s
        if o is not None and abs(o)<=a.wire_band_us:
            if inside is None: inside=s
            if conv is None and s-inside>=a.hold_s: conv=inside
        else: inside=None
    if not rows: print("wire: no rows after", a.wire_from)
    else: print(f"wire: first row +{first-t0s:.0f} s | |offset|<={a.wire_band_us:.0f} us held {a.hold_s:.0f} s from {'n/a' if conv is None else f'+{conv-t0s:.0f} s'} | rows {len(rows)} to +{rows[-1][0]-t0s:.0f} s")
