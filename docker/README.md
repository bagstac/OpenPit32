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
nano .env                                # HOST_IP, GRILL_PROXY_HOST, GRILL_PROXY_KEY
```

Create the login nginx will require for the whole app (do this on the
Docker host itself, over your own SSH session — never pass the password
through anything else):

```
printf "yourusername:$(openssl passwd -apr1)\n" > docker/.htpasswd
```

It prompts for the password twice (hidden input) and writes the hash —
`docker/.htpasswd` is git-ignored, same as `.env`.

```
docker compose up -d --build
```

Open `http://<HOST_IP>:5219` — the browser will prompt for that
username/password before showing anything — then click **Pit Boss
account…** and sign in once to fetch the grill's Bluetooth password. It's
saved into the `grill-data` volume, so it survives a `docker compose
restart` or a reboot.

## Updating

```
git pull   # or re-copy the changed files
docker compose up -d --build
```

The grill password lives in the `grill-data` volume, not the image, so a
rebuild doesn't lose it. `docker compose down -v` does — only use `-v` if
you want to re-run setup. `docker/.htpasswd` is a bind-mounted file, not
baked into the image either, so it survives rebuilds the same way.

## Exposing it over the internet (e.g. a Cloudflare Tunnel)

Point the tunnel's public hostname at `http://<HOST_IP>:5219` on this
machine — plain HTTP is fine; the tunnel handles TLS at the edge. Nothing
else needs a public hostname: the sidecar's own port (8091) is only for LAN
diagnostics and shouldn't be exposed. Since nginx already requires the
username/password from the step above for every request — the page and its
API calls alike — the tunnel doesn't need its own auth layer on top unless
you want one.

## Notes

- The sidecar's `GRILL_SIDECAR_ORIGINS` (from `HOST_IP`/`WEB_PORT` in
  `.env`) only matters for LAN-direct access to port 8091 for diagnostics;
  the browser's normal path through nginx is same-origin and doesn't use it.
- Nothing here needs Bluetooth hardware on the Docker host — that's the
  ESP32's job. The sidecar only needs LAN access to it (TCP 6053).
- `.env` holds the ESP32's API key and `docker/.htpasswd` holds the app's
  login hash; both are git-ignored, same as `scripts/.grill_env` and
  `esphome/secrets.yaml` for a bare-metal run.
