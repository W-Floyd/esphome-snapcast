#!/opt/homebrew/Cellar/esphome/2026.8.1/libexec/bin/python
"""Decompose the commanded differential rate noise into its TWO injectors. Runtime only, no flash.

WHY TWO. `trim_applied = clamp(kp*e + integral) + align_kick` (snapcast_client.cpp:4634, :4651),
so the commanded rate carries two independent noise sources and a tau_s sweep moves only one:

  P-term      kp * dl_err,  kp = 1/tau_eff = 0.008 at the 120 s floor.  With sd(dl_err) ~ 133 us
              measured overnight, that is ~1.06 ppm per board.
  align kick  bounded by ALIGN_KICK_MAX_PPM = 1.5 ppm (:811), fired on the ~10-20 s align cadence.

Measured sd(trim - int) is 2.15 ppm per board, which BOTH together explain and NEITHER alone does.
A plain tau_s sweep would therefore have under-delivered and been read as "the P-term is not the
mechanism" -- the confound, not the plant.

DESIGN. A-B-A-B with a return to baseline, because the wander's 60-120 s correlation time means a
single before/after pair measures where the wander was (R5.3). Graded on SF(tau <= 10 s), which
R5.3 measured reproducible to ~5% across independent 15-min windows; the plateau needs hours and is
NOT claimed from this.

  arm 0  baseline            both injectors live
  arm 1  align_apply 0       kick OFF, P-term live      -> isolates the P-term
  arm 2  align_apply 1, tau_s 480   kick live, P-term /4 -> isolates the kick
  arm 3  baseline restored   drift control

PREDICTIONS, stated before the run (a prediction made after is a description):
  kick dominant     arm 1 cuts sd(trim-int) hard; arm 2 barely moves it
  P-term dominant   arm 2 cuts it ~4x;            arm 1 barely moves it
  neither           both arms leave it near baseline -> the wander is downstream of the command,
                    and SF_d's "downstream" branch is reached by a route that actually discriminates

CAVEAT to read the wire with: align_apply 0 also stops align correcting real differential offset,
so the wire MEAN may drift in arm 1. That is expected and is why the grade is SF (differential
structure), not the mean.

Both boards always get the same value -- the differential is the quantity of interest.
"""
import asyncio, json, subprocess, sys, time
from aioesphomeapi import APIClient

HOSTS = ["snapclient-supermini-e985e8.local", "snapclient-supermini-f04d74.local"]
ARMS = [
    ("baseline",     {}),
    ("align_off",    {"align_apply": 0}),
    ("tau480",       {"align_apply": 1, "tau_s": 480}),
    ("restore",      {"align_apply": 1, "tau_s": 120}),
]
SETTLE_S = 60
HOLD_S = int(sys.argv[1]) if len(sys.argv) > 1 else 600


async def setp(host, name, value):
    c = APIClient(host, 6053, None)
    await c.connect(login=True)
    _, svcs = await c.list_entities_services()
    svc = next(s for s in svcs if s.name == "servo_param")
    await c.execute_service(svc, {"name": name, "value": float(value)})
    await asyncio.sleep(0.3)
    await c.disconnect()


async def main():
    log = []
    for arm, params in ARMS:
        for k, v in params.items():
            await asyncio.gather(*(setp(h, k, v) for h in HOSTS))
            print(f"[{time.strftime('%H:%M:%S')}] {arm}: set {k}={v} on both", flush=True)
        if not params:
            print(f"[{time.strftime('%H:%M:%S')}] {arm}: no changes (as-found)", flush=True)
        await asyncio.sleep(SETTLE_S)
        t0 = time.time()
        print(f"[{time.strftime('%H:%M:%S')}] {arm}: WINDOW OPEN ({HOLD_S}s)", flush=True)
        await asyncio.sleep(HOLD_S)
        t1 = time.time()
        log.append({"arm": arm, "params": params, "t0": t0, "t1": t1})
        print(f"[{time.strftime('%H:%M:%S')}] {arm}: window closed", flush=True)
        json.dump(log, open("/tmp/injector-ab.json", "w"), indent=1)
    print("DONE ->/tmp/injector-ab.json", flush=True)

asyncio.run(main())
