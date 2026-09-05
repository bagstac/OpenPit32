"""Route pytboss's BLE transport through an ESPHome bluetooth_proxy ESP32.

The grill is out of BLE range of the PC, so an ESP32 running the stock
ESPHome ``bluetooth_proxy`` component (esphome/grill-proxy.yaml) sits next to
it. ``bleak-esphome`` is a Bleak backend that drives that proxy over the
ESPHome native API (TCP 6053). It plugs into ``habluetooth``'s central
BluetoothManager, which then hands out proxy-backed clients from the normal
``bleak.BleakClient`` / ``BleakScanner`` entry points.

Usage::

    proxy = await open_proxy("192.168.1.x", noise_key)
    device = await find_grill("PBV2-XXXXXXXXXXXX")   # the grill's advertised name
    conn = BleConnection(device)          # pytboss, unchanged
    ...
    await proxy.close()

Gotcha handled here: habluetooth works by monkey-patching ``bleak.BleakClient``
and ``bleak_retry_connector.BleakClientWithServiceCache`` at manager setup.
``pytboss.ble`` binds both names at import time, so if pytboss was imported
first (the sidecar does) it would keep the originals and try the PC's own
adapter. ``open_proxy`` re-points ``pytboss.ble`` at the patched classes.
"""

from __future__ import annotations

import asyncio
import logging
from dataclasses import dataclass

import bleak
import bleak_retry_connector
import habluetooth
from bleak import BLEDevice
from bleak_esphome import APIConnectionManager

_LOGGER = logging.getLogger("esphome_ble")

DEFAULT_PORT = 6053


class TelemetryProxyManager(APIConnectionManager):
    """APIConnectionManager that also follows the proxy's own sensors.

    grill-proxy.yaml exposes wifi_signal / uptime sensors; this subscribes
    to their states on the same API session the BLE scanner uses, so the
    sidecar's /health can show the ESP32's WiFi leg next to the BLE one.

    Hooks the private ``_on_connect`` / ``_on_disconnect`` / ``_cli`` of
    bleak-esphome 4.1.0 (pinned in STATE.md): they are the only place that
    runs after every (re)connect, which is when a state subscription has to
    be re-established.
    """

    def __init__(self, config) -> None:
        super().__init__(config)
        self.sensors: dict[str, float] = {}  # object_id -> last state
        self._sensor_keys: dict[int, str] = {}

    async def _on_connect(self) -> None:
        await super()._on_connect()
        try:
            entities, _services = await self._cli.list_entities_services()
            self._sensor_keys = {
                e.key: e.object_id for e in entities
                if type(e).__name__ == "SensorInfo"
            }
            self._cli.subscribe_states(self._on_sensor_state)
        except Exception as ex:  # noqa: BLE001 - telemetry is best-effort
            _LOGGER.warning("proxy telemetry subscription failed: %s", ex)

    async def _on_disconnect(self, expected_disconnect: bool) -> None:
        self.sensors.clear()  # stale values must not outlive the link
        await super()._on_disconnect(expected_disconnect)

    def _on_sensor_state(self, state) -> None:
        object_id = self._sensor_keys.get(getattr(state, "key", None))
        if object_id is None or getattr(state, "missing_state", False):
            return
        value = getattr(state, "state", None)
        if isinstance(value, (int, float)):
            self.sensors[object_id] = value


@dataclass
class ProxyHandle:
    """An open ESPHome proxy session plus the Bluetooth manager behind it."""

    manager: habluetooth.BluetoothManager
    api: TelemetryProxyManager
    host: str

    @property
    def connected(self) -> bool:
        """Whether the ESPHome API session is up and its scanner registered.

        APIConnectionManager reconnects on its own (aioesphomeapi
        ReconnectLogic) and unregisters the scanner while the link is down,
        so a registered scanner is the same thing as "proxy reachable".
        """
        return self.manager.async_scanner_count(True) > 0

    @property
    def wifi_rssi(self) -> int | None:
        """The ESP32's own WiFi RSSI in dBm (its `wifi_signal` sensor)."""
        v = self.api.sensors.get("wifi_signal")
        return int(v) if v is not None else None

    @property
    def uptime_s(self) -> int | None:
        v = self.api.sensors.get("uptime")
        return int(v) if v is not None else None

    async def close(self) -> None:
        try:
            await self.api.stop()
        finally:
            self.manager.async_stop()


