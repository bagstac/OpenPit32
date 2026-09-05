#!/usr/bin/env python3
"""Scan for the Pit Boss grill over Bluetooth LE with the PC's own adapter.

Lists every advertisement the PC hears and flags names that look like the
grill ("PBV2-..."). Useful once, to learn the grill's advertised name and
whether the PC is in range at all; for the ESP32 proxy use proxy_scan.py.

Usage: .venv312\\Scripts\\python.exe scripts\\ble_scan.py [--seconds 20]
"""

import argparse
import asyncio

from bleak import BleakScanner


async def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=float, default=20.0)
    args = ap.parse_args()

    print(f"scanning BLE for {args.seconds:.0f}s ...")
    devices = await BleakScanner.discover(timeout=args.seconds, return_adv=True)
    if not devices:
        print("no BLE devices found (adapter off or nothing in range)")
        return 1

    print(f"{len(devices)} device(s) found:")
    for addr, (dev, adv) in devices.items():
        name = dev.name or adv.local_name or "?"
        rssi = adv.rssi
        marker = ""
        if name.startswith("PBV") or "PITBOSS" in name.upper():
            marker = "  <-- likely grill"
        print(f"  {addr:17}  rssi={rssi:>4}  {name}{marker}")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
