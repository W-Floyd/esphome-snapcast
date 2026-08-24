#!/usr/bin/env python3
"""Per-station signal and PHY rates from a GFiber (ubus/OpenWrt-based) router.

Why this exists: speaker starvations turned out to be airtime contention, and the
decisive evidence is per-station PHY rates -- not RSSI. 802.11's DCF gives each
station roughly equal *transmit opportunities*, not equal airtime, so one station
negotiating 1 Mbps occupies the channel ~50x longer per byte than one at 54 Mbps
and drags the whole cell down with it (the classic "performance anomaly of
802.11b", Heusse et al. 2003). A cell can therefore look fine on RSSI and still
stall audio.

API (reverse-engineered from the router's own web UI):
    POST https://<router>/ubus        (JSON-RPC 2.0, self-signed TLS)
    {"jsonrpc":"2.0","id":"1","method":"call",
     "params":["<session>", "data_repo.webinfo", "get_topology", {}]}

Auth: the LAN UI grants a session for an ANONYMOUS login -- blank username and
password against the null session -- so no credentials are needed and sessions
(300 s timeout) are obtained automatically:
    params: ["00000000000000000000000000000000","session","login",
             {"username":"","password":""}]  -> result.ubus_rpc_session

    python3 scripts/ap-stations.py                       # auto-login
    python3 scripts/ap-stations.py --watch 10            # correlate with speaker logs
    python3 scripts/ap-stations.py --from-json topo.json # offline
    python3 scripts/ap-stations.py --session <token>     # reuse a UI session

The "cost/byte" column is 1/uplink + 1/downlink normalized across the radio: it
ranks how expensive each station's traffic is *per byte*, NOT its total airtime.
A slow station only actually hurts if it transmits often -- a 1 Mbps sensor
sending a packet a minute is harmless, while a 1 Mbps device streaming audio or
BLE-proxy telemetry is ruinous. This router's Data Elements counters
(network.wireless.get_DataElements) do expose byte/packet totals but populate
utilization inconsistently, so weigh a flagged station by whether it plausibly
carries continuous traffic before chasing it.
"""

import argparse
import json
import ssl
import sys
import time
import urllib.request

NULL_SESSION = "0" * 32
# HT20 1-stream tops out ~72 Mbps; anything at/below this is legacy-rate slow
SLOW_MBPS = 6


def ubus(host, session, obj, func, args=None):
    payload = json.dumps({
        "jsonrpc": "2.0", "id": "1", "method": "call",
        "params": [session, obj, func, args or {}],
    }).encode()
    req = urllib.request.Request(f"https://{host}/ubus", data=payload,
                                headers={"Content-Type": "application/json",
                                         "Referer": f"https://{host}/",
                                         "Origin": f"https://{host}"})
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with urllib.request.urlopen(req, context=ctx, timeout=10) as r:
        body = json.load(r)
    if "error" in body:
        raise RuntimeError(f"ubus error: {body['error']}")
    result = body.get("result")
    if isinstance(result, list):
        if result[0] != 0:
            raise RuntimeError(f"ubus status {result[0]} (expired session? re-grab it)")
        return result[1] if len(result) > 1 else {}
    return result


def login(host, username="", password=""):
    """Anonymous by default: the LAN UI itself logs in with blank credentials."""
    res = ubus(host, NULL_SESSION, "session", "login",
               {"username": username, "password": password})
    token = res.get("ubus_rpc_session")
    if not token:
        raise RuntimeError(f"login returned no session: {res}")
    return token


def stations(topology):
    """Flatten the topology tree into wireless station rows."""
    found, out = [], []

    def walk(o):
        if isinstance(o, dict):
            if o.get("macAddress") and o.get("connections"):
                found.append(o)
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(topology)
    for d in found:
        for c in d.get("connections", []):
            w = c.get("wireless")
            if not w:
                continue
            out.append({
                "radio": w.get("radio", "?"),
                "rssi": w.get("rssi"),
                # get_topology's uplink/downlink labels are INVERTED relative to
                # the Wi-Fi Data Elements spec that the same router serves from
                # data_repo.data_element: for one station sampled 5x in the same
                # minute, get_topology said uplink=39/downlink=1 while
                # data_element said Downlink=39/Uplink=1. Data Elements is a
                # published spec (uplink = STA->AP) and get_topology is vendor UI
                # glue, so trust the spec and swap here. cost/byte is
                # direction-symmetric, so only the labels were ever affected.
                "up": w.get("downlinkPhyRate"),
                "down": w.get("uplinkPhyRate"),
                "name": d.get("hostname") or d["macAddress"],
                "mac": d["macAddress"],
                "uptime": c.get("uptime"),
                "upstream": d.get("upstreamId", ""),
            })
    return out


