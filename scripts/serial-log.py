#!/usr/bin/env python3
"""Timestamped USB-serial logging, without esphome's log viewer.

Two reasons to prefer this over `esphome logs --device /dev/tty...`:

1. `esphome logs` symbolizes crash lines ("Saved PC:", backtraces) by shelling
   out to cmake in the recorded build directory. A Docker-built project records
   container paths (/config/...), so the very first crash line aborts the whole
   log session with FileNotFoundError -- exactly when the log matters most.
2. USB logging consumes no wifi airtime. When the thing under investigation IS
   airtime, streaming logs over the API is part of
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
import errno
import os
import select
import subprocess
import sys
import time


def stamp():
    return datetime.datetime.now().strftime("[%H:%M:%S.%f")[:-3] + "]"


def resolve(device):
    """macOS: /dev/tty.* is the dial-in device and blocks on carrier-detect
    ("Device not configured" on USB CDC); /dev/cu.* is the call-out device."""
    base = os.path.basename(device)
    if base.startswith("tty."):
        callout = os.path.join(os.path.dirname(device), "cu." + base[4:])
        if os.path.exists(callout):
            return callout
    return device


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("device", help="e.g. /dev/tty.usbmodem101")
    ap.add_argument("--baud", default="115200")
    ap.add_argument("--out", help="also append to this file (like `| tee`)")
    args = ap.parse_args()

    device = resolve(args.device)
    if device != args.device:
        print(f"{stamp()} using {device} (macOS call-out device)", file=sys.stderr)

    sink = open(args.out, "a", buffering=1) if args.out else None
    pending = b""
    fd = None
    try:
        while True:
            if fd is None:
                # A reboot or a flash makes the USB CDC device vanish and return;
                # keep waiting for it instead of exiting (watching reboots is the
                # whole point).
                if not os.path.exists(device):
                    time.sleep(0.5)
                    continue
                try:
                    # O_NONBLOCK so the open itself cannot block on carrier-detect
                    fd = os.open(device, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
                except OSError as exc:
                    if exc.errno in (errno.ENXIO, errno.EBUSY, errno.ENOENT, errno.EIO):
                        time.sleep(0.5)
                        continue
                    raise
                # USB-serial-JTAG ignores baud; a real UART bridge does not.
                # -hupcl/clocal: do NOT drop DTR on close and ignore modem lines --
                # DTR/RTS transitions are exactly how esptool resets an ESP32 and
                # drives it into ROM download mode, so a logger must never toggle them.
                #
                # BSD stty takes -f, GNU stty takes -F, and the call is check=False, so on
                # the wrong one it fails silently and leaves the tty in cooked mode with
                # echo on. Try both and keep whichever the platform accepts.
                settings = [args.baud, "raw", "-echo", "-hupcl", "clocal"]
                for flag in ("-f", "-F"):
                    if subprocess.run(["stty", flag, device] + settings,
                                      check=False, capture_output=True).returncode == 0:
                        break
                else:
                    print(f"{stamp()} --- WARNING: stty failed; tty may not be raw ---",
                          flush=True)
                print(f"{stamp()} --- serial attached ---", flush=True)

            select.select([fd], [], [], 1.0)
            try:
                chunk = os.read(fd, 512)
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    continue
                print(f"{stamp()} --- serial detached ({exc.strerror}) ---", flush=True)
                os.close(fd)
                fd = None
                continue
            if not chunk:  # EOF: device reset
                print(f"{stamp()} --- serial detached (EOF) ---", flush=True)
                os.close(fd)
                fd = None
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
    finally:
        if fd is not None:
            os.close(fd)
        if sink is not None:
            sink.close()


if __name__ == "__main__":
    main()
