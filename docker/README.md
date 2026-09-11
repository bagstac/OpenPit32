# Running OpenPit32 with Docker

Two containers, both on one host (a Raspberry Pi, a NAS, a home server —
anything on the same LAN as the grill's ESP32):

- `auth` — the login-only service (`scripts/login_service.py`), port 8091.
  Everything grill-specific (BLE, commands, alarms, notifications) lives on
  the ESP32 itself now (`esphome/grill-firmware.yaml`) — this container's
  only job is the session-cookie login nginx gates the whole app behind.
- `web` — the Blazor app, served by nginx on port 5219 by default. nginx
  also reverse-proxies `/api/` — mostly straight to the grill's ESP32
  directly, with `/login`, `/logout`, `/auth-check` going to `auth` — so the
  browser only ever talks to one origin, one port.

## Deploy

```
scp -r . user@host:~/openpit32          # or git clone there directly
ssh user@host
cd ~/openpit32
cp docker/.env.example .env
nano .env   # HOST_IP, AUTH_USERNAME, AUTH_PASSWORD, GRILL_PROXY_HOST
docker compose up -d --build
```

`AUTH_USERNAME` / `AUTH_PASSWORD` are the login the app requires — a real
HTML form (`scripts/login_service.py`'s `/login`), not a browser Basic Auth
popup, so a password manager can offer to save and fill it. Open
`http://<HOST_IP>:5219`, log in, then click **Fetch grill password…** and
sign in with your Pit Boss account once. That calls the ESP32's own
`POST /setup` directly (Phase 7, `docs/ESP32_FIRMWARE_PLAN.md`) — the
password lands in the ESP32's own flash (NVS), not in this Docker stack at
all, so it survives an ESP32 OTA reflash the same way it survives a
`docker compose restart` here.

Changing the login password later: edit `AUTH_PASSWORD` in `.env` and
`docker compose up -d` (recreates the `auth` container only, picks up the
new value; nothing to rebuild). Existing sessions stay valid — the cookie
isn't the password, just proof someone once knew it — so also set
`AUTH_SECRET` to any new value if you want to force everyone to log in
again immediately (e.g. you suspect the login is compromised).

## Updating

```
git pull   # or re-copy the changed files
docker compose up -d --build
```

The login's session-signing key lives in the `grill-data` volume, not the
image, so a rebuild doesn't lose it and nobody gets logged out. (The grill
password and alarm/Telegram config used to live here too, back when a
Python sidecar held the BLE session — as of Phase 7/8 those are entirely on
the ESP32's own flash instead, so this volume only holds that one signing
key now.) `docker compose down -v` does lose it — only use `-v` if you want
to force a fresh login for everyone.

## Exposing it over the internet (e.g. a Cloudflare Tunnel)

Point the tunnel's public hostname at `http://<HOST_IP>:5219` on this
machine — plain HTTP is fine; the tunnel handles TLS at the edge. Nothing
else needs a public hostname: the `auth` container's own port (8091) is
only for LAN diagnostics and shouldn't be exposed. nginx already requires
the login from the step above for every request — the page and its API
calls alike — so the tunnel doesn't need its own auth layer on top unless
you want one.

## Notes

- The `auth` container's `CORS_ORIGINS` (from `HOST_IP`/`WEB_PORT` in
  `.env`) only matters for LAN-direct access to port 8091 for diagnostics;
  the browser's normal path through nginx is same-origin and doesn't use it.
- Nothing here needs Bluetooth hardware on the Docker host at all — that's
  entirely the ESP32's job now. `web` needs LAN access to it on TCP 80
  (HTTP) for `/api/health`, `/api/state`, `/api/info`, `/api/config`,
  `/api/command`, `/api/alarms`, `/api/setup` (`GRILL_PROXY_HOST`); `auth`
  needs no access to the grill at all.
- `.env` holds the app's login; git-ignored, same as `esphome/secrets.yaml`
  for a bare-metal ESP32 flash.
