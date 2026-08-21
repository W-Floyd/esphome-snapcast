#!/usr/bin/env python3
"""Bidirectional volume/mute propagation test.

Direction 1 (server -> HA): change volume/mute via snapserver JSON-RPC, assert the
ESPHome media player entity state (via native API, as HA sees it) follows.
Direction 2 (HA -> server): send volume/mute media player commands via the native
API, assert snapserver's client config follows (via ClientInfo).
"""
import asyncio
import json
import socket

from aioesphomeapi import APIClient, MediaPlayerEntityState, MediaPlayerCommand

CLIENT_ID = "00:00:00:00:00:00"

def snap_rpc(method, params=None, req_id=1):
    msg = {"id": req_id, "jsonrpc": "2.0", "method": method}
    if params:
        msg["params"] = params
    with socket.create_connection(("127.0.0.1", 1705), timeout=3) as s:
        s.sendall((json.dumps(msg) + "\r\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            buf += s.recv(4096)
    return json.loads(buf.decode().splitlines()[0])

def snap_client_volume():
    st = snap_rpc("Server.GetStatus")
    for g in st["result"]["server"]["groups"]:
        for c in g["clients"]:
            if c["id"] == CLIENT_ID:
                return c["config"]["volume"]
    return None

async def main():
    client = APIClient("127.0.0.1", 6053, None)
    await client.connect(login=True)
    entities = await client.list_entities_services()
    mp = next(e for e in entities[0] if e.object_id.endswith("media_player") or "media" in e.object_id)
    print(f"media player entity: {mp.object_id} (key {mp.key})")

    state = {}
    ev = asyncio.Event()
    def on_state(s):
        if isinstance(s, MediaPlayerEntityState) and s.key == mp.key:
            state["volume"] = s.volume
            state["muted"] = s.muted
            ev.set()
    client.subscribe_states(on_state)
    await asyncio.sleep(2)
    print(f"initial entity state: volume={state.get('volume')}, muted={state.get('muted')}")

    async def wait_for(pred, what, timeout=8.0):
        deadline = asyncio.get_event_loop().time() + timeout
        while asyncio.get_event_loop().time() < deadline:
            if pred():
                print(f"  PASS: {what} (volume={state.get('volume')}, muted={state.get('muted')})")
                return True
            ev.clear()
            try:
                await asyncio.wait_for(ev.wait(), 0.5)
            except asyncio.TimeoutError:
                pass
        print(f"  FAIL: {what} (volume={state.get('volume')}, muted={state.get('muted')})")
        return False

    ok = True
    print("\n--- Direction 1: server -> entity ---")
    snap_rpc("Client.SetVolume", {"id": CLIENT_ID, "volume": {"percent": 55, "muted": False}}, 10)
    ok &= await wait_for(lambda: abs(state.get("volume", -1) - 0.55) < 0.02 and state.get("muted") is False,
                         "server volume 55 -> entity 0.55")

    snap_rpc("Client.SetVolume", {"id": CLIENT_ID, "volume": {"percent": 55, "muted": True}}, 11)
    ok &= await wait_for(lambda: state.get("muted") is True, "server mute-only -> entity muted")

    snap_rpc("Client.SetVolume", {"id": CLIENT_ID, "volume": {"percent": 55, "muted": False}}, 12)
    ok &= await wait_for(lambda: state.get("muted") is False, "server unmute-only -> entity unmuted")

    print("\n--- Direction 2: entity -> server ---")
    client.media_player_command(mp.key, volume=0.8)
    await asyncio.sleep(3)
    vol = snap_client_volume()
    passed = vol and vol["percent"] == 80
    print(f"  {'PASS' if passed else 'FAIL'}: entity volume 0.8 -> server {vol}")
    ok &= bool(passed)

    client.media_player_command(mp.key, command=MediaPlayerCommand.MUTE)
    await asyncio.sleep(3)
    vol = snap_client_volume()
    passed = vol and vol["muted"] is True and vol["percent"] == 80
    print(f"  {'PASS' if passed else 'FAIL'}: entity mute -> server {vol}")
    ok &= bool(passed)

    client.media_player_command(mp.key, command=MediaPlayerCommand.UNMUTE)
    await asyncio.sleep(3)
    vol = snap_client_volume()
    passed = vol and vol["muted"] is False and vol["percent"] == 80
    print(f"  {'PASS' if passed else 'FAIL'}: entity unmute -> server {vol}")
    ok &= bool(passed)

    await client.disconnect()
    print("\nALL PASS" if ok else "\nFAILURES PRESENT")

asyncio.run(main())
