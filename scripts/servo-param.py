#!/opt/homebrew/Cellar/esphome/2026.8.1/libexec/bin/python
"""Set a delay-loop tuning parameter on one or more boards over the native API.

Usage:
    servo-param.py <param> <value> <host> [host...]

Names and bounds live at SnapcastClient::set_servo_param (tau_s, block_n, splice_us,
tag_stale_ms, blank_ms, gap_blank_ms, autotune). Each set is WARN-logged on the device
(SERVOPARAM), so the analyser's annotations carry the change. Values are session-local:
a reboot restores the flashed defaults.
"""
import asyncio
import sys

from aioesphomeapi import APIClient


async def set_one(host: str, name: str, value: float) -> None:
    client = APIClient(host, 6053, None)
    await client.connect(login=True)
    _, services = await client.list_entities_services()
    svc = next(s for s in services if s.name == "servo_param")
    await client.execute_service(svc, {"name": name, "value": value})
    await asyncio.sleep(0.5)
    await client.disconnect()
    print(f"{host}: servo_param {name}={value}")


async def main() -> None:
    name, value, hosts = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
    await asyncio.gather(*(set_one(h, name, value) for h in hosts))


asyncio.run(main())
