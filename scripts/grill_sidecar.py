#!/usr/bin/env python3
"""Local bridge: Pit Boss grill over BLE (via an ESP32 proxy) <-> localhost HTTP.

The web UI (OpenPit32, Blazor WebAssembly) cannot speak Bluetooth, so this
sidecar holds one pytboss PitBoss session to the grill and exposes it on
http://127.0.0.1:8091 with CORS for the local web app:

    GET  /health        -> configured?, grill link, proxy link, RSSIs, last error
    GET  /state         -> cached decoded grill state (FE0B/FE0C frames)
    GET  /info          -> board id, model, firmware, accepted setpoints
    GET  /models        -> grill models pytboss knows for this control board
    GET  /probe-targets -> current probe targets
    POST /command       -> {action, value?, probe?, confirm?}
    POST /setup         -> {email, password, country?, grill_id?, model?}
                           one-time: fetch the grill password from the Pit
                           Boss account, save it to .grill_env, reconnect

Path to the grill: pytboss BleConnection -> habluetooth/bleak-esphome ->
ESPHome native API -> ESP32 running `bluetooth_proxy` (esphome/grill-proxy.yaml)
-> grill GATT (Mongoose OS RPC service). It answers even with the controller
switched off. BleConnection does not reconnect by itself, so _ble_keepalive
rescans and calls reset_device() with backoff; the ESP32 API link reconnects
on its own (aioesphomeapi ReconnectLogic).

Safety: power_on / power_off require "confirm": true — power_on ignites the
grill. Temperatures snap to the board's accepted setpoints inside pytboss.

Configuration: scripts/.grill_env (git-ignored), keys GRILL_PROXY_HOST,
GRILL_PROXY_KEY, GRILL_PROXY_PORT, GRILL_BOARD_ID, GRILL_PASSWORD, GRILL_MODEL.
Environment variables of the same names override it. The grill password is
never printed or logged.

Deployment: GRILL_SIDECAR_HOST binds the HTTP server (default
127.0.0.1; set to 0.0.0.0 in a container). GRILL_ENV_PATH points
scripts/pitboss_cloud.py at a mounted volume instead of a path
next to this script, so a password fetched via /setup survives a
container restart. See docker/ and docker-compose.yml.

Login: GET/POST /login and GET /logout back the app's own login page
(behind docker/nginx.conf's auth_request, not used for a bare-metal
run), plus GET /auth-check for nginx to call. A signed cookie, not
Basic Auth, so a browser password manager can see this as an ordinary
form and offer to save/fill it. AUTH_USERNAME/AUTH_PASSWORD (from the
environment or .grill_env) gate it; unset, /auth-check always denies
rather than accidentally leaving a deployment open. AUTH_SECRET signs
the session cookie — generate one and set it explicitly to invalidate
every session at once (e.g. after changing the password); left unset,
one is created on first run and persisted next to .grill_env.
"""

from __future__ import annotations

import argparse
import asyncio
import hmac
import json
import logging
import os
import secrets
import sys
import time
from html import escape
from pathlib import Path

from aiohttp import web

from pytboss import BleConnection, PitBoss
from pytboss.exceptions import Error, Unauthorized, UnsupportedOperation
from pytboss.grills import get_grills

sys.path.insert(0, str(Path(__file__).resolve().parent))
import esphome_ble  # noqa: E402
import pitboss_cloud  # noqa: E402

logging.basicConfig(level=logging.WARNING)
_LOGGER = logging.getLogger("grill_sidecar")

DEFAULT_PORT = 8091
POLL_SECONDS = 10.0
DEFAULT_MODEL = "PBV5 P2"  # what this project was developed against
CONTROL_BOARD = "PBV2"  # the prefix the grill advertises over BLE
BLE_SCAN_SECONDS = 10.0
BLE_BACKOFF_MAX = 60.0
# The firmware keys the password on its uptime in 10 s buckets and accepts
# only the current or next one; pytboss extrapolates uptime locally, so a
# request that is slow to arrive (BLE writes 20-byte chunks, worse at low
# RSSI) can land one bucket late and draw a spurious Unauthorized. A retry
# a moment later recomputes the key, so one such rejection is not an error.
AUTH_RETRY_DELAY = 1.0

