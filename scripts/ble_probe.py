#!/usr/bin/env python3
"""Probe the Pit Boss grill over Bluetooth LE (read-only).

The grill advertises as PBV2-<12 hex digits of its MAC> and serves a
Mongoose OS JSON-RPC interface over GATT. This script connects with
pytboss's BleConnection and pokes a few unauthenticated methods.

Usage: .venv312\\Scripts\\python.exe scripts\\ble_probe.py --proxy 192.168.1.x
       .venv312\\Scripts\\python.exe scripts\\ble_probe.py            (PC's own adapter)

With --proxy the connection goes through the ESPHome bluetooth_proxy ESP32
next to the grill (see esphome/grill-proxy.yaml, scripts/esphome_ble.py);
the API key is read from GRILL_PROXY_KEY in scripts/.grill_env unless
--proxy-key is given. The grill name defaults to GRILL_BOARD_ID from
.grill_env; pass --name otherwise.
"""

import argparse
import asyncio
import json
import sys
from pathlib import Path

import bleak

from pytboss import BleConnection
from pytboss.exceptions import Error

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esphome_ble  # noqa: E402

ENV_PATH = Path(__file__).resolve().parent / ".grill_env"


def _env_value(key: str) -> str | None:
    if not ENV_PATH.exists():
        return None
    for line in ENV_PATH.read_text(encoding="utf-8").splitlines():
        k, _, v = line.strip().partition("=")
        if k.strip() == key:
            return v.strip() or None
    return None


GRILL_NAME = _env_value("GRILL_BOARD_ID") or "PBV2-"


async def _dump_gatt(device) -> None:
    """Print the GATT table — used when the Mongoose RPC service is missing."""
    print("GATT table:")
    async with bleak.BleakClient(device, timeout=30) as client:
        for svc in client.services:
            print(f"  service {svc.uuid}")
            for ch in svc.characteristics:
                print(f"    char {ch.uuid} {','.join(ch.properties)}")


async def call(conn, method, params=None, timeout=15.0):
    try:
        return await conn.send_command(method, params or {}, timeout=timeout)
    except Error as ex:
        return f"{type(ex).__name__}: {ex}"


async def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--address", default=None,
                    help="BLE MAC for the PC-adapter path (random, may rotate)")
    ap.add_argument("--name", default=GRILL_NAME,
                    help="advertised name (default: GRILL_BOARD_ID from .grill_env)")
    ap.add_argument("--proxy", metavar="HOST[:PORT]",
                    help="ESPHome bluetooth_proxy to connect through")
    ap.add_argument("--proxy-key", default=None,
                    help="api encryption key (default: GRILL_PROXY_KEY in .grill_env)")
    ap.add_argument("--no-proxy", action="store_true",
                    help="use the PC's own Bluetooth adapter instead of the proxy")
    args = ap.parse_args()
    if not args.proxy and not args.no_proxy:
        args.proxy = _env_value("GRILL_PROXY_HOST")

    proxy = None
    if args.proxy:
        key = args.proxy_key or _env_value("GRILL_PROXY_KEY")
        print(f"connecting to ESPHome proxy {args.proxy} ...")
        proxy = await esphome_ble.open_proxy(args.proxy, key)
        print("proxy connected")
        args.address = None  # the proxy resolves by advertisement, not MAC

    try:
        device = None
        if args.address:
            print(f"looking up {args.address} ...")
            try:
                device = await bleak.BleakScanner.find_device_by_address(
                    args.address, timeout=20
                )
            except Exception as ex:  # bleak quirks on Windows
                print(f"  find_by_address failed ({ex}); falling back to name scan")
        if device is None:
            print(f"scanning for name {args.name} ...")
            if proxy is not None:
                device = await esphome_ble.find_grill(args.name, timeout=30)
            else:
                device = await bleak.BleakScanner.find_device_by_filter(
                    lambda d, ad: (d.name or ad.local_name or "") == args.name,
                    timeout=30)
        if device is None:
            print("grill not found over BLE (unplugged, out of range, or wrong --name?)")
            return 1

        rssi = esphome_ble.grill_rssi(args.name) if proxy else None
        print(f"device: {device.address} {device.name}"
              + (f" rssi={rssi} dBm" if rssi is not None else ""))
        conn = BleConnection(device)
        try:
            await conn.connect()
        except Exception as ex:  # noqa: BLE001 - want the GATT table on any failure
            # connect() fails if the Mongoose RPC characteristics are absent;
            # show what the grill actually exposes before giving up.
            print(f"BleConnection.connect failed: {type(ex).__name__}: {ex}")
            await _dump_gatt(device)
            return 2
        print("connected:", conn.is_connected())
        try:
            for method in ("RPC.Ping", "Sys.GetInfo", "RPC.ListEx"):
                result = await call(conn, method)
                if isinstance(result, (dict, list)):
                    text = json.dumps(result)[:400]
                else:
                    text = str(result)
                print(f"{method} -> {text}")
        finally:
            await conn.disconnect()
        print("done")
        return 0
    finally:
        if proxy is not None:
            await proxy.close()


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
