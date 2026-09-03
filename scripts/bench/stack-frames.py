#!/usr/bin/env python3
"""Per-function stack frame sizes from a built ELF, and the deepest logging path on a task.

Xtensa's windowed ABI encodes the frame size in the `entry` instruction, and call0 frames in
`addi a1, a1, -N`, so the image itself carries exact numbers -- no hardware, no run.

WHY THIS EXISTS. snap_player overflowed its 6144-byte stack seven times on 2026-09-03 with
`|<-CORRUPTED` on every backtrace, so the crash never said how deep it went. Reading the frames
off the ELF does, and it found the deepest logging path summing to 4896 bytes (80%).

THE PARSER BUG THIS FILE IS SHAPED AROUND. objdump prints most operands in decimal but some in
hex, so a `(\\d+)` pattern silently matches the `0` of `0x1a0` and reports a 416-byte frame as 0.
The first version of this scan reported "largest frame = 240 bytes" and every large frame in the
image -- including player_task_'s 2528 -- read as zero. A measurement that cannot see the values
it exists to find looks exactly like a reassuring answer. Accept BOTH formats, and treat a 0 for a
function with known locals as a parser failure rather than a result.

Usage:
    stack-frames.py <firmware.elf> [--objdump PATH] [--top N] [--match SUBSTR]...
"""
import argparse
import re
import shutil
import subprocess
import sys

LABEL = re.compile(r"^[0-9a-f]+ <(.+)>:")
# entry a1, N  |  addi a1, a1, -N  |  addi.n a1, a1, -N   -- N decimal OR 0x-hex.
FRAME = re.compile(
    r"\b(?:entry\s+a1,\s*|addi(?:\.n)?\s+a1,\s*a1,\s*-)((?:0x)?[0-9a-fA-F]+)\b"
)

DEFAULT_OBJDUMP = (
    "/Users/william/.platformio/tools/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump"
)


def frames(elf, objdump):
    dis = subprocess.run([objdump, "-d", elf], capture_output=True)
    if dis.returncode != 0:
        sys.exit(f"objdump failed: {dis.stderr.decode('utf-8', 'replace')[:300]}")
    out, cur = {}, None
    for line in dis.stdout.decode("utf-8", "replace").splitlines():
        m = LABEL.match(line)
        if m:
            cur = m.group(1)
            continue
        if cur is None:
            continue
        f = FRAME.search(line)
        if f:
            v = f.group(1)
            out.setdefault(cur, int(v, 16) if v.startswith("0x") else int(v))
            cur = None
    return out


ap = argparse.ArgumentParser()
ap.add_argument("elf")
ap.add_argument("--objdump", default=DEFAULT_OBJDUMP)
ap.add_argument("--top", type=int, default=15)
ap.add_argument("--match", action="append", default=[])
a = ap.parse_args()

if not shutil.which(a.objdump) and not subprocess.run(
    ["test", "-x", a.objdump]
).returncode == 0:
    sys.exit(f"objdump not executable: {a.objdump}")

fr = frames(a.elf, a.objdump)
if not fr:
    sys.exit("no frames parsed -- wrong architecture, or the operand format changed again")

print(f"  {len(fr)} functions with a measurable frame")
print(f"\n  largest {a.top}:")
for n, s in sorted(fr.items(), key=lambda x: -x[1])[: a.top]:
    print(f"    {s:6d}  {n[:96]}")

# The default path is the one that crashed: player task -> report -> logger -> newlib formatter.
wanted = a.match or [
    "player_task_Ev",
    "log_sync_report",
    "emit_pre_trace_line",
    "refresh_tsf_peers",
    "esp_log_printf_",
    "log_vprintf_non_main_thread",
    "_svfprintf_r",
    "_dtoa_r",
]
print("\n  requested functions (0 here means the PARSER failed, not a free function):")
total = 0
for w in wanted:
    hits = sorted([(n, s) for n, s in fr.items() if w in n], key=lambda x: -x[1])[:1]
    if not hits:
        print(f"    {'?':>6}  {w}  -- inlined or absent")
        continue
    n, s = hits[0]
    total += s
    print(f"    {s:6d}  {n[:96]}")
print(f"    {'-'*6}")
print(f"    {total:6d}  sum (a chain only if these actually call one another -- check, do not assume)")