SESSION_COOKIE = "openpit32_session"
SESSION_MAX_AGE = 60 * 60 * 24 * 30  # 30 days — a password manager should
                                     # only need to fill this once in a while
LOGIN_FAIL_DELAY = 1.0  # blunts trivial brute-forcing without real rate limiting


def setting(env: dict, key: str, default=None):
    """Environment variable, else .grill_env, else default."""
    return os.environ.get(key) or env.get(key) or default


def _auth_secret_path() -> Path:
    return pitboss_cloud.ENV_PATH.parent / ".auth_secret"


def load_or_create_auth_secret(configured: str | None) -> bytes:
    """AUTH_SECRET from settings, else one persisted next to .grill_env.

    Persisting (rather than regenerating every start) matters: a fresh
    secret invalidates every session cookie signed with the old one,
    logging everyone out on each restart otherwise.
    """
    if configured:
        return configured.encode("utf-8")
    path = _auth_secret_path()
    if path.exists():
        return bytes.fromhex(path.read_text(encoding="utf-8").strip())
    secret = secrets.token_bytes(32)
    path.write_text(secret.hex(), encoding="utf-8")
    return secret


def _sign(secret: bytes, *parts: str) -> str:
    return hmac.new(secret, ":".join(parts).encode("utf-8"), "sha256").hexdigest()


def make_session_cookie(secret: bytes, username: str) -> str:
    expires = str(int(time.time()) + SESSION_MAX_AGE)
    return f"{expires}.{_sign(secret, username, expires)}"


def session_cookie_valid(secret: bytes, username: str, cookie: str | None) -> bool:
    if not cookie or "." not in cookie:
        return False
    expires, _, sig = cookie.partition(".")
    if not expires.isdigit() or int(expires) < time.time():
        return False
    return hmac.compare_digest(sig, _sign(secret, username, expires))


LOGIN_PAGE = """\
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OpenPit32 — Log in</title>
<link rel="icon" href="/favicon.ico" sizes="any">
<style>
  :root {{ color-scheme: dark; }}
  body {{
    background: #1b1a1d; color: #e8e6e3; font-family: system-ui, sans-serif;
    display: flex; align-items: center; justify-content: center;
    min-height: 100vh; margin: 0;
  }}
  form {{
    background: #232226; border-radius: 12px; padding: 2rem;
    width: 100%; max-width: 320px; box-shadow: 0 8px 24px rgba(0,0,0,.4);
  }}
  h1 {{ font-size: 1.25rem; margin: 0 0 1.25rem; color: #ff6a2b; }}
  label {{ display: block; font-size: .85rem; color: #9aa0a6; margin: .75rem 0 .25rem; }}
  input {{
    width: 100%; box-sizing: border-box; padding: .5rem .6rem;
    border-radius: 6px; border: 1px solid #3a393e; background: #1b1a1d;
    color: #e8e6e3; font-size: 1rem;
  }}
  button {{
    width: 100%; margin-top: 1.5rem; padding: .6rem; border: none;
    border-radius: 6px; background: #ff6a2b; color: #1b1a1d;
    font-weight: 600; font-size: 1rem; cursor: pointer;
  }}
  button:hover {{ background: #ff8248; }}
  .error {{ color: #ff8080; font-size: .85rem; margin-top: 1rem; }}
</style>
</head>
<body>
<form method="post" action="/login">
  <h1>OpenPit32</h1>
  <input type="hidden" name="next" value="{next_url}">
  <label for="username">Username</label>
  <input id="username" name="username" autocomplete="username" required autofocus>
  <label for="password">Password</label>
  <input id="password" name="password" type="password" autocomplete="current-password" required>
  <button type="submit">Log in</button>
  {error_html}
</form>
</body>
</html>
"""


