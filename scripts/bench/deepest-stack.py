#!/usr/bin/env python3
"""Deepest reachable stack path from a root function, from an xtensa ELF disassembly.

Frame sizes come from `entry a1, N` / `addi a1, a1, -N` -- N in DECIMAL OR HEX, because objdump
prints both and a \\d+ pattern silently reads 0x1a0 as 0.

Call edges come from call0/call4/call8/call12 with a resolved <symbol>. callx* is an INDIRECT call
and cannot be resolved statically, so any path through a vtable, a std::function or a callback is
INVISIBLE here -- the number this prints is a lower bound on the true worst case, and functions
whose deepest edge is indirect are listed separately so the gap is explicit rather than assumed.
"""
import re
import subprocess
import sys

LABEL = re.compile(r"^[0-9a-f]+ <(.+)>:")
FRAME = re.compile(r"\b(?:entry\s+a1,\s*|addi(?:\.n)?\s+a1,\s*a1,\s*-)((?:0x)?[0-9a-fA-F]+)\b")
# objdump prints the target WITHOUT a 0x prefix here ("call0 40376744 <sym>"), so requiring one
# matched nothing at all and the first run reported "0 direct edges" -- an empty graph that
# printed a tidy 2528-byte answer instead of an error.
CALL = re.compile(r"\bcall(?:0|4|8|12)\s+(?:0x)?[0-9a-f]+\s*<([^>]+)>")
CALLX = re.compile(r"\bcallx(?:0|4|8|12)\b")

elf, root = sys.argv[1], sys.argv[2]
objdump = (
    sys.argv[3]
    if len(sys.argv) > 3
    else "/Users/william/.platformio/tools/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump"
)

dis = subprocess.run([objdump, "-d", elf], capture_output=True).stdout.decode("utf-8", "replace")

frames, calls, indirect = {}, {}, set()
cur = None
for line in dis.splitlines():
    m = LABEL.match(line)
    if m:
        cur = m.group(1)
        frames.setdefault(cur, 0)
        calls.setdefault(cur, set())
        continue
    if cur is None:
        continue
    f = FRAME.search(line)
    if f and frames.get(cur, 0) == 0:
        v = f.group(1)
        frames[cur] = int(v, 16) if v.startswith("0x") else int(v)
    c = CALL.search(line)
    if c:
        calls[cur].add(c.group(1))
    if CALLX.search(line):
        indirect.add(cur)

roots = [n for n in frames if root in n]
if not roots:
    sys.exit(f"root {root!r} not found")
start = max(roots, key=lambda n: frames[n])
print(f"  root: {start}  (frame {frames[start]})")
print(f"  {len(frames)} functions, {sum(len(v) for v in calls.values())} direct edges, "
      f"{len(indirect)} functions containing an indirect call\n")

memo, onstack = {}, set()


def deepest(fn):
    """(depth, path) for the deepest DIRECT-call chain from fn. Cycles are cut, not followed."""
    if fn in memo:
        return memo[fn]
    if fn in onstack:            # recursion: charge one frame and stop
        return frames.get(fn, 0), [fn + " (recursive, cut)"]
    onstack.add(fn)
    best_d, best_p = 0, []
    for callee in calls.get(fn, ()):
        if callee not in frames:
            continue
        d, p = deepest(callee)
        if d > best_d:
            best_d, best_p = d, p
    onstack.discard(fn)
    out = (frames.get(fn, 0) + best_d, [fn] + best_p)
    memo[fn] = out
    return out


total, path = deepest(start)
print(f"  DEEPEST DIRECT CHAIN: {total} bytes")
run = 0
for fn in path:
    sz = frames.get(fn.split(" (")[0], 0)
    run += sz
    mark = "  <-- has an indirect call, real depth may exceed this" if fn in indirect else ""
    print(f"    {sz:6d}  (cum {run:6d})  {fn[:88]}{mark}")
