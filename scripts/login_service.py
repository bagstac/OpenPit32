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
  AUTH_USERS — one or more logins, "user1:pass1,user2:pass2". Merged with
    AUTH_USERNAME/AUTH_PASSWORD below if both are set (a duplicate username
    in AUTH_USERS wins). At least one of AUTH_USERS or AUTH_USERNAME+
    AUTH_PASSWORD is required for /auth-check to ever allow anyone in; left
    unset, it always denies rather than accidentally leaving a deployment
    open (same fail-closed behavior the old sidecar had).
  AUTH_USERNAME / AUTH_PASSWORD — a single login, kept for deployments that
    only ever needed one. Equivalent to one more entry in AUTH_USERS.
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
  EVENT_LOG_TOKEN — 2026-10-05, the "what's resetting the setpoint to 130?"
    investigation (docker/README.md's "Grill event log" section). Bearer
    token POST /log-event requires in its Authorization header — must match
    event_log_token in the ESP32's esphome/secrets.yaml exactly. Left unset,
    POST /log-event always answers 503 rather than silently accepting
    unauthenticated writes from anything that finds the port.
  EVENT_LOG_DB_PATH — where the SQLite file POST /log-event writes to
    (default: /data/grill_events.db, i.e. the same grill-data volume
    AUTH_SECRET_PATH already uses, not RBP1/pi-spare-01 — ESP32 firmware
    can't mount an NFS share or speak SQLite, and this app host has no
    existing NFS mount to that Pi, so keeping it local avoids inventing one
    for a database this small). scripts/cleanup_grill_events.py prunes it;
    see that script's own docstring for the retention policy and how to
    schedule it.
"""

from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import secrets
import sqlite3
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
DEFAULT_EVENT_LOG_DB_PATH = Path(__file__).resolve().parent / "grill_events.db"

# Keep in sync with scripts/cleanup_grill_events.py, which opens the same
# file/table by the same names rather than importing this module (it's
# meant to run standalone via `docker exec`/cron — see its own docstring).
EVENT_LOG_SCHEMA = """\
CREATE TABLE IF NOT EXISTS grill_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    received_at INTEGER NOT NULL,
    ts INTEGER,
    event TEXT NOT NULL,
    action TEXT,
    value REAL,
    old_value REAL,
    ok INTEGER,
    error TEXT,
    source_ip TEXT,
    detail TEXT
);
CREATE INDEX IF NOT EXISTS idx_grill_events_received_at ON grill_events(received_at);
"""


def open_event_log_db(path: Path) -> sqlite3.Connection:
    """One connection, opened once at startup and reused — see make_app()'s
    comment on why not "open per write": aiohttp's event loop is single-
    threaded and these writes are rare (at most a few an hour), so there's
    no concurrency to speak of; a short-lived connection per request would
    only add needless file-open overhead on a Pi's SD card for nothing a
    kept-open one doesn't already give for free.
    """
    conn = sqlite3.connect(str(path), check_same_thread=False)
    conn.executescript(EVENT_LOG_SCHEMA)
    conn.commit()
    return conn


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


def parse_users(auth_users: str | None, legacy_username: str | None,
                legacy_password: str | None) -> dict[str, str]:
    """AUTH_USERS ("user1:pass1,user2:pass2") merged with legacy AUTH_USERNAME/PASSWORD."""
    users: dict[str, str] = {}
    if legacy_username and legacy_password:
        users[legacy_username] = legacy_password
    for pair in (auth_users or "").split(","):
        pair = pair.strip()
        if not pair or ":" not in pair:
            continue
        username, _, password = pair.partition(":")
        if username and password:
            users[username] = password
    return users


def _check_credentials(users: dict[str, str], username: str, password: str) -> bool:
    return any(hmac.compare_digest(username, u) and hmac.compare_digest(password, p)
              for u, p in users.items())


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


# Columns grill_events has its own place for — anything else POST /log-event
# receives (e.g. setpoint_jump's "fahrenheit", disconnect's "mid_command")
# is kept, not dropped, as a small JSON blob in the `detail` column instead
# of growing a new column per event type's one-off field.
EVENT_LOG_KNOWN_FIELDS = {"event", "ts", "action", "value", "old_value", "ok", "error", "source_ip"}


def make_app(users: dict[str, str], auth_secret: bytes, event_log_token: str,
            event_log_db: sqlite3.Connection | None) -> web.Application:
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
        if not users:
            return web.Response(
                text="Login is not configured — set AUTH_USERS (or "
                     "AUTH_USERNAME/AUTH_PASSWORD) (see docker/README.md).",
                status=500)
        body = await request.post()
        username = str(body.get("username") or "")
        password = str(body.get("password") or "")
        next_url = _safe_next(str(body.get("next") or ""))
        ok = _check_credentials(users, username, password)
        del password, body
        if not ok:
            _LOGGER.info("login failed for username %r", username)
            await asyncio.sleep(LOGIN_FAIL_DELAY)
            raise web.HTTPFound(f"/login?error=1&next={next_url}")
        resp = web.HTTPFound(next_url)
        resp.set_cookie(SESSION_COOKIE, make_session_cookie(auth_secret, username),
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
        # wide open. The cookie doesn't carry its username in cleartext, so
        # check it against every configured user (the list is always small).
        if not users:
            return web.Response(status=401)
        cookie = request.cookies.get(SESSION_COOKIE)
        ok = any(session_cookie_valid(auth_secret, u, cookie) for u in users)
        return web.Response(status=204 if ok else 401)

    async def list_events(request):
        # Read side of the same investigation, for GrillEvents.razor — gated
        # by the ordinary browser session cookie (not EVENT_LOG_TOKEN, which
        # is the ESP32's write-only credential), checked here directly
        # rather than relying solely on nginx's auth_request: port 8091 is
        # still reachable straight from the LAN, bypassing nginx entirely
        # (see docker/README.md), so this mirrors auth_check()'s own
        # fail-closed logic instead of trusting the caller already passed
        # through it.
        if not users:
            return web.Response(status=401)
        cookie = request.cookies.get(SESSION_COOKIE)
        if not any(session_cookie_valid(auth_secret, u, cookie) for u in users):
            return web.Response(status=401)
        if event_log_db is None:
            return web.json_response({"events": []})
        try:
            limit = max(1, min(int(request.query.get("limit", "200")), 500))
        except ValueError:
            limit = 200
        rows = event_log_db.execute(
            "SELECT received_at, ts, event, action, value, old_value, ok, error, source_ip, detail "
            "FROM grill_events ORDER BY received_at DESC LIMIT ?",
            (limit,),
        ).fetchall()
        events = [
            {
                "received_at": r[0],
                "ts": r[1],
                "event": r[2],
                "action": r[3],
                "value": r[4],
                "old_value": r[5],
                "ok": bool(r[6]) if r[6] is not None else None,
                "error": r[7],
                "source_ip": r[8],
                "detail": json.loads(r[9]) if r[9] else None,
            }
            for r in rows
        ]
        return web.json_response({"events": events})

    async def log_event(request):
        # 2026-10-05 investigation (docker/README.md's "Grill event log"
        # section): a separate bearer token, not the browser session cookie
        # above — the ESP32 is not a browser and has no way to go through
        # /login. Fails closed exactly like auth_check() does when login
        # itself isn't configured: an unset EVENT_LOG_TOKEN always 503s
        # rather than silently accepting unauthenticated writes from
        # anything that finds this port.
        if not event_log_token or event_log_db is None:
            return web.Response(status=503, text="EVENT_LOG_TOKEN not configured")
        auth_header = request.headers.get("Authorization", "")
        presented = auth_header[len("Bearer "):] if auth_header.startswith("Bearer ") else ""
        if not presented or not hmac.compare_digest(presented, event_log_token):
            return web.Response(status=401)
        try:
            payload = await request.json()
        except Exception:
            return web.Response(status=400, text="invalid JSON")
        if not isinstance(payload, dict) or not payload.get("event"):
            return web.Response(status=400, text="missing 'event'")
        extra = {k: v for k, v in payload.items() if k not in EVENT_LOG_KNOWN_FIELDS}
        event_log_db.execute(
            "INSERT INTO grill_events "
            "(received_at, ts, event, action, value, old_value, ok, error, source_ip, detail) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (
                int(time.time()),
                payload.get("ts"),
                str(payload["event"])[:64],
                str(payload["action"])[:64] if payload.get("action") is not None else None,
                payload.get("value"),
                payload.get("old_value"),
                int(bool(payload["ok"])) if "ok" in payload else None,
                str(payload["error"])[:500] if payload.get("error") is not None else None,
                str(payload["source_ip"])[:64] if payload.get("source_ip") is not None else None,
                json.dumps(extra) if extra else None,
            ),
        )
        event_log_db.commit()
        return web.Response(status=204)

    app.router.add_get("/login", login_page)
    app.router.add_post("/login", login_submit)
    app.router.add_get("/logout", logout)
    app.router.add_get("/auth-check", auth_check)
    app.router.add_post("/log-event", log_event)
    app.router.add_get("/log-events", list_events)
    return app


async def main():
    port = int(os.environ.get("LOGIN_SERVICE_PORT", DEFAULT_PORT))
    secret_path = Path(os.environ.get("AUTH_SECRET_PATH", str(DEFAULT_SECRET_PATH)))

    users = parse_users(os.environ.get("AUTH_USERS"),
                        os.environ.get("AUTH_USERNAME"), os.environ.get("AUTH_PASSWORD"))
    auth_secret = load_or_create_auth_secret(os.environ.get("AUTH_SECRET"), secret_path)
    if not users:
        _LOGGER.warning(
            "AUTH_USERS/AUTH_USERNAME+AUTH_PASSWORD not set — /auth-check "
            "will always deny; fine for local dev, but "
            "docker/nginx.conf.template's login gate won't let anyone in "
            "until these are set")

    event_log_token = os.environ.get("EVENT_LOG_TOKEN", "")
    event_log_db_path = Path(os.environ.get("EVENT_LOG_DB_PATH", str(DEFAULT_EVENT_LOG_DB_PATH)))
    # Opened regardless of whether EVENT_LOG_TOKEN is set: GET /log-events
    # (GrillEvents.razor) should still work — showing an empty list — even
    # before the ESP32 side is configured to write anything.
    event_log_db = open_event_log_db(event_log_db_path)
    if not event_log_token:
        _LOGGER.warning(
            "EVENT_LOG_TOKEN not set — POST /log-event will always 503; "
            "fine if you haven't set up the ESP32's event_log_url yet (see "
            "docker/README.md's \"Grill event log\" section)")

    app = make_app(users, auth_secret, event_log_token, event_log_db)
    runner = web.AppRunner(app)
    await runner.setup()
    bind_host = os.environ.get("LOGIN_SERVICE_HOST", "127.0.0.1")
    site = web.TCPSite(runner, bind_host, port)
    await site.start()
    status = f"{len(users)} user(s) configured" if users else "NOT configured"
    print(f"login service listening on http://{bind_host}:{port} ({status})")
    try:
        await asyncio.Event().wait()
    finally:
        await runner.cleanup()
        if event_log_db is not None:
            event_log_db.close()


if __name__ == "__main__":
    asyncio.run(main())
