import asyncio, sys, time
from aioesphomeapi import APIClient

HOST, KEY = sys.argv[1], "fLqi30JJIFnTdxgNm9XE+zQRpfU2/jdvrhjifE7LgCs="
US = int(sys.argv[2])

async def main():
    c = APIClient(HOST, 6053, None)
    await c.connect(login=True)
    _, services = await c.list_entities_services()
    svc = next(s for s in services if s.name == "inject_split")
    r = c.execute_service(svc, {"us": US})
    if hasattr(r, "__await__"):  # newer aioesphomeapi returns a coroutine; a bare call is a no-op
        await r
    print(f"inject_split {US:+d} us -> {HOST} at {time.time():.3f} ({time.strftime('%H:%M:%S')})")
    await asyncio.sleep(1.0)  # let the call flush before the socket closes
    await c.disconnect()

asyncio.run(main())