class GrillBridge:
    """One ESP32 proxy session plus (once configured) one grill session."""

    def __init__(self, proxy_host: str, proxy_key: str | None,
                 proxy_port: int = esphome_ble.DEFAULT_PORT):
        self._proxy_host = proxy_host
        self._proxy_port = proxy_port
        self._proxy_key = proxy_key
        self.proxy: esphome_ble.ProxyHandle | None = None
        # Grill session — None until configure() has a board id + password.
        self.board_id: str | None = None
        self.model: str | None = None
        self.boss: PitBoss | None = None
        self._ble_conn: BleConnection | None = None
        self.rssi: int | None = None
        self.state = None
        self.state_at: float | None = None
        self.last_error: str | None = None
        self.firmware = None
        self._poll_task: asyncio.Task | None = None
        self._keepalive_task: asyncio.Task | None = None
        self._ble_dropped = asyncio.Event()   # wakes the keepalive early
        self._ble_first_try = asyncio.Event()  # set after the initial attempt
        self._configure_lock = asyncio.Lock()

    @property
    def configured(self) -> bool:
        return self.boss is not None

    @property
    def proxy_host(self) -> str:
        return self._proxy_host

    async def start(self) -> None:
        self.proxy = await esphome_ble.open_proxy(
            self._proxy_host, self._proxy_key, port=self._proxy_port)

    async def stop(self) -> None:
        await self._stop_grill()
        if self.proxy is not None:
            await self.proxy.close()

    async def configure(self, board_id: str, password: str, model: str) -> None:
        """(Re)start the grill session with these credentials.

        Starts with a device-less BleConnection: connect() is a no-op without
        one, so PitBoss.start() loads the spec and the HTTP routes work before
        the grill is heard. _ble_keepalive finds it and calls reset_device(),
        which is also the reconnect path after a drop.
        """
        async with self._configure_lock:
            await self._stop_grill()
            self.board_id = board_id
            self.model = model
            self.state = self.state_at = self.firmware = None
            self.last_error = None
            self._ble_first_try.clear()
            self._ble_conn = BleConnection(
                None, disconnect_callback=self._on_ble_disconnect)
            boss = PitBoss(self._ble_conn, model, password,
                           control_board=CONTROL_BOARD)
            await boss.start()
            await boss.subscribe_state(self._on_state)
            self.boss = boss
            self._keepalive_task = asyncio.create_task(self._ble_keepalive())
            await self._ble_first_try.wait()  # one connect attempt before serving
            try:
                self.firmware = await boss.get_firmware_version()
            except Error as ex:
                self.last_error = f"firmware: {ex}"
            self._poll_task = asyncio.create_task(self._poll_loop())

    async def _stop_grill(self) -> None:
        for task in (self._poll_task, self._keepalive_task):
            if task:
                task.cancel()
        self._poll_task = self._keepalive_task = None
        if self.boss is not None:
            boss, self.boss = self.boss, None
            await boss.stop()

    def is_connected(self) -> bool:
        return self.boss is not None and self.boss.is_connected()

    def proxy_connected(self) -> bool:
        return self.proxy is not None and self.proxy.connected

    def _on_ble_disconnect(self, client):
        # Runs inside bleak's callback: only flag it, the keepalive reconnects.
        self.last_error = "grill BLE link dropped — reconnecting"
        self._ble_dropped.set()

    async def _ble_keepalive(self):
        """Keep the BLE link up: scan via the proxy, reset_device(), back off.

        Woken immediately by the disconnect callback; otherwise rechecks every
        BLE_SCAN_SECONDS. The first pass doubles as the initial connect —
        configure() waits on _ble_first_try, set after that attempt whether
        or not it succeeded, so the sidecar serves either way.
        """
        backoff = 2.0
        while True:
            try:
                if self.is_connected():
                    self.rssi = esphome_ble.grill_rssi(self.board_id)
                    backoff = 2.0
                elif not self.proxy_connected():
                    self.last_error = (
                        f"ESPHome proxy {self._proxy_host} unreachable")
                else:
                    device = await esphome_ble.find_grill(
                        self.board_id, timeout=BLE_SCAN_SECONDS)
                    if device is None:
                        self.last_error = (
                            "grill not heard by the proxy — powered off at "
                            "the mains, or out of the ESP32's range?")
                    else:
                        self.rssi = esphome_ble.grill_rssi(self.board_id)
                        await self._ble_conn.reset_device(device)
                        self.last_error = None
                        backoff = 2.0
                        _LOGGER.info("BLE connected to %s (rssi %s)",
                                     device.address, self.rssi)
            except asyncio.CancelledError:
                return
            except Exception as ex:  # noqa: BLE001 - bleak raises many types
                self.last_error = f"BLE connect: {type(ex).__name__}: {ex}"
                backoff = min(backoff * 2, BLE_BACKOFF_MAX)
            finally:
                self._ble_first_try.set()
            self._ble_dropped.clear()
            wait = BLE_SCAN_SECONDS if self.is_connected() else backoff
            try:
                await asyncio.wait_for(self._ble_dropped.wait(), timeout=wait)
            except asyncio.TimeoutError:
                pass

    async def _on_state(self, state):
        self.state = state
        self.state_at = time.time()
        self.last_error = None

    async def _poll_loop(self):
        """Backup to the pushed state: the grill notifies FE0B/FE0C frames on
        its own over BLE, this just covers a quiet link."""
        while True:
            await asyncio.sleep(POLL_SECONDS)
            try:
                if not self.is_connected():
                    continue
                fresh = await self._get_state_retrying()
                # The firmware blanks its status frames right after every
                # command and refills them on the next MCU reply — an empty
                # decode must not wipe the cached state.
                if fresh:
                    self.state = fresh
                    self.state_at = time.time()
                    self.last_error = None
            except asyncio.CancelledError:
                return
            except Error as ex:
                self.last_error = f"{type(ex).__name__}: {ex}"

    async def _get_state_retrying(self):
        """get_state(), retrying once across a spurious Unauthorized."""
        try:
            return await self.boss.get_state()
        except Unauthorized:
            _LOGGER.info("get_state Unauthorized (key bucket skew) — retrying")
            await asyncio.sleep(AUTH_RETRY_DELAY)
            return await self.boss.get_state()

    async def _refresh_now(self):
        """Poll a few times so /state reflects a just-sent command quickly.

        The grill pushes state on its own (see _on_state), so a failure here
        only means the push wins — it must not raise a banner for a command
        that already succeeded.
        """
        for _attempt in range(5):
            try:
                fresh = await self._get_state_retrying()
                if fresh:
                    self.state = fresh
                    self.state_at = time.time()
                    self.last_error = None
                    return
            except Error:
                return
            await asyncio.sleep(2)

    def freshness(self) -> float | None:
        if self.state_at is None:
            return None
        return round(time.time() - self.state_at, 1)

    async def command(self, action: str, value, probe: int | None,
                      confirm: bool) -> dict:
        action = (action or "").strip().lower()

        if not self.configured:
            return {"ok": False, "error": "grill not set up yet — POST /setup"}
        if action in ("power_on", "power_off") and not confirm:
            return {"ok": False, "error": f"'{action}' requires confirm: true"}
        if action == "set_temp" and value is None:
            return {"ok": False, "error": "set_temp needs a value"}
        if action == "set_probe" and (probe is None or value is None):
            return {"ok": False, "error": "set_probe needs probe and value"}
        if action not in ("power_on", "power_off", "light_on", "light_off",
                          "prime_on", "prime_off", "set_temp", "set_probe"):
            return {"ok": False, "error": f"unknown action {action!r}"}

        boss = self.boss

        async def dispatch():
            if action == "power_on":
                await boss.turn_grill_on()
            elif action == "power_off":
                await boss.turn_grill_off()
            elif action == "light_on":
                await boss.turn_light_on()
            elif action == "light_off":
                await boss.turn_light_off()
            elif action == "prime_on":
                await boss.turn_primer_motor_on()
            elif action == "prime_off":
                await boss.turn_primer_motor_off()
            elif action == "set_temp":
                await boss.set_grill_temperature(int(value))
            elif action == "set_probe":
                await boss.set_probe_target(int(probe), int(value))

        try:
            try:
                await dispatch()
            except Unauthorized:
                # Key-bucket skew (see AUTH_RETRY_DELAY), not a bad password:
                # the same command a second later normally goes through. The
                # MCU commands are idempotent (absolute setpoint / on / off),
                # so a retry cannot double-apply.
                _LOGGER.info("%s Unauthorized — retrying once", action)
                await asyncio.sleep(AUTH_RETRY_DELAY)
                await dispatch()
        except UnsupportedOperation as ex:
            return {"ok": False, "error": f"unsupported on this grill: {ex}"}
        except Error as ex:
            return {"ok": False, "error": f"{type(ex).__name__}: {ex}"}

        # Refresh the cache right away so the UI sees the change within a
        # couple of seconds instead of waiting for the next poll cycle.
        await self._refresh_now()
        return {"ok": True, "action": action}


