#!/usr/bin/env python3
"""Print a board's running build, over the native API. The witness for an OTA.

    python3 scripts/bench/device-info.py analyzed-supermini-e8c440.local

An OTA log that says "successful" is NOT evidence that the new build is running: a replug
inside the first minute after the reboot rolls both halves of the flash back, the log still
reads clean, and a whole session was then spent grading a gate that was never in the running
binary. `compilation_time` is the thing that cannot lie about which build answered.

Exits 0 with "<name>\t<compilation_time>\t<version>" on stdout, 1 if the board cannot be
reached -- so a caller can diff the string across a flash rather than trusting the flasher.
"""

import asyncio
import sys

try:
    from aioesphomeapi import APIClient
except ImportError:
    sys.exit("aioesphomeapi not importable by this interpreter; run with esphome's python")


async def main(host, timeout):
    c = APIClient(host, 6053, None)
    try:
        await asyncio.wait_for(c.connect(login=True), timeout)
        info = await asyncio.wait_for(c.device_info(), timeout)
    finally:
        try:
            await c.disconnect()
        except Exception:
            pass
    print(f"{info.name}\t{info.compilation_time}\t{info.esphome_version}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    try:
        asyncio.run(main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 10.0))
    except Exception as e:
        print(f"{sys.argv[1]}: {type(e).__name__}: {e}", file=sys.stderr)
        sys.exit(1)
