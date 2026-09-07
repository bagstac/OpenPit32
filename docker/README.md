# Running OpenPit32 with Docker

Two containers, both on one host (a Raspberry Pi, a NAS, a home server —
anything on the same LAN as the grill and its ESP32 proxy):

- `sidecar` — the Python bridge to the grill over Bluetooth LE, port 8091.
- `web` — the Blazor app, served by nginx on port 5219 by default. nginx
  also reverse-proxies `/api/` to the sidecar and requires the login set up
  below, so the browser only ever talks to one origin, one port.

## Deploy

```
scp -r . user@host:~/openpit32          # or git clone there directly
ssh user@host
cd ~/openpit32
cp docker/.env.example .env
nano .env   # HOST_IP, AUTH_USERNAME, AUTH_PASSWORD, GRILL_PROXY_HOST, GRILL_PROXY_KEY
docker compose up -d --build
```

`AUTH_USERNAME` / `AUTH_PASSWORD` are the login the app requires — a real
HTML form (`scripts/grill_sidecar.py`'s `/login`), not a browser Basic Auth
popup, so a password manager can offer to save and fill it. Open
`http://<HOST_IP>:5219`, log in, then click **Pit Boss account…** and sign
in once to fetch the grill's Bluetooth password. It's saved into the
`grill-data` volume, so it survives a `docker compose restart` or a reboot
— the login session (30 days) does too, since its signing key is generated
once and persisted in that same volume rather than regenerated on restart.

Changing the password later: edit `AUTH_PASSWORD` in `.env` and
`docker compose up -d` (recreates the sidecar only, picks up the new
value; nothing to rebuild). Existing sessions stay valid — the cookie
isn't the password, just proof someone once knew it — so also set
`AUTH_SECRET` to any new value if you want to force everyone to log in
again immediately (e.g. you suspect the login is compromised).

## Updating

```
git pull   # or re-copy the changed files
docker compose up -d --build
```

The grill password and the login's signing key both live in the
`grill-data` volume, not the image, so a rebuild doesn't lose either.
`docker compose down -v` does — only use `-v` if you want to re-run setup
and force a fresh login for everyone.

## Exposing it over the internet (e.g. a Cloudflare Tunnel)

Point the tunnel's public hostname at `http://<HOST_IP>:5219` on this
machine — plain HTTP is fine; the tunnel handles TLS at the edge. Nothing
else needs a public hostname: the sidecar's own port (8091) is only for LAN
diagnostics and shouldn't be exposed. nginx already requires the login from
the step above for every request — the page and its API calls alike — so
the tunnel doesn't need its own auth layer on top unless you want one.

## Notes

- The sidecar's `GRILL_SIDECAR_ORIGINS` (from `HOST_IP`/`WEB_PORT` in
  `.env`) only matters for LAN-direct access to port 8091 for diagnostics;
  the browser's normal path through nginx is same-origin and doesn't use it.
- Nothing here needs Bluetooth hardware on the Docker host — that's the
  ESP32's job. The sidecar only needs LAN access to it (TCP 6053).
- `.env` holds the ESP32's API key and the app's login; git-ignored, same
  as `scripts/.grill_env` and `esphome/secrets.yaml` for a bare-metal run.
