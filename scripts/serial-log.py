#!/usr/bin/env python3
"""Timestamped USB-serial logging, without esphome's log viewer.

Two reasons to prefer this over `esphome logs --device /dev/tty...`:

1. `esphome logs` symbolizes crash lines ("Saved PC:", backtraces) by shelling
   out to cmake in the recorded build directory. A Docker-built project records
   container paths (/config/...), so the very first crash line aborts the whole
   log session with FileNotFoundError -- exactly when the log matters most.
2. USB logging consumes no wifi airtime. When the thing under investigation IS
   airtime (see scripts/ap-stations.py), streaming logs over the API is part of
   the problem.

Timestamps are emitted as [HH:MM:SS.mmm] so the output is parseable by
scripts/diagnose-logs.py and scripts/sync-delta.py just like esphome's own logs.

    python3 scripts/serial-log.py /dev/tty.usbmodem101 | tee c.log
    python3 scripts/serial-log.py /dev/tty.usbmodem101 --out c.log

Crash lines are passed through verbatim (undecoded). To symbolize one:
    xtensa-esp-elf-addr2line -pfiaC -e <build>/<name>.elf 0x<PC>
"""

import argparse
import datetime
import subprocess
import sys


def stamp():
    return datetime.datetime.now().strftime("[%H:%M:%S.%f")[:-3] + "]"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("device", help="e.g. /dev/tty.usbmodem101")
    ap.add_argument("--baud", default="115200")
    ap.add_argument("--out", help="also append to this file (like `| tee`)")
    args = ap.parse_args()

    # ESP32-S3 USB-serial-JTAG ignores the baud rate, but a real UART bridge does not
    subprocess.run(["stty", "-f", args.device, args.baud, "raw", "-echo"], check=True)

    sink = open(args.out, "a", buffering=1) if args.out else None
    pending = b""
    try:
        with open(args.device, "rb", buffering=0) as port:
            while True:
                chunk = port.read(256)
                if not chunk:
                    continue
                pending += chunk
                while b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    line = stamp() + raw.rstrip(b"\r").decode("utf-8", "replace")
                    print(line, flush=True)
                    if sink is not None:
                        sink.write(line + "\n")
    except KeyboardInterrupt:
        pass
    except OSError as exc:  # device unplugged / reset during flashing
        print(f"{stamp()} serial closed: {exc}", file=sys.stderr)
    finally:
        if sink is not None:
            sink.close()


if __name__ == "__main__":
    main()
