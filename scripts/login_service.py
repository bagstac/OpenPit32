#!/usr/bin/env python3
"""Login-only auth service for the OpenPit32 web app.

Phase 8 of docs/ESP32_FIRMWARE_PLAN.md: the ESP32 (esphome/grill-firmware.yaml)
now owns 100% of grill control, alarms, and Telegram notifications directly,
and nginx (docker/nginx.conf.template) proxies /api/health, /state, /info,
/config, /command, /alarms (+ DELETE /alarms/{id}), /setup straight to it.
This process is what's left of scripts/grill_sidecar.py once everything
grill-specific is stripped out (decision #5 in the plan) — the
password-manager-friendly login form (GET/POST /login), GET /logout, and
GET /auth-check (nginx's auth_request target). Session-cookie logic is
otherwise byte-for-byte the same as before: a signed
"<expires-unix-ts>.<hmac>" cookie, not Basic Auth, so a browser password
manager sees an ordinary form and offers to save/fill it.

Everything else that used to live here — the BLE bridge, the one-time Pit
Boss cloud password fetch, the alarm monitor, VAPID web push, the grill
model catalog — is gone: either ported onto the ESP32 itself (grill
control, alarms, Telegram notify) or dropped outright (VAPID push, the
model/probe-targets routes — see the plan's "What's removed entirely"
section and decisions #3/#6).

Configuration, all from the environment:
  AUTH_USERNAME / AUTH_PASSWORD — the login. Required for /auth-check to
    ever allow anyone in; left unset, it always denies rather than
    accidentally leaving a deployment open (same fail-closed behavior the
    old sidecar had).
  AUTH_SECRET — signs the session cookie. Generate one and set it
    explicitly to invalidate every session at once (e.g. after changing
    AUTH_PASSWORD); left unset, one is created on first run and persisted
    to AUTH_SECRET_PATH so sessions survive a restart.
  AUTH_SECRET_PATH — where to persist a generated secret (default:
    .auth_secret next to this script). docker/auth.Dockerfile points this
    at the same /data/.auth_secret path the old sidecar container used, in
    the same grill-data volume, so existing sessions carry over unchanged
    across the Phase 8 upgrade rather than forcing everyone to log in again.
  LOGIN_SERVICE_HOST / LOGIN_SERVICE_PORT — bind address (default
    127.0.0.1; 0.0.0.0 in a container) and port (default 8091, unchanged
    from the old sidecar's port so nginx.conf.template needed no port
    renumbering).
  CORS_ORIGINS — only matters for hitting this service directly (LAN
    diagnostics); the browser's normal path through nginx is same-origin
    and doesn't use it. Same default/caveat docker/README.md documented for
    the old sidecar's GRILL_SIDECAR_ORIGINS.
"""

from __future__ import annotations

import asyncio
import hmac
import logging
import os
import secrets
import time
from html import escape
from pathlib import Path

from aiohttp import web

logging.basicConfig(level=logging.WARNING)
_LOGGER = logging.getLogger("login_service")

DEFAULT_PORT = 8091
SESSION_COOKIE = "openpit32_session"
SESSION_MAX_AGE = 60 * 60 * 24 * 30  # 30 days — a password manager should
                                     # only need to fill this once in a while
LOGIN_FAIL_DELAY = 1.0  # blunts trivial brute-forcing without real rate limiting
DEFAULT_SECRET_PATH = Path(__file__).resolve().parent / ".auth_secret"


def load_or_create_auth_secret(configured: str | None, path: Path) -> bytes:
    """AUTH_SECRET from settings, else one persisted at `path`.

    Persisting (rather than regenerating every start) matters: a fresh
    secret invalidates every session cookie signed with the old one,
    logging everyone out on each restart otherwise.
    """
    if configured:
        return configured.encode("utf-8")
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


def make_app(auth_username: str | None, auth_password: str | None,
            auth_secret: bytes) -> web.Application:
    allowed_origins = {
        o.strip()
        for o in os.environ.get(
            "CORS_ORIGINS",
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
        # nginx auth_request target (docker/nginx.conf.template): status
        # code only, body/headers discarded. Fails closed if login isn't
        # configured at all, rather than leaving a misconfigured deployment
        # wide open.
        if not auth_username or not auth_password:
            return web.Response(status=401)
        cookie = request.cookies.get(SESSION_COOKIE)
        ok = session_cookie_valid(auth_secret, auth_username, cookie)
        return web.Response(status=204 if ok else 401)

    app.router.add_get("/login", login_page)
    app.router.add_post("/login", login_submit)
    app.router.add_get("/logout", logout)
    app.router.add_get("/auth-check", auth_check)
    return app


async def main():
    port = int(os.environ.get("LOGIN_SERVICE_PORT", DEFAULT_PORT))
    secret_path = Path(os.environ.get("AUTH_SECRET_PATH", str(DEFAULT_SECRET_PATH)))

    auth_username = os.environ.get("AUTH_USERNAME")
    auth_password = os.environ.get("AUTH_PASSWORD")
    auth_secret = load_or_create_auth_secret(os.environ.get("AUTH_SECRET"), secret_path)
    if not (auth_username and auth_password):
        _LOGGER.warning(
            "AUTH_USERNAME/AUTH_PASSWORD not set — /auth-check will always "
            "deny; fine for local dev, but docker/nginx.conf.template's "
            "login gate won't let anyone in until these are set")

    app = make_app(auth_username, auth_password, auth_secret)
    runner = web.AppRunner(app)
    await runner.setup()
    bind_host = os.environ.get("LOGIN_SERVICE_HOST", "127.0.0.1")
    site = web.TCPSite(runner, bind_host, port)
    await site.start()
    print(f"login service listening on http://{bind_host}:{port} "
          f"({'configured' if auth_username and auth_password else 'NOT configured'})")
    try:
        await asyncio.Event().wait()
    finally:
        await runner.cleanup()


if __name__ == "__main__":
    asyncio.run(main())
