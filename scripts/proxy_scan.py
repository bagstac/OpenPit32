#!/usr/bin/env python3
"""List everything the ESPHome BLE proxy hears for N seconds.

Usage: .venv312\\Scripts\\python.exe scripts\\proxy_scan.py [--seconds 20]
Host/key come from GRILL_PROXY_HOST / GRILL_PROXY_KEY in scripts/.grill_env.
"""

import argparse
import asyncio
import logging
import sys
from pathlib import Path

import habluetooth

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esphome_ble  # noqa: E402

ENV_PATH = Path(__file__).resolve().parent / ".grill_env"


def env(key):
    for line in ENV_PATH.read_text(encoding="utf-8").splitlines():
        k, _, v = line.strip().partition("=")
        if k.strip() == key:
            return v.strip()
    return None


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--host", default=env("GRILL_PROXY_HOST"))
    a = ap.parse_args()
    logging.basicConfig(level=logging.WARNING)

    proxy = await esphome_ble.open_proxy(a.host, env("GRILL_PROXY_KEY"))
    try:
        await asyncio.sleep(a.seconds)
        mgr = habluetooth.get_manager()
        infos = sorted(mgr.async_discovered_service_info(False),
                       key=lambda i: -i.rssi)
        print(f"{len(infos)} devices heard in {a.seconds:.0f}s:")
        for i in infos:
            flag = "  <== GRILL" if (i.name or "").startswith("PBV") else ""
            print(f"  {i.rssi:4d} dBm  {i.address}  {i.name or '-'}{flag}")
    finally:
        await proxy.close()


if __name__ == "__main__":
    asyncio.run(main())