def make_app(bridge: GrillBridge, auth_username: str | None = None,
            auth_password: str | None = None, auth_secret: bytes | None = None,
            ) -> web.Application:
    allowed_origins = {
        o.strip()
        for o in os.environ.get(
            "GRILL_SIDECAR_ORIGINS",
            "http://localhost:5219,http://127.0.0.1:5219",
        ).split(",")
        if o.strip()
    }

    def cors_headers(request) -> dict:
        origin = request.headers.get("Origin", "")
        ok = origin in allowed_origins or origin.startswith("http://localhost:")
        return ({"Access-Control-Allow-Origin": origin,
                 "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
                 "Access-Control-Allow-Headers": "Content-Type"} if ok else {})

    @web.middleware
    async def cors(request, handler):
        if request.method == "OPTIONS":
            return web.Response(status=204, headers=cors_headers(request))
        resp = await handler(request)
        resp.headers.update(cors_headers(request))
        return resp

    app = web.Application(middlewares=[cors])

    async def health(request):
        return web.json_response({
            "configured": bridge.configured,
            "connected": bridge.is_connected(),
            "proxy_connected": bridge.proxy_connected(),
            "proxy_host": bridge.proxy_host,
            # BLE leg: ESP32's view of the grill's advertisements (only
            # refreshed while the grill advertises, i.e. between connections).
            "rssi": bridge.rssi,
            # WiFi leg: the ESP32's own wifi_signal sensor, ~30 s cadence.
            "proxy_wifi_rssi": bridge.proxy.wifi_rssi if bridge.proxy else None,
            "proxy_uptime_seconds": bridge.proxy.uptime_s if bridge.proxy else None,
            "state_age_seconds": bridge.freshness(),
            "last_error": bridge.last_error,
        })

    async def state(request):
        return web.json_response({
            "state": bridge.state,
            "state_age_seconds": bridge.freshness(),
            "last_error": bridge.last_error,
        })

    async def info(request):
        if not bridge.configured:
            return web.json_response({"configured": False}, status=404)
        boss = bridge.boss
        return web.json_response({
            "configured": True,
            "board_id": bridge.board_id,
            "model": bridge.model,
            "firmware": bridge.firmware,
            "accepted_setpoints_f": boss.accepted_setpoints(fahrenheit=True),
            "has_lights": boss.spec.has_lights,
            "meat_probes": boss.spec.meat_probes,
        })

    async def models(request):
        names = sorted(g.name for g in get_grills(control_board=CONTROL_BOARD))
        return web.json_response({"control_board": CONTROL_BOARD, "models": names,
                                  "default": DEFAULT_MODEL})

    async def probe_targets(request):
        if not bridge.configured:
            return web.json_response({"ok": False, "error": "not set up"})
        try:
            targets = await bridge.boss.get_probe_targets()
            return web.json_response({"ok": True, "targets": targets})
        except Error as ex:
            return web.json_response(
                {"ok": False, "error": f"{type(ex).__name__}: {ex}"})

    async def command(request):
        try:
            body = await request.json()
        except (ValueError, json.JSONDecodeError):
            return web.json_response({"ok": False, "error": "invalid JSON"},
                                     status=400)
        result = await bridge.command(
            body.get("action"), body.get("value"), body.get("probe"),
            bool(body.get("confirm")))
        return web.json_response(result,
                                 status=200 if result.get("ok") else 400)

    async def setup(request):
        """Fetch the grill password from the Pit Boss account and connect.

        The account credentials are used for one login and discarded; only
        the grill's board id / password / model land in .grill_env.
        """
        try:
            body = await request.json()
        except (ValueError, json.JSONDecodeError):
            return web.json_response({"ok": False, "error": "invalid JSON"},
                                     status=400)
        email = (body.get("email") or "").strip()
        password = body.get("password") or ""
        if not email or not password:
            return web.json_response(
                {"ok": False, "error": "email and password are required"},
                status=400)
        country = (body.get("country") or "US").strip().upper()
        grill_id = body.get("grill_id")
        model = (body.get("model") or bridge.model or DEFAULT_MODEL).strip()
        try:
            # urllib is blocking; keep the event loop (and the BLE link) alive.
            result = await asyncio.to_thread(
                pitboss_cloud.fetch_and_save, email, password,
                country=country, grill_id=grill_id)
        except pitboss_cloud.GrillChoiceNeeded as ex:
            return web.json_response(
                {"ok": False, "error": str(ex), "grills": ex.grills}, status=409)
        except pitboss_cloud.CloudError as ex:
            return web.json_response({"ok": False, "error": str(ex)}, status=502)
        del password, body
        env = pitboss_cloud.load_env()
        pitboss_cloud.save_env({"GRILL_MODEL": model})
        try:
            await bridge.configure(result["board_id"], env["GRILL_PASSWORD"], model)
        except Error as ex:
            return web.json_response(
                {"ok": False, "error": f"saved, but connecting failed: {ex}"},
                status=500)
        return web.json_response({"ok": True, "board_id": result["board_id"],
                                  "nickname": result["nickname"], "model": model,
                                  "connected": bridge.is_connected()})

    def _safe_next(raw: str | None) -> str:
        # Only an on-site path is safe to redirect to — an absolute URL in
        # "next" could send a just-authenticated browser somewhere else.
        return raw if raw and raw.startswith("/") and not raw.startswith("//") else "/"

    async def login_page(request):
        return web.Response(
            text=LOGIN_PAGE.format(
                next_url=escape(_safe_next(request.query.get("next")), quote=True),
                error_html=('<p class="error">Wrong username or password.</p>'
                           if request.query.get("error") else "")),
            content_type="text/html")

    async def login_submit(request):
        if not auth_username or not auth_password:
            return web.Response(
                text="Login is not configured — set AUTH_USERNAME and "
                     "AUTH_PASSWORD (see docker/README.md).",
                status=500)
        body = await request.post()
        username = str(body.get("username") or "")
        password = str(body.get("password") or "")
        next_url = _safe_next(str(body.get("next") or ""))
        ok = (hmac.compare_digest(username, auth_username)
              and hmac.compare_digest(password, auth_password))
        del password, body
        if not ok:
            _LOGGER.info("login failed for username %r", username)
            await asyncio.sleep(LOGIN_FAIL_DELAY)
            raise web.HTTPFound(f"/login?error=1&next={next_url}")
        resp = web.HTTPFound(next_url)
        resp.set_cookie(SESSION_COOKIE, make_session_cookie(auth_secret, auth_username),
                        max_age=SESSION_MAX_AGE, httponly=True, samesite="Lax", path="/")
        raise resp

    async def logout(request):
        resp = web.HTTPFound("/login")
        resp.del_cookie(SESSION_COOKIE, path="/")
        raise resp

    async def auth_check(request):
        # nginx auth_request target (docker/nginx.conf): status code only,
        # body/headers are discarded. Fails closed if login isn't configured
        # at all, rather than leaving a misconfigured deployment wide open.
        if not auth_username or not auth_password:
            return web.Response(status=401)
        cookie = request.cookies.get(SESSION_COOKIE)
        ok = session_cookie_valid(auth_secret, auth_username, cookie)
        return web.Response(status=204 if ok else 401)

    app.router.add_get("/health", health)
    app.router.add_get("/state", state)
    app.router.add_get("/info", info)
    app.router.add_get("/models", models)
    app.router.add_get("/probe-targets", probe_targets)
    app.router.add_post("/command", command)
    app.router.add_post("/setup", setup)
    app.router.add_get("/login", login_page)
    app.router.add_post("/login", login_submit)
    app.router.add_get("/logout", logout)
    app.router.add_get("/auth-check", auth_check)
    return app


