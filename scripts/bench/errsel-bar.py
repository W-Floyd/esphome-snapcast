#!/usr/bin/env python3
"""STAGE 3a BAR: does active_error() agree with the live selector through a disturbance?

    python3 scripts/bench/errsel-bar.py                    # quiet + starvation + split
    python3 scripts/bench/errsel-bar.py --phase quiet      # just read the counters

PLAN-timing-v2 Stage 3a: "run both selectors live, log a mismatch counter, change nothing. Bar:
zero mismatches over a session including an injected starvation and an `inject_split`. Only then
swap the consumers."

Quiet agreement is the easy half and it already holds (srcdiff=0 over thousands of chunks on both
boards). The two selectors differ only in conditions that a settled bench never exercises:
`active_error()` additionally requires `dl_err_at_us != 0` and an accumulator fresher than
`tune_tag_stale_ms_`. A starvation is what makes tags go missing and an `inject_split` is what
makes the ledger disagree with them, so those two events are the whole test — and if the counters
stay at zero through both, the extra conditions are not a fix, they are dead weight, which is a
result worth having either way.

ERRSEL counters are CUMULATIVE since boot, so every number here is a delta between phases. The
counters are read from the serial logs rather than over the API because that is where they are,
and because a log line carries the wall clock that the injections are timed against.

Run this ON the bench host (it reads a.log/b.log and needs aioesphomeapi for the hooks).
"""

import argparse
import asyncio
import os
import re
import subprocess
import sys
import time

LOGS = {}          # name -> path, set in main(); report() needs it to count events by offset

ERRSEL = re.compile(
    r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*?\bERRSEL n=(\d+) srcdiff=(\d+) valdiff=(\d+) "
    r"live=(\w+) new=(\w+) worst=([+-]\d+)"
)
EVENTS = ("Hard resync", "OUT OF RANGE", "PLAYER STALLED", "TAGFAULT", "Muting")


def last_errsel(path, tail_mb=8):
    """Newest ERRSEL counters in a log, or None. Byte-anchored tail: these logs span days."""
    out = subprocess.run(["tail", "-c", str(tail_mb << 20), path],
                         capture_output=True).stdout.decode(errors="replace")
    m = None
    for m in ERRSEL.finditer(out):
        pass
    if m is None:
        return None
    return {"t": f"{m.group(1)}:{m.group(2)}:{m.group(3)}", "n": int(m.group(5)),
            "srcdiff": int(m.group(6)), "valdiff": int(m.group(7)),
            "live": m.group(8), "new": m.group(9), "worst": int(m.group(10))}


def log_size(path):
    return os.path.getsize(path)


def event_counts_since(path, offset):
    """Events in the bytes appended after `offset`. BYTE-ANCHORED, not a fixed tail.

    Counting in a `tail -c 8M` window and differencing gives NEGATIVE deltas as the log grows and
    old occurrences slide out of the window -- which is exactly what the first run of this script
    printed ("Hard resync: -1", "OUT OF RANGE: -4"). A count whose window moves is not a count.
    """
    with open(path, "rb") as fh:
        fh.seek(offset)
        out = fh.read().decode(errors="replace")
    return {e: out.count(e) for e in EVENTS}


async def call(host, action, arg_name, value, timeout=10.0):
    from aioesphomeapi import APIClient
    c = APIClient(host, 6053, None)
    try:
        await asyncio.wait_for(c.connect(login=True), timeout)
        _, svcs = await asyncio.wait_for(c.list_entities_services(), timeout)
        svc = next((s for s in svcs if s.name == action), None)
        if svc is None:
            raise RuntimeError(f"{host}: no {action} action")
        r = c.execute_service(svc, {arg_name: value})
        if hasattr(r, "__await__"):      # coroutine in newer aioesphomeapi, a plain call in older
            await r
        await asyncio.sleep(1.0)          # let it flush before the socket closes
    finally:
        try:
            await c.disconnect()
        except Exception:
            pass


def sample(logs):
    """Counters plus the byte offset each phase's event count will be measured from."""
    return {name: (last_errsel(path), log_size(path)) for name, path in logs.items()}