def report(rows, brief=False):
    by_radio = {}
    for r in rows:
        by_radio.setdefault(r["radio"], []).append(r)

    for radio in sorted(by_radio, key=lambda x: ("2.4" not in x, x)):
        group = sorted(by_radio[radio], key=lambda r: (r["up"] or 0))
        # cost PER BYTE ~ 1/rate, normalized within the radio (not total airtime)
        costs = [(1 / (r["up"] or 1) + 1 / (r["down"] or 1)) for r in group]
        total = sum(costs) or 1
        slow = [r for r in group if min(r["up"] or 999, r["down"] or 999) <= SLOW_MBPS]
        print(f"\n--- {radio}: {len(group)} stations, {len(slow)} slow (<= {SLOW_MBPS} Mbps) ---")
        if brief:
            continue
        print(f"{'rssi':>5s} {'up':>5s} {'down':>5s} {'cost/byte':>10s}  station")
        for r, cost in zip(group, costs):
            share = 100 * cost / total
            flag = "  <-- legacy rates: costly IF it carries real traffic" if r in slow else ""
            print(f"{r['rssi'] if r['rssi'] is not None else '?':>5} "
                  f"{r['up'] if r['up'] is not None else '?':>5} "
                  f"{r['down'] if r['down'] is not None else '?':>5} "
                  f"{share:9.1f}%  {r['name'][:36]:36s}{flag}")
        if slow:
            worst = max(zip(group, costs), key=lambda p: p[1])[0]
            print(f"      -> most expensive per byte: {worst['name']} "
                  f"({worst['up']}/{worst['down']} Mbps at {worst['rssi']} dBm) "
                  f"-- confirm it actually transmits before acting")


# --- congestion -------------------------------------------------------------
# Airtime is what congests a cell, and this router cannot report it: the Data
# Elements UtilizationTransmit/Receive fields are a stub (six stations reported
# an identical 120000 delta over 61 s while the only station actually sending
# reported 0), BytesSent/BytesReceived are mutually inconsistent with the packet
# counts, and the per-radio Channel/Utilization survey is a cached off-channel
# scan that can be byte-identical across a 40-minute gap.
#
# What does survive cross-checking is PacketsSent/PacketsReceived, RetransCount
# and the PHY rates. So estimate airtime the honest way: measure the packet rate
# over a real interval and price each packet at that station's own PHY rate.
# That is a MODEL, not a measurement -- it assumes a mean frame size and a fixed
# per-frame overhead -- but it is directional, and it is the only thing here that
# separates a slow station that transmits from one that merely idles.
FRAME_BYTES = 800         # assumed mean frame; shifts absolute ms/s, not ranking
FRAME_OVERHEAD_US = 120   # preamble + DIFS + SIFS + ACK, roughly, per frame