async def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("GRILL_SIDECAR_PORT", DEFAULT_PORT)))
    ap.add_argument("--model", default=None,
                    help="grill model name as pytboss knows it (see /models); "
                         "default GRILL_MODEL from .grill_env, else "
                         f"{DEFAULT_MODEL!r}")
    args = ap.parse_args()

    env = pitboss_cloud.load_env()
    proxy_host = setting(env, "GRILL_PROXY_HOST")
    if not proxy_host:
        raise SystemExit(
            "GRILL_PROXY_HOST (and GRILL_PROXY_KEY) must be set in "
            f"{pitboss_cloud.ENV_PATH} or the environment — the IP of the "
            "ESP32 running esphome/grill-proxy.yaml")

    bridge = GrillBridge(
        proxy_host, setting(env, "GRILL_PROXY_KEY"),
        proxy_port=int(setting(env, "GRILL_PROXY_PORT", esphome_ble.DEFAULT_PORT)))
    await bridge.start()

    board_id = setting(env, "GRILL_BOARD_ID")
    password = setting(env, "GRILL_PASSWORD")
    model = args.model or setting(env, "GRILL_MODEL", DEFAULT_MODEL)
    if board_id and password:
        await bridge.configure(board_id, password, model)
        status = f"board {board_id}, model {model}"
    else:
        bridge.model = model
        status = "NOT SET UP — open the web UI and sign in to fetch the grill password"

    auth_username = setting(env, "AUTH_USERNAME")
    auth_password = setting(env, "AUTH_PASSWORD")
    auth_secret = load_or_create_auth_secret(setting(env, "AUTH_SECRET"))
    if not (auth_username and auth_password):
        _LOGGER.warning(
            "AUTH_USERNAME/AUTH_PASSWORD not set — /auth-check will always "
            "deny; fine for local dev, but docker/nginx.conf's login gate "
            "won't let anyone in until these are set")

    app = make_app(bridge, auth_username, auth_password, auth_secret)
    runner = web.AppRunner(app)
    await runner.setup()
    bind_host = os.environ.get("GRILL_SIDECAR_HOST", "127.0.0.1")
    site = web.TCPSite(runner, bind_host, args.port)
    await site.start()
    print(f"grill sidecar listening on http://{bind_host}:{args.port} "
          f"(proxy {proxy_host}; {status})")
    try:
        await asyncio.Event().wait()
    finally:
        await bridge.stop()
        await runner.cleanup()


if __name__ == "__main__":
    asyncio.run(main())
