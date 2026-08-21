#!/usr/bin/env python3
"""Diagnose snapclient speaker failures from captured logs.

Encodes the failure taxonomy learned in the field. Feed it one log per device
(captured on ONE machine so timestamps share a clock):

    python3 scripts/diagnose-logs.py a.log b.log c.log d.log

It extracts events (starvations, hard resyncs, pipeline wedges, reconnects,
stream-idle, OTA, TSF role churn, ring low-water, accounting anomalies), groups
them into per-device incidents, correlates incidents across the fleet, and
classifies each into a failure plane:

  SERVER      simultaneous events on most devices (tight window): the server
              dropped connections or stopped producing chunks
  CONGESTION  staggered starvations across several devices: shared-channel
              airtime contention (weak peer retransmissions, OTA, other traffic)
  DEVICE      isolated to one device: its radio/antenna/placement
  OTA         within the shadow of a firmware update
  WEDGE       pipeline refusing audio: speaker-framework race (upstream bug)
  ACCOUNTING  pipeline depth impossible/pinned low: playout accounting split

Ends with a per-device scorecard and prioritized recommendations.
"""

import argparse
import os
import re
import statistics
import sys
from collections import defaultdict

TS = re.compile(r"\[(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?\]")
SYNC = re.compile(
    r"median (-?\d+) us .*?corrected -(\d+)/\+(\d+) frames, (\d+) hard resyncs.*?"
    r"buffered (\d+) ms(?:, pipeline (-?\d+) ms)?"
)
RESYNC = re.compile(r"Hard resync: (\d+) ms (late|early)")
RSSI = re.compile(r"rssi=(-\d+) dBm")

# Event kinds worth an incident (line-substring -> kind)
MARKERS = [
    ("re-baselining playout", "starve"),
    ("refusing audio", "wedge"),
    ("Connected to ", "reconnect"),
    ("Stream idle for", "stream_idle"),
    ("Starting update from", "ota_start"),
    ("Update complete", "ota_done"),
    ("Assuming TSF leadership", "tsf_lead"),
    ("Yielding leadership", "tsf_yield"),
    ("Sync locked", "lock"),
]

RING_LOW_MS = 400        # ring below this = delivery visibly stalling
PIPELINE_LOW_MS = 100    # accounted pipeline below this (sustained) = suspicious
INCIDENT_GAP_S = 12      # events closer than this merge into one incident
SIMULTANEOUS_S = 4       # spread within this across devices = server plane
CLUSTER_S = 30           # loose cross-device clustering = congestion plane
OTA_SHADOW_S = 90        # events this long after any OTA = OTA-attributed
UPSTREAM_FREEZE_MS = 1000  # resync lateness beyond this = multi-second freeze


def t_of(line):
    m = TS.search(line)
    if not m:
        return None
    h, mi, s, ms = m.groups()
    return int(h) * 3600 + int(mi) * 60 + int(s) + (int(ms.ljust(3, "0")) / 1000 if ms else 0)


def parse(path):
    """-> events [(t, kind, detail)], sync series, rssi series."""
    events, sync, rssi = [], [], []
    for line in open(path, errors="replace"):
        t = t_of(line)
        if t is None:
            continue
        for needle, kind in MARKERS:
            if needle in line:
                events.append((t, kind, ""))
                break
        m = RESYNC.search(line)
        if m:
            events.append((t, "resync", f"{m.group(1)}ms {m.group(2)}"))
        m = SYNC.search(line)
        if m:
            median, drop, ins, resyncs, buf, pipe = m.groups()
            pipe = int(pipe) if pipe is not None else None
            sync.append((t, int(median), int(drop), int(ins), int(resyncs), int(buf), pipe))
            if int(buf) < RING_LOW_MS:
                events.append((t, "ring_low", f"{buf}ms"))
            if pipe is not None and pipe < 0:
                events.append((t, "pipeline_neg", f"{pipe}ms"))
        m = RSSI.search(line)
        if m:
            rssi.append(int(m.group(1)))
    events.sort()
    return events, sync, rssi