def arms_seen(path, offset):
    """Which selector arms the log shows after `offset`. A phase in which the live selector never
    left the tag path did not exercise the difference between the two selectors, so zero mismatches
    there is a weak result -- say so rather than counting it as a pass."""
    with open(path, "rb") as fh:
        fh.seek(offset)
        out = fh.read().decode(errors="replace")
    return {m.group(8) for m in ERRSEL.finditer(out)}


def report(before, after, label):
    print(f"\n=== {label} ===")
    print(f"  {'board':<8} {'chunks':>8} {'srcdiff':>8} {'valdiff':>8} {'worst us':>9}  events")
    ok = True
    for name in before:
        b, boff = before[name]
        a, _ = after[name]
        if b is None or a is None:
            print(f"  {name:<8} no ERRSEL line (build predates the shadow?)")
            ok = False
            continue
        dn = a["n"] - b["n"]
        ds = a["srcdiff"] - b["srcdiff"]
        dv = a["valdiff"] - b["valdiff"]
        ev = {k: v for k, v in event_counts_since(LOGS[name], boff).items() if v}
        arms = arms_seen(LOGS[name], boff)
        print(f"  {name:<8} {dn:>8} {ds:>8} {dv:>8} {a['worst']:>+9}  "
              f"arms={'+'.join(sorted(arms)) or '?'}  {ev if ev else '-'}")
        if arms and arms == {"tag"}:
            print(f"           (live selector stayed on tags throughout -- this phase did not "
                  f"exercise the ledger arm)")
        if dn <= 0:
            print(f"           !! no chunks advanced -- the board was not playing, so this phase "
                  f"tested nothing")
            ok = False
        if ds or dv:
            ok = False
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host-a", default="analyzed-supermini-e8c440.local")
    ap.add_argument("--host-b", default="analyzed-supermini-665b84.local")
    ap.add_argument("--log-a", default="a.log")
    ap.add_argument("--log-b", default="b.log")
    ap.add_argument("--starve-ms", type=int, default=300)
    ap.add_argument("--split-us", type=int, default=1000)
    ap.add_argument("--settle", type=float, default=20.0,
                    help="seconds to let each disturbance play out; must exceed the 5 s ERRSEL "
                         "period or the phase's own counters never get emitted")
    ap.add_argument("--phase", choices=["quiet", "starve", "split", "all"], default="all")
    args = ap.parse_args()

    logs = {"a": args.log_a, "b": args.log_b}
    LOGS.update(logs)
    hosts = {"a": args.host_a, "b": args.host_b}
    passed = True

    base = sample(logs)
    for name, (e, _) in base.items():
        if e is None:
            sys.exit(f"{logs[name]}: no ERRSEL line -- flash the Stage 3a shadow build first")
        print(f"  {name}: ERRSEL n={e['n']} srcdiff={e['srcdiff']} valdiff={e['valdiff']} "
              f"(last at {e['t']})")

    if args.phase in ("quiet", "all"):
        print(f"\nquiet baseline: {args.settle:.0f}s with nothing done to the bench")
        time.sleep(args.settle)
        passed &= report(base, sample(logs), f"QUIET ({args.settle:.0f}s)")
        base = sample(logs)

    if args.phase in ("starve", "all"):
        print(f"\ninjecting {args.starve_ms} ms starvation on board a ({hosts['a']})")
        asyncio.run(call(hosts["a"], "inject_starvation", "ms", args.starve_ms))
        time.sleep(args.settle)
        passed &= report(base, sample(logs), f"STARVATION {args.starve_ms} ms on a")
        base = sample(logs)

    if args.phase in ("split", "all"):
        print(f"\ninjecting {args.split_us:+d} us split on board a ({hosts['a']})")
        asyncio.run(call(hosts["a"], "inject_split", "us", args.split_us))
        time.sleep(args.settle)
        passed &= report(base, sample(logs), f"SPLIT {args.split_us:+d} us on a")

    print()
    if passed:
        print("BAR MET: zero selector mismatches through every phase, and every phase advanced "
              "chunks.\n  Note what that means: the two extra conditions in active_error() never "
              "bound, even\n  under the disturbances designed to make them bind. Swapping the "
              "consumers is safe, and\n  the stricter test buys nothing measurable -- decide "
              "deliberately whether to keep it.")
        return 0
    print("BAR NOT MET: see the phases above. A mismatch must be explained before any consumer\n"
          "  is swapped -- that is the whole reason the selector runs as a shadow.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