def data_elements(payload):
    """Flatten get_DataElements into (radio_rows, sta_rows)."""
    radios, stas = [], []
    for dev in payload.get("Network", {}).get("Device", []):
        for radio in dev.get("Radio", []):
            chan = None
            for prof in radio.get("CurrentOperatingClassProfile", []):
                chan = prof.get("Channel", chan)
            scan_util, neighbors = None, 0
            for sr in radio.get("ScanResult", []):
                for oc in sr.get("OpClassScan", []):
                    for cs in oc.get("ChannelScan", []):
                        if cs.get("Channel") == chan:
                            scan_util = cs.get("Utilization")
                        neighbors += cs.get("NeighborBSSNumberOfEntries", 0)
            radios.append({"id": radio.get("ID"), "channel": chan,
                           # Radio.Utilization is a LIVE busy measurement: it
                           # moved 38/44/37/37 over four 20 s samples while the
                           # ScanResult table below stayed frozen at 58. Scale is
                           # ambiguous (0-255 per BSS-Load, or already percent),
                           # so it is reported raw -- trend and ranking are sound,
                           # the absolute figure is not.
                           "util": radio.get("Utilization"),
                           "scan_util": scan_util, "neighbors": neighbors,
                           "device": dev.get("ID"), "noise_raw": radio.get("Noise")})
            for bss in radio.get("BSS", []):
                for sta in bss.get("STA", []):
                    sig = sta.get("SignalStrength")
                    stas.append({
                        "radio": radio.get("ID"),
                        "channel": chan,
                        "mac": (sta.get("MACAddress") or "").lower(),
                        # SignalStrength is RCPI: dBm = value/2 - 110, verified
                        # against get_topology's rssi for the same station.
                        "rssi": (sig / 2 - 110) if sig is not None else None,
                        "up_rate": sta.get("LastDataUplinkRate"),
                        "down_rate": sta.get("LastDataDownlinkRate"),
                        "pkts_sent": sta.get("PacketsSent"),      # AP -> STA
                        "pkts_recv": sta.get("PacketsReceived"),  # STA -> AP
                        "retrans": sta.get("RetransCount"),
                    })
    return radios, stas


def airtime_ms(packets, rate_mbps):
    """Modelled airtime for `packets` frames at `rate_mbps`, in milliseconds."""
    if not packets or not rate_mbps:
        return 0.0
    return packets * ((FRAME_BYTES * 8) / rate_mbps + FRAME_OVERHEAD_US) / 1000.0


