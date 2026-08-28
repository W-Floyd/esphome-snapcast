"""Minimal snapserver JSON-RPC client (control port 1705)."""
import json, socket, sys

HOST = "192.168.1.2"
PORT = 1705

def rpc(method, params=None, _id=1):
    s = socket.create_connection((HOST, PORT), timeout=8)
    req = {"id": _id, "jsonrpc": "2.0", "method": method}
    if params is not None:
        req["params"] = params
    s.sendall((json.dumps(req) + "\r\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    s.close()
    return json.loads(buf.split(b"\n")[0].decode())

if __name__ == "__main__":
    if sys.argv[1] == "status":
        r = rpc("Server.GetStatus")["result"]["server"]
        print("STREAMS:")
        for st in r.get("streams", []):
            print(f"  id={st['id']!r} status={st.get('status')} "
                  f"uri={st.get('uri',{}).get('raw','')[:70]}")
        print("GROUPS:")
        for g in r.get("groups", []):
            names = [c["host"]["name"] for c in g.get("clients", [])]
            print(f"  group={g['id'][:8]} stream={g.get('stream_id')!r} muted={g.get('muted')} "
                  f"clients={names}")
    elif sys.argv[1] == "setstream":
        print(rpc("Group.SetStream", {"id": sys.argv[2], "stream_id": sys.argv[3]}))
