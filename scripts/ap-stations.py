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

Airtime share is estimated as 1/uplink + 1/downlink normalized across stations:
a first-order model of who is eating the channel, good enough to rank offenders.
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
                "up": w.get("uplinkPhyRate"),
                "down": w.get("downlinkPhyRate"),
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
        # first-order airtime model: cost ~ 1/rate, normalized within the radio
        costs = [(1 / (r["up"] or 1) + 1 / (r["down"] or 1)) for r in group]
        total = sum(costs) or 1
        slow = [r for r in group if min(r["up"] or 999, r["down"] or 999) <= SLOW_MBPS]
        print(f"\n--- {radio}: {len(group)} stations, {len(slow)} slow (<= {SLOW_MBPS} Mbps) ---")
        if brief:
            continue
        print(f"{'rssi':>5s} {'up':>5s} {'down':>5s} {'airtime':>8s}  station")
        for r, cost in zip(group, costs):
            share = 100 * cost / total
            flag = "  <-- RATE ANOMALY: taxes every station on this radio" if r in slow else ""
            print(f"{r['rssi'] if r['rssi'] is not None else '?':>5} "
                  f"{r['up'] if r['up'] is not None else '?':>5} "
                  f"{r['down'] if r['down'] is not None else '?':>5} "
                  f"{share:7.1f}%  {r['name'][:36]:36s}{flag}")
        if slow:
            worst = max(zip(group, costs), key=lambda p: p[1])[0]
            print(f"      -> biggest airtime consumer: {worst['name']} "
                  f"({worst['up']}/{worst['down']} Mbps at {worst['rssi']} dBm)")


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
    args = ap.parse_args()

    if args.from_json:
        report(stations(json.load(open(args.from_json))), args.brief)
        return

    session = args.session or login(args.host, *(args.login or ("", "")))

    while True:
        try:
            try:
                topo = ubus(args.host, session, "data_repo.webinfo", "get_topology")
            except RuntimeError:
                # Sessions expire after 300 s; re-login once and retry
                session = login(args.host, *(args.login or ("", "")))
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