async def open_proxy(host: str, noise_key: str | None = None,
                     port: int = DEFAULT_PORT) -> ProxyHandle:
    """Connect to the ESPHome proxy and make it the BLE adapter for bleak.

    :param host: proxy hostname/IP. A ``host:port`` form is accepted too.
    :param noise_key: the ``api.encryption.key`` from grill-proxy.yaml
        (GRILL_PROXY_KEY in scripts/.grill_env). None if the API is unencrypted.
    """
    if ":" in host and not host.startswith("["):
        host, _, port_s = host.rpartition(":")
        port = int(port_s)
    if port != DEFAULT_PORT:
        # APIConnectionManager hardcodes 6053; aioesphomeapi accepts host:port
        # in the address field, so fold it back in.
        host = f"{host}:{port}"

    manager = habluetooth.BluetoothManager()
    await manager.async_setup()  # installs the bleak catcher (see module doc)
    _repoint_pytboss()

    api = TelemetryProxyManager({"address": host, "noise_psk": noise_key})
    try:
        await api.start()  # returns once the scanner is registered
    except Exception:
        manager.async_stop()
        raise
    _LOGGER.info("ESPHome proxy %s connected; scanner registered", host)
    return ProxyHandle(manager=manager, api=api, host=host)


def _repoint_pytboss() -> None:
    """Make an already-imported pytboss.ble use the habluetooth wrappers."""
    import sys

    mod = sys.modules.get("pytboss.ble")
    if mod is None:
        return  # not imported yet; it will pick up the patched names itself
    mod.BleakClient = bleak.BleakClient
    mod.BleakClientWithServiceCache = bleak_retry_connector.BleakClientWithServiceCache


async def find_grill(name: str, timeout: float = 30.0) -> BLEDevice | None:
    """Wait for the proxy to hear an advertisement with exactly this name.

    Matches on the advertised name rather than the MAC: the grill uses a
    random BLE address that was seen to change between scans.

    Polls the manager's connectable history rather than
    ``BleakScanner.find_device_by_filter``: the bare ``BluetoothManager`` has a
    no-op ``_discover_service_info`` hook, so the bleak scanner-wrapper
    callbacks never fire outside Home Assistant (verified 2026-09-05: the
    proxy heard the grill at -63 dBm while find_device_by_filter timed out).
    The history's ``device`` is the proxy-backed BLEDevice, so a
    ``BleakClient`` built on it routes through the ESP32.
    """
    manager = habluetooth.get_manager()
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while True:
        for info in manager.async_discovered_service_info(True):
            if info.name == name:
                return info.device
        if loop.time() >= deadline:
            return None
        await asyncio.sleep(0.5)


def grill_rssi(name: str) -> int | None:
    """Last RSSI the proxy reported for the grill, or None if never heard."""
    manager = habluetooth.get_manager()
    for info in manager.async_discovered_service_info(True):
        if info.name == name:
            return info.rssi
    return None


async def _selftest(host: str, key: str | None, name: str) -> int:
    proxy = await open_proxy(host, key)
    try:
        dev = await find_grill(name)
        if dev is None:
            print(f"proxy up, but {name} not heard in 30 s (rssi={grill_rssi(name)})")
            return 1
        print(f"heard {dev.name} at {dev.address}, rssi={grill_rssi(name)} dBm")
        return 0
    finally:
        await proxy.close()


if __name__ == "__main__":
    import argparse

    import pitboss_cloud

    env = pitboss_cloud.load_env()
    ap = argparse.ArgumentParser(description="ESPHome proxy self-test")
    ap.add_argument("host", nargs="?", default=env.get("GRILL_PROXY_HOST"),
                    help="proxy IP (default: GRILL_PROXY_HOST from .grill_env)")
    ap.add_argument("--key", default=env.get("GRILL_PROXY_KEY"),
                    help="api encryption key (default: GRILL_PROXY_KEY from .grill_env)")
    ap.add_argument("--name", default=env.get("GRILL_BOARD_ID"),
                    help="grill's advertised name (default: GRILL_BOARD_ID from .grill_env)")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    if not a.host or not a.name:
        ap.error("need a proxy host and --name (or set them in scripts/.grill_env)")
    logging.basicConfig(level=logging.DEBUG if a.v else logging.INFO)
    raise SystemExit(asyncio.run(_selftest(a.host, a.key, a.name)))
