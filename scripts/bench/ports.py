#!/usr/bin/env python3
"""List attached ESP32 USB serial ports with a stable per-board identity. macOS and Linux.

Ports are enumerated by Espressif VID 0x303A, the same test scripts/flash.sh uses. The
identity is the USB serial number, which for the S3's native USB-Serial/JTAG is the MAC --
so its last six hex digits are exactly the board suffix used everywhere else in this repo
(e985e8, f04d74, e99574, ...). That matters more than it looks: the device path
(/dev/cu.usbmodemNNN on macOS, /dev/ttyACMn on Linux) is assigned in enumeration order, so
it moves between boards across a replug or a reboot, and a logger that trusted the path
would quietly write board B's lines into a.log. Every downstream conclusion would then be
attributed to the wrong board, and nothing in the log would say so.

Two backends, in order:

  1. pyserial, which reports vid/serial_number identically on both platforms. It comes from
     esphome's own interpreter (it is an esphome dependency), so it is normally already there.
  2. sysfs (/sys/class/tty), on Linux only, when pyserial is not importable. This walks up
     from each tty to the USB device that owns it. `lsusb` is the obvious thing to reach for
     and the wrong one: it enumerates USB devices but cannot tell you which /dev/ttyACMn any
     of them became, which is the entire question here.

A bridge-based board (CH340, CP210x) has no MAC in its descriptor. Those come back with an
empty suffix and must be pinned by hand in boards.conf -- refusing is correct, guessing is not.

    python3 scripts/bench/ports.py            # TSV: device, suffix, serial, product
    python3 scripts/bench/ports.py --json
"""

import argparse
import glob
import json
import os
import re
import sys

ESPRESSIF_VID = 0x303A


def suffix_of(serial):
    """Last six hex digits of a MAC-shaped USB serial number, lowercased."""
    if not serial:
        return ""
    hexonly = re.sub(r"[^0-9a-fA-F]", "", serial)
    # A MAC is 12 hex digits. Anything shorter (e.g. the literal "0" some boards report,
    # or a bridge's "0001") is not an identity and must not be treated as one.
    if len(hexonly) < 12:
        return ""
    return hexonly[-6:].lower()


def from_pyserial():
    import serial.tools.list_ports
    rows = []
    for p in serial.tools.list_ports.comports():
        if p.vid != ESPRESSIF_VID:
            continue
        rows.append({"device": p.device, "suffix": suffix_of(p.serial_number),
                     "serial": p.serial_number or "", "product": p.product or ""})
    return rows


def _read(path):
    try:
        with open(path) as fh:
            return fh.read().strip()
    except OSError:
        return ""


def from_sysfs(root="/"):
    """Linux fallback: /sys/class/tty/<n>/device -> walk up to the owning USB device.

    `root` exists so this can be tested against a synthetic tree on a machine that has no
    /sys at all -- the alternative is shipping the Linux path unexecuted.
    """
    # realpath: the walk below compares resolved paths, so the boundary must be resolved too
    # (/sys is already canonical on Linux; a test root under a symlinked /tmp is not).
    sysroot = os.path.realpath(os.path.join(root, "sys"))
    rows = []
    for tty in sorted(glob.glob(os.path.join(sysroot, "class/tty/*"))):
        link = os.path.join(tty, "device")
        if not os.path.exists(link):
            continue
        # ttyACM0's device is the CDC interface; idVendor lives a couple of levels up on the
        # USB device itself. Walk up until a directory carries it, stopping at /sys.
        node = os.path.realpath(link)
        while node.startswith(sysroot) and node != sysroot:
            if os.path.exists(os.path.join(node, "idVendor")):
                break
            node = os.path.dirname(node)
        else:
            continue
        if not os.path.exists(os.path.join(node, "idVendor")):
            continue
        try:
            vid = int(_read(os.path.join(node, "idVendor")), 16)
        except ValueError:
            continue
        if vid != ESPRESSIF_VID:
            continue
        serial = _read(os.path.join(node, "serial"))
        rows.append({"device": "/dev/" + os.path.basename(tty), "suffix": suffix_of(serial),
                     "serial": serial, "product": _read(os.path.join(node, "product"))})
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    try:
        rows = from_pyserial()
    except ImportError:
        if not sys.platform.startswith("linux"):
            print("ERROR: pyserial not importable by this interpreter and no sysfs fallback "
                  "on this platform; run with esphome's python", file=sys.stderr)
            return 3
        rows = from_sysfs()

    rows.sort(key=lambda r: r["device"])
    if args.json:
        print(json.dumps(rows))
    else:
        for r in rows:
            print(f"{r['device']}\t{r['suffix']}\t{r['serial']}\t{r['product']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
