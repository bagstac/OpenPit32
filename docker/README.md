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

Adding another person with their own login: set `AUTH_USERS` in `.env` to
`username:password` pairs separated by commas, e.g.
`AUTH_USERS=alice:hunter2,bob:correcthorse`, then `docker compose up -d`
(recreates `auth` only). It's merged with `AUTH_USERNAME`/`AUTH_PASSWORD` if
those are still set, so the original login keeps working unless you also
give it a username already listed in `AUTH_USERS` (that entry wins). Each
person logs in with their own username/password; sessions aren't tied to
which one they used, so revoking a single person's access means changing
their entry (or everyone's `AUTH_SECRET`, which logs out all of them).

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

## Grill event log

> **FIXED 2026-10-12 — the crash below came from doing the POST on the httpd
> task; `log_event_()` is now deferred onto the main loop like every other
> caller.**
>
> Original symptom: with `event_log_url` set, the ESP32 reset on its own event
> POST — `Fault - LoadProhibited` in lwip's `tcp_new_port` (tcp.c:1035, the
> `tcp_pcb` list walk, *after* its NULL guard), reached via
> `HttpRequestComponent::post` → `HttpRequestIDF::perform` →
> `esp_http_client_open` → `tcp_connect`. It rebooted every ~60-120 s while
> commands were being issued, taking the live grill monitor offline with each
> reset — and the crashing request was the one that should have written the
> event, so `grill_events.db` never received a single row.
>
> Cause: `handle_command_()` runs on the httpd task and called `log_event_()`
> *inline*, and that call blocks for a whole HTTP round trip (its own
> `esp_http_client`, with its own watchdog manager and 512-byte buffers). The
> httpd task is a small-stack task, so that stack ran past its end into
> adjacent heap — and adjacent heap is where lwip keeps its PCBs, hence a
> `tcp_pcb` list holding garbage. Every other `log_event_()` caller (connect,
> disconnect, setpoint_jump) already deferred onto the main loop for exactly
> this reason; the command path was the one that didn't. `notify_()`'s
> Telegram POST has the same shape and *is* exercised: a real alarm was fired on
> 2026-10-12 and the message was delivered with the box staying up (uptime 663 s
> and climbing, no reset, no crash report). That is the independent evidence
> that the POST itself was never the problem — only the task it ran on, which is
> the whole reason the fix is a `defer()` and not a change to the request code.
>
> Verified after the fix: two deliberate `set_temp` commands, each carrying
> the setpoint the grill already had so nothing about the burn changed. The
> device stayed up through both (uptime 31→94 s, then 119→182 s; the old
> build died inside ~70 s), and each command landed in `grill_events.db` and
> came back through `GET /api/events` on both the LAN and the public
> hostname, with the right `action`, `value`, `ok` and `source_ip`.
>
> Fixed 2026-10-13 (`3e70501`): that boot-time connect was the one event that
> ALWAYS vanished — the BLE link to the grill is already up (`gattc_calls=774`,
> tracker `active: 1` in a captured boot dump) before `http_request` and WiFi
> are usable, so `log_event_()` took its "http_request component missing" early
> return and left no row and no error. It now serializes the body, tries the
> POST, and on failure queues it in a bounded backlog (8 events, oldest dropped
> with a warning) retried from the main loop every 15 s — one event per tick,
> oldest first, so an unreachable sink costs one client timeout per interval
> rather than stalling the queue. Nothing is silently dropped any more, which
> covers a transient network blip as well as boot. Verified on the device: after
> an OTA reboot the boot-time connect landed as a row (`10:13:00Z`, the first
> this log has ever captured) with `Event log backlog: one sent, 0 still
> pending` in the device log, and a deliberate `set_temp` still wrote its row on
> the first attempt.

2026-10-05: set up to answer "what's resetting the setpoint to 130?" — was
it a stray command from the real Pit Boss app/cloud, or something in this
project? Until now, nothing here logged who/what sent a command at all.

With `EVENT_LOG_TOKEN` set in `.env` and the matching `event_log_url`/
`event_log_token` uncommented in `esphome/grill-firmware.yaml` (pointed at
`http://<HOST_IP>:8091/log-event` — the `auth` container's own LAN port,
not through nginx, since this isn't a browser request and doesn't need the
login gate), the ESP32 itself POSTs one JSON event, bearer-authenticated,
for each of:

- every `POST /command` (`power_on`/`power_off`/`set_temp`), with the
  action, value, whether it succeeded, and the caller's IP;
