"""Refuse to measure until the bench is in a known state.

Written after a campaign was killed mid-cycle and left one board at latency=500 ms server-side.
Everything measured afterwards was contaminated, including a "2.46 ms standing offset" that was
reported as a finding and used to justify a firmware change. The scripts set-and-restore in
pairs, so ANY interrupted run can leave a stray setting -- checking is cheap, and the failure is
silent and convincing.

The observer is checked too, not just the probed pair: it now contributes a third estimate to the
consensus, so it being on another stream or a non-zero latency changes what the pair adopts.
"""
import os, sys, importlib.util

P = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("snapctl", P + "/snapctl.py")
m = importlib.util.module_from_spec(spec)
sys.argv = ["x", "noop"]
spec.loader.exec_module(m)

BOARDS = ("f04d74", "e985e8", "e99574")
r = m.rpc("Server.GetStatus")["result"]["server"]
ok, seen = True, {}
for g in r["groups"]:
    for c in g["clients"]:
        n = c["host"]["name"]
        for b in BOARDS:
            if b in n:
                seen[b] = (c["config"].get("latency"), g.get("stream_id"), c.get("connected"))
for b in BOARDS:
    lat, stream, conn = seen.get(b, (None, None, None))
    bad = []
    if lat != 0:
        bad.append(f"latency={lat} (want 0)")
    if stream != "MLS44":
        bad.append(f"stream={stream} (want MLS44)")
    if not conn:
        bad.append("NOT CONNECTED")
    print(f"  {b}: latency={lat} stream={stream} connected={conn}"
          + ("   <-- " + ", ".join(bad) if bad else "   ok"))
    if bad:
        ok = False
streams = {s for _, s, _ in seen.values()}
if len(streams) > 1:
    print(f"  boards are on DIFFERENT streams {streams} -- render phase is out of contract")
    ok = False
print("PREFLIGHT " + ("OK" if ok else "FAILED"))
sys.exit(0 if ok else 1)
