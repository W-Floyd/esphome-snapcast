#!/usr/bin/env python3
"""Set (or read) a select entity on one or more boards, over the native API.

    python3 scripts/bench/set-select.py --get  'Sync Resilience' host...
    python3 scripts/bench/set-select.py 'Sync Resilience' 'Never mute' host...

The bench case is `Sync Resilience` -> `Never mute`: a muted board emits no I2S, so the
analyser sees no BCLK and the capture reads as a dead bus rather than as a board that chose
silence. Never-mute keeps the wire alive through a re-lock, which is what makes an
acquisition audible -- and measurable -- instead of a gap.

READS BACK WHAT IT WROTE. A command accepted over the API is not proof the entity changed:
the state is what the board is acting on, so the exit status here follows the read-back, not
the send. Exits 1 if any board's final state is not the requested option.
"""

import argparse
import asyncio
import sys

try:
    from aioesphomeapi import APIClient
except ImportError:
    sys.exit("aioesphomeapi not importable by this interpreter; run with esphome's python")


async def one(host, name, option, timeout):
    c = APIClient(host, 6053, None)
    state = {}
    try:
        await asyncio.wait_for(c.connect(login=True), timeout)
        ents, _ = await asyncio.wait_for(c.list_entities_services(), timeout)
        sel = next((e for e in ents
                    if type(e).__name__.startswith("Select") and e.name == name), None)
        if sel is None:
            return host, None, f"no select named {name!r}"
        if option is not None and option not in sel.options:
            return host, None, f"{option!r} not in {list(sel.options)}"

        done = asyncio.Event()

        def on_state(s):
            if getattr(s, "key", None) == sel.key:
                state["v"] = getattr(s, "state", None)
                done.set()

        # subscribe_states is a plain call in some aioesphomeapi versions and a coroutine in
        # others; awaiting unconditionally raises "NoneType can't be awaited". Same guard as
        # scripts/bench/inject.py uses for execute_service.
        r = c.subscribe_states(on_state)
        if hasattr(r, "__await__"):
            await r
        if option is not None:
            c.select_command(sel.key, option)
            # Re-arm so the value read back is the one AFTER the command, not the retained
            # state that arrived with the subscription.
            state.clear()
            done.clear()
        try:
            await asyncio.wait_for(done.wait(), timeout)
        except asyncio.TimeoutError:
            return host, None, "no state published"
        return host, state.get("v"), None
    finally:
        try:
            await c.disconnect()
        except Exception:
            pass


async def main(args):
    results = await asyncio.gather(
        *(one(h, args.name, args.option, args.timeout) for h in args.hosts),
        return_exceptions=True)
    rc = 0
    for host, res in zip(args.hosts, results):
        if isinstance(res, Exception):
            print(f"  {host:34s} ERROR {type(res).__name__}: {res}", file=sys.stderr)
            rc = 1
            continue
        _, value, err = res
        if err:
            print(f"  {host:34s} FAILED {err}", file=sys.stderr)
            rc = 1
        elif args.option is not None and value != args.option:
            print(f"  {host:34s} NOT APPLIED (reads {value!r})", file=sys.stderr)
            rc = 1
        else:
            print(f"  {host:34s} {args.name} = {value!r}")
    return rc


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", help="select entity name, e.g. 'Sync Resilience'")
    ap.add_argument("option", nargs="?", help="option to set; omit (or --get) to just read")
    ap.add_argument("hosts", nargs="+")
    ap.add_argument("--get", action="store_true", help="read only, never write")
    ap.add_argument("--timeout", type=float, default=10.0)
    a = ap.parse_args()
    if a.get and a.option is not None:      # 'name --get host...' puts a host in `option`
        a.hosts.insert(0, a.option)
        a.option = None
    sys.exit(asyncio.run(main(a)))