def build_incidents(events):
    """Merge a device's events into incidents: (t_start, t_end, kinds, details)."""
    incidents = []
    for t, kind, detail in events:
        if kind in ("lock", "tsf_lead", "tsf_yield", "ota_done"):
            continue  # context, not failures
        if incidents and t - incidents[-1][1] <= INCIDENT_GAP_S:
            s, _e, kinds, details = incidents[-1]
            kinds.add(kind)
            if detail:
                details.append(detail)
            incidents[-1] = (s, t, kinds, details)
        else:
            incidents.append((t, t, {kind}, [detail] if detail else []))
    return incidents


def max_lateness_ms(details):
    worst = 0
    for d in details:
        m = re.match(r"(\d+)ms late", d)
        if m:
            worst = max(worst, int(m.group(1)))
    return worst


def hms(t):
    return f"{int(t // 3600):02d}:{int(t % 3600 // 60):02d}:{t % 60:06.3f}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    args = ap.parse_args()
    names = [os.path.splitext(os.path.basename(p))[0] for p in args.logs]

    per_dev = {}
    ota_times = []
    for name, path in zip(names, args.logs):
        events, sync, rssi = parse(path)
        per_dev[name] = {"events": events, "sync": sync, "rssi": rssi,
                         "incidents": build_incidents(events)}
        ota_times += [t for t, k, _ in events if k == "ota_start"]

    # ── Correlate incidents across devices into fleet incidents ────────────────
    all_inc = sorted(
        (start, end, name, kinds, details)
        for name, d in per_dev.items()
        for (start, end, kinds, details) in d["incidents"]
    )
    fleet = []  # groups of per-device incidents
    for inc in all_inc:
        if fleet and inc[0] - max(e[1] for e in fleet[-1]) <= CLUSTER_S:
            fleet[-1].append(inc)
        else:
            fleet.append([inc])

    print(f"{'=' * 74}\nFLEET INCIDENTS ({len(fleet)})\n{'=' * 74}")
    counts = defaultdict(int)
    for group in fleet:
        devs = sorted({g[2] for g in group})
        t0, t1 = min(g[0] for g in group), max(g[1] for g in group)
        spread = t1 - t0
        kinds = set().union(*(g[3] for g in group))
        worst_late = max(max_lateness_ms(g[4]) for g in group)

        # Classification (order matters: most specific first)
        if "wedge" in kinds:
            cls = "WEDGE (framework race: mixer stopped draining -- upstream bug)"
        elif "pipeline_neg" in kinds:
            cls = "ACCOUNTING (impossible pipeline depth -- report this, it should be fixed)"
        elif ota_times and any(0 <= t0 - ot <= OTA_SHADOW_S for ot in ota_times):
            cls = "OTA (within a firmware-update shadow; expected disturbance)"
        elif len(devs) >= 2 and ("reconnect" in kinds or "stream_idle" in kinds) and spread <= SIMULTANEOUS_S:
            cls = ("UPSTREAM (simultaneous fleet-wide drop: snapserver host OR the AP's radio -- "
                   "probe the server from another band/wire to split them)")
        elif len(devs) >= 2 and worst_late >= UPSTREAM_FREEZE_MS:
            cls = ("UPSTREAM (multi-second delivery freeze on several devices: snapserver host OR the "
                   "AP's 2.4 GHz radio stalling, e.g. background channel scans -- a clean probe of the "
                   "server from another band/wire during an event exonerates the host)")
        elif len(devs) >= 2:
            cls = "CONGESTION (staggered starvations across devices: shared-channel airtime)"
        else:
            cls = f"DEVICE ({devs[0]}: isolated -- its radio/antenna/placement)"
        counts[cls.split(" ")[0]] += 1

        detail_bits = []
        if "starve" in kinds:
            detail_bits.append("starved")
        if "resync" in kinds:
            detail_bits.append(f"resyncs (worst {worst_late} ms late)" if worst_late else "resyncs")
        if "ring_low" in kinds:
            detail_bits.append("ring drained")
        if "reconnect" in kinds:
            detail_bits.append("TCP reconnect")
        if "stream_idle" in kinds:
            detail_bits.append("stream idle")
        print(f"{hms(t0)}  devices [{', '.join(devs)}]  spread {spread:5.1f}s  {', '.join(detail_bits)}")
        print(f"           -> {cls}")

    # ── Per-device scorecard ────────────────────────────────────────────────────
    print(f"\n{'=' * 74}\nDEVICE SCORECARD\n{'=' * 74}")
    print(f"{'dev':>6s} {'span':>7s} {'starves':>8s} {'wedges':>7s} {'resyncs':>8s} "
          f"{'ring<' + str(RING_LOW_MS):>9s} {'rssi avg':>9s} {'median std':>11s}")
    for name, d in per_dev.items():
        ev = d["events"]
        span_h = (ev[-1][0] - ev[0][0]) / 3600 if len(ev) > 1 else 0
        n = lambda k: sum(1 for _, kind, _ in ev if kind == k)
        rssi = f"{statistics.fmean(d['rssi']):.0f}" if d["rssi"] else "--"
        healthy = [m for _, m, dr, ins, rs, _, _ in d["sync"] if dr == 0 and ins == 0 and rs == 0]
        med_std = f"{statistics.pstdev(healthy):.0f} us" if len(healthy) > 2 else "--"
        print(f"{name:>6s} {span_h:6.2f}h {n('starve'):8d} {n('wedge'):7d} {n('resync'):8d} "
              f"{n('ring_low'):9d} {rssi:>9s} {med_std:>11s}")

    # ── Recommendations ────────────────────────────────────────────────────────
    print(f"\n{'=' * 74}\nRECOMMENDATIONS (prioritized)\n{'=' * 74}")
    if counts.get("UPSTREAM"):
        print(f"* {counts['UPSTREAM']} upstream incident(s): either the snapserver host or the AP's"
              "\n  2.4 GHz radio (fleet-wide from the ESP view either way). Split them by probing the"
              "\n  server from another band/wire DURING an event: clean probe -> AP radio (check its"
              "\n  background-scan / auto-channel / RRM settings); slow probe -> server host"
              "\n  (journalctl -u snapserver, host load, the source feeding it).")
    if counts.get("DEVICE"):
        dev_names = defaultdict(int)
        for group in fleet:
            devs = {g[2] for g in group}
            kinds = set().union(*(g[3] for g in group))
            if len(devs) == 1 and "wedge" not in kinds and "pipeline_neg" not in kinds:
                dev_names[next(iter(devs))] += 1
        worst = max(dev_names, key=dev_names.get, default=None)
        if worst:
            print(f"* Isolated incidents concentrate on '{worst}' ({dev_names[worst]}x): suspect its"
                  "\n  radio -- swap positions with a healthy sibling; if trouble follows the device,"
                  "\n  the antenna is bad. A weak receiver also taxes the whole channel (low-rate"
                  "\n  retransmissions), so fixing it can resolve CONGESTION incidents too.")
    if counts.get("CONGESTION"):
        print(f"* {counts['CONGESTION']} congestion incident(s): shared 2.4 GHz airtime. Levers:"
              "\n  fix any weak-antenna device, reduce concurrent log streams, avoid parallel OTA"
              "\n  during playback, and (server-side, your call) a larger stream buffer.")
    if counts.get("WEDGE"):
        print("* WEDGE incidents are the ESPHome mixer/speaker race -- the client survives them"
              "\n  (bounded push loop) but the root cause belongs upstream; keep these logs.")
    if counts.get("ACCOUNTING"):
        print("* ACCOUNTING incidents (negative pipeline) should be impossible on current builds --"
              "\n  if the firmware is current, report with the log.")
    if counts.get("OTA"):
        print(f"* {counts['OTA']} incident(s) in OTA shadows: expected; flash during silence to avoid.")
    if not counts:
        print("* No failure incidents found. Fleet looks healthy.")


if __name__ == "__main__":
    main()