- every BLE connect/disconnect to the grill — lets you tell "the official
  app took the one BLE connection instead" apart from "a command came
  through this project's own `/command`";
- any setpoint change the ESP32 observes that *isn't* explained by a
  command it just dispatched itself — the strongest signal, since it
  catches a reset coming from somewhere this project never touches at all
  (the real app connecting directly, the Pit Boss cloud relay, the grill's
  own auto-shutoff).

`auth` (`scripts/login_service.py`) writes these to a small SQLite file —
`grill_events.db` in the same `grill-data` volume the session-signing
secret already lives in, **not** RBP1/pi-spare-01: the ESP32 can't mount
an NFS share or speak SQLite, and this app host (`srv-mc-01`) has no
existing NFS mount to that Pi (the one that exists today only reaches
`pi-print-01`, for an unrelated app's database) — so keeping this one
local avoids inventing a cross-host dependency for a database this small.

Inspect it directly on the host:

```
docker compose exec auth sqlite3 /data/grill_events.db \
    "SELECT datetime(received_at, 'unixepoch'), event, action, value, ok, error, source_ip FROM grill_events ORDER BY received_at DESC LIMIT 20;"
```

Only events worth looking at are ever written here — never routine status
polling — so there's no further "keep only the interesting ones"
filtering to do. What still needs bounding is how far back even that
smaller set is kept, on a host with limited disk: `scripts/
cleanup_grill_events.py` deletes rows older than `EVENT_LOG_RETENTION_DAYS`
(30), and isn't run automatically by anything in this repo — same
limitation as the `docker builder prune` cron job below, since nothing in
this Docker stack can schedule something on the host itself. Deployed on
srv-mc-01 (2026-10-13) as one crontab line calling `scripts/
grill-event-log-prune.sh`, which runs the prune only in the hour that is
05:00 in America/New_York:

```
# crontab -e (as brian, the user docker compose runs under)
0 * * * * /home/brian/openpit32/scripts/grill-event-log-prune.sh >>/home/brian/openpit32-cleanup.log 2>&1
```

Hourly-plus-a-gate rather than `0 9 * * *`, because that box runs
`Etc/UTC` and Ubuntu's cron is vixie 3.0pl1 (checked: no `CRON_TZ`), so
any fixed UTC hour is an hour out for half the year — 09:00 UTC is 05:00
EDT but 04:00 EST. A systemd timer with
`OnCalendar=*-*-* 05:00:00 America/New_York` would be exact too (that
syntax does work here — `systemd-analyze calendar` confirms it), but
installing one needs root and this box only takes a passworded sudo. The
gate reads the Eastern wall clock instead, so it stays at 05:00 local
across the DST change with no twice-a-year edit, and it keeps working if
the box is ever moved to local time (an open question in the fleet docs).
Verified 2026-10-13 with a one-minute self-test entry: cron ran the whole
chain (cron → `docker compose exec` → the script) and logged
`deleted 0 row(s) older than 30 days`.

## Disk space on a small host (e.g. a Raspberry Pi)

Found live 2026-09-13 on the Pi this actually runs on: a run of same-day
redeploys (each `docker compose up -d --build web` after a code change)
had quietly filled a 14GB SD card to 83% — not container logs (still under
a few hundred KB each at the time), but **5.5GB of dead `docker builder`
cache**, an old layer left behind by every rebuild and never evicted on
its own. `docker system df` shows the breakdown (`Build Cache` row,
`RECLAIMABLE` column); `docker builder prune -af` reclaims it immediately
— safe any time, since it's only cached build layers, never anything a
running container or the `grill-data` volume depends on (worst case, the
next build is slower for having to redo that layer).

To stop this recurring silently, a cron job on the host runs that
automatically:

```
# crontab -e (as the user docker compose runs under)
0 4 * * 0 docker builder prune -af --filter "until=168h" >/dev/null 2>&1
```

Weekly, keeps anything built in the last 7 days (so a same-day rebuild
still hits a warm cache) and evicts everything older. Set up once by hand
on the host — this repo has no mechanism to install it for you, and
`docker-compose.yml` doesn't run on the host outside a container so it
can't schedule anything on the host itself either.

Separately, both services set an explicit `logging.options.max-size`/
`max-file` in `docker-compose.yml` (10MB x 3 files each) — the Docker
daemon's default `json-file` driver has no cap at all otherwise, and
`web`'s nginx access log records every request, including every poll
from every open tab.
