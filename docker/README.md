# Running OpenPit32 with Docker

Two containers, both on one host (a Raspberry Pi, a NAS, a home server —
anything on the same LAN as the grill and its ESP32 proxy):

- `sidecar` — the Python bridge to the grill over Bluetooth LE, port 8091.
- `web` — the Blazor app, published as static files and served by nginx,
  port 5219 by default.

The browser talks to both directly (it loads the page from `web` and calls
`sidecar` on its own port), so pick a `HOST_IP` the browser can actually
reach — usually just this machine's LAN address.

## Deploy

```
scp -r . user@host:~/openpit32          # or git clone there directly
ssh user@host
cd ~/openpit32
cp docker/.env.example .env
nano .env                                # HOST_IP, GRILL_PROXY_HOST, GRILL_PROXY_KEY
docker compose up -d --build
```

Open `http://<HOST_IP>:5219`, click **Pit Boss account…**, and sign in once
to fetch the grill's Bluetooth password — it's saved into the `grill-data`
volume, so it survives a `docker compose restart` or a reboot.

## Updating

```
git pull   # or re-copy the changed files
docker compose up -d --build
```

The grill password lives in the `grill-data` volume, not the image, so a
rebuild doesn't lose it. `docker compose down -v` does — only use `-v` if
you want to re-run setup.

## Notes

- The sidecar's `GRILL_SIDECAR_ORIGINS` is derived from `HOST_IP`/`WEB_PORT`
  in `.env`; if the browser's origin doesn't match, `/setup` and the live
  status calls fail with a CORS error in the browser console.
- Nothing here needs Bluetooth hardware on the Docker host — that's the
  ESP32's job. The sidecar only needs LAN access to it (TCP 6053).
- `.env` holds the ESP32's API key; it's git-ignored, same as
  `scripts/.grill_env` and `esphome/secrets.yaml` for a bare-metal run.
