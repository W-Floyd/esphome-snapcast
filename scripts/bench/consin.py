#!/usr/bin/env python3
"""Grade CONSIN lines: which contributor moved the timebase, and by how much.

    scripts/bench/consin.py a.log [b.log ...]

Written against the real line shapes BEFORE the firmware emitted any, including the pathological
ones: `expected=n/a` (no mapping held), a truncated line, and a line whose fields are absent
entirely. A parser that requires a trailing field drops the whole record when the line is cut,
which turns a formatting limit into silent data loss -- that has happened on this bench.
"""
import re
import sys
from collections import defaultdict

# NOTHING TRAILING IS REQUIRED. Each field is looked up independently, so a line cut mid-token
# still yields every field that did fit.
WHY = re.compile(r'CONSIN (\w+) ')
IDX = re.compile(r'CONSIN \w+ (\d+)/(\d+) (SELF|peer):([0-9A-Fa-f]{4})')
FIELD = {
    'tsf_base': re.compile(r'tsf_base=(-?\d+)'),
    'tms_base': re.compile(r'tms_base=(-?\d+)'),
    'drift':    re.compile(r'drift=([+-][\d.]+)'),
    'tms_ref':  re.compile(r'tms@ref=(-?\d+)'),
    'dev':      re.compile(r'dev=([+-]?\d+)'),
    'ref_tsf':  re.compile(r'ref_tsf=(-?\d+)'),
    'adopt':    re.compile(r'adopt=(-?\d+)'),
    'held':     re.compile(r'held=(\d)'),
    'held_tms': re.compile(r'held_tms=(-?\d+)'),
    'expected': re.compile(r'expected=(n/a|-?\d+)'),
}
ANSI = re.compile(r'\x1b\[[0-9;]*m')


def parse(path):
    per_peer = defaultdict(list)
    events = []
    truncated = 0
    for raw in open(path, 'rb'):
        line = ANSI.sub('', raw.decode('utf8', 'replace'))
        if 'CONSIN' not in line:
            continue
        why = WHY.search(line)
        if not why:
            truncated += 1
            continue
        m = IDX.search(line)
        got = {k: r.search(line) for k, r in FIELD.items()}
        if m:
            dev = got['dev']
            per_peer[f"{m.group(3)}:{m.group(4)}"].append(
                (int(dev.group(1)) if dev else None,
                 int(got['tms_base'].group(1)) if got['tms_base'] else None))
        elif got['adopt']:
            events.append((why.group(1),
                           got['expected'].group(1) if got['expected'] else None,
                           int(got['adopt'].group(1)),
                           got['held'].group(1) if got['held'] else None))
    return per_peer, events, truncated


for path in sys.argv[1:]:
    per_peer, events, truncated = parse(path)
    print(f"=== {path}: {len(events)} CONSIN events, "
          f"{sum(len(v) for v in per_peer.values())} contributor lines, "
          f"{truncated} unparsable ===")
    if not per_peer:
        print("  no CONSIN lines yet (they fire only on a timebase step, or on a throttled "
              "single-contributor adoption)")
        continue
    for who, vals in sorted(per_peer.items(), key=lambda kv: -len(kv[1])):
        devs = sorted(abs(d) for d, _ in vals if d is not None)
        if not devs:
            print(f"  {who}: {len(vals)} lines, no dev field survived")
            continue
        print(f"  {who}: n={len(devs):4d}  |dev| median {devs[len(devs)//2]:>12d} us  "
              f"p90 {devs[int(.9 * len(devs))]:>13d} us  max {devs[-1]:>14d} us")
    solo = sum(1 for w, _, _, _ in events if w == 'solo')
    noheld = sum(1 for _, e, _, _ in events if e == 'n/a')
    print(f"  events: {solo} solo (single contributor adopted verbatim), "
          f"{len(events) - solo} step, {noheld} with no mapping held")