def congestion(host, session_box, login_args, seconds, names):
    def fetch():
        try:
            return ubus(host, session_box[0], "network.wireless", "get_DataElements")
        except RuntimeError:
            session_box[0] = login(host, *login_args)
            return ubus(host, session_box[0], "network.wireless", "get_DataElements")

    r0, s0 = data_elements(fetch())
    t0 = time.time()
    print("sampling %.0fs of traffic ..." % seconds, file=sys.stderr)
    time.sleep(seconds)
    r1, s1 = data_elements(fetch())
    elapsed = time.time() - t0

    before = {s["mac"]: s for s in s0}
    rows = []
    for s in s1:
        b = before.get(s["mac"])
        if not b:
            continue   # associated mid-window: no baseline, so no rate
        d_sent = (s["pkts_sent"] or 0) - (b["pkts_sent"] or 0)
        d_recv = (s["pkts_recv"] or 0) - (b["pkts_recv"] or 0)
        d_retr = (s["retrans"] or 0) - (b["retrans"] or 0)
        if d_sent < 0 or d_recv < 0 or d_retr < 0:
            continue   # counter reset (reassociation): the delta is garbage
        ms = airtime_ms(d_sent, s["down_rate"]) + airtime_ms(d_recv, s["up_rate"])
        rows.append({**s, "d_sent": d_sent, "d_recv": d_recv, "d_retr": d_retr,
                     "pps": (d_sent + d_recv) / elapsed,
                     "air": 100 * ms / (elapsed * 1000),
                     "name": names.get(s["mac"], s["mac"])})

    was = {r["id"]: r for r in r0}
    surveys = {r["id"]: r for r in r1}
    by_radio = {r["id"]: r for r in r1}
    groups = {}
    for r in rows:
        groups.setdefault(r["radio"], []).append(r)
    for rid in by_radio:          # radios with no transmitting station still count
        groups.setdefault(rid, [])

    print("\n===== congestion over %.0fs (%s) =====" % (elapsed, time.strftime("%H:%M:%S")))
    for rid in sorted(groups, key=lambda k: (by_radio.get(k, {}).get("channel") or 0, k)):
        group = sorted(groups[rid], key=lambda r: -r["air"])
        # A zero-packet row means "not counted", not "idle": rank only real rows.
        active = [r for r in group if r["d_sent"] or r["d_recv"]]
        total = sum(r["air"] for r in active)
        srv, prev = surveys.get(rid, {}), was.get(rid, {})
        stale = srv.get("scan_util") is not None and srv.get("scan_util") == prev.get("scan_util")
        print("\n--- radio %s  channel %s: %d associated, %d transmitting, "
              "~%.1f%% modelled airtime ---"
              % (rid, srv.get("channel"), len(group), len(active), total))
        print("    live Radio.Utilization: %s -> %s (raw; moves, so it is real)"
              % (prev.get("util"), srv.get("util")))
        if srv.get("scan_util") is not None:
            print("    cached scan survey: utilization=%s, neighbor BSS=%s%s"
                  % (srv.get("scan_util"), srv.get("neighbors"),
                     "  [identical in both samples: frozen, ignore]" if stale else ""))
        if not active:
            print("    no station reported a packet delta")
            continue
        print("%6s %7s %6s %5s %5s %5s  station" % ("air%", "pkt/s", "retx%", "rssi", "up", "down"))
        for r in active:
            denom = r["d_sent"] + r["d_retr"]
            retx = 100 * r["d_retr"] / denom if denom else 0.0
            flag = ""
            if r["air"] >= 5 and min(r["up_rate"] or 999, r["down_rate"] or 999) <= 24:
                flag = "  <-- slow AND busy: this is what costs the cell"
            elif retx >= 20:
                flag = "  <-- heavy retransmission"
            print("%6.2f %7.1f %6.1f %5s %5s %5s  %-32s%s"
                  % (r["air"], r["pps"], retx,
                     "?" if r["rssi"] is None else round(r["rssi"]),
                     r["up_rate"] or "?", r["down_rate"] or "?", r["name"][:32], flag))
        idle = len(group) - len(active)
        if idle:
            print("    (%d associated station(s) reported no packets: absent from this "
                  "ranking, not proven idle)" % idle)
    print("\nairtime is MODELLED -- packet delta priced at each station's PHY rate, "
          "assuming\n%dB frames + %dus overhead. It excludes other BSSes, non-Wi-Fi "
          "interference\nand management traffic, so read the total as a floor for this "
          "cell, not the\nchannel's true busy fraction." % (FRAME_BYTES, FRAME_OVERHEAD_US))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.1.1")
    ap.add_argument("--session", help="reuse a ubus session token instead of logging in")
    ap.add_argument("--login", nargs=2, metavar=("USER", "PASS"),
                    help="credentials, if this router's UI needs them (default: anonymous)")
    ap.add_argument("--from-json", help="parse a saved get_topology result instead of querying")
    ap.add_argument("--watch", type=float, metavar="SECONDS",
                    help="poll forever at this interval, timestamped for log correlation")
    ap.add_argument("--brief", action="store_true", help="counts only (useful with --watch)")
    ap.add_argument("--congestion", type=float, nargs="?", const=60, metavar="SECONDS",
                    help="sample a packet-rate window and rank stations by modelled airtime")
    args = ap.parse_args()

    if args.from_json:
        report(stations(json.load(open(args.from_json))), args.brief)
        return

    login_args = tuple(args.login or ("", ""))
    session = args.session or login(args.host, *login_args)

    if args.congestion:
        box = [session]
        try:   # hostnames live in get_topology, not in Data Elements
            topo = ubus(args.host, box[0], "data_repo.webinfo", "get_topology")
            names = {s["mac"].lower(): s["name"] for s in stations(topo)}
        except Exception:
            names = {}
        congestion(args.host, box, login_args, args.congestion, names)
        return

    while True:
        try:
            try:
                topo = ubus(args.host, session, "data_repo.webinfo", "get_topology")
            except RuntimeError:
                # Sessions expire after 300 s; re-login once and retry
                session = login(args.host, *login_args)
                topo = ubus(args.host, session, "data_repo.webinfo", "get_topology")
            print(f"\n===== {time.strftime('%H:%M:%S')} =====")
            report(stations(topo), args.brief)
        except Exception as exc:  # keep a --watch session alive across hiccups
            print(f"{time.strftime('%H:%M:%S')}  query failed: {exc}", file=sys.stderr)
            if not args.watch:
                sys.exit(1)
        if not args.watch:
            break
        time.sleep(args.watch)


if __name__ == "__main__":
    main()
