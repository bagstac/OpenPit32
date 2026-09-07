# OpenPit32 — Session Handoff / Current State

Read this first, then PLAN.md (history) and PROTOCOL.md (protocol facts).
The README at the repo root is the user-facing setup guide.

Updated 2026-09-07: BLE via the ESP32 proxy is the only grill transport. The
cloud-relay transport, the account login UI and all reverse-engineering
artifacts were removed (2026-09-05 publish prep). The Pit Boss cloud is
touched exactly once, by the "Pit Boss account" dialog / `scripts/pitboss_cloud.py`,
to fetch the grill's Bluetooth password.

Since publish prep, three more things landed: the app is an installable PWA
(manifest + service worker, flame-and-ripples icon set); a Docker deployment
(`docker-compose.yml`, `docker/`) runs it as two containers behind nginx,
single-origin (`/api/` reverse-proxied to the sidecar); and that Docker
deployment gates itself with a real login form + signed session cookie
(`scripts/grill_sidecar.py`'s `/login`, `/logout`, `/auth-check`), not a
browser Basic Auth popup, so a password manager can fill it. One such
deployment is exposed at `openpit32.codingattempts.com` via a Cloudflare
Tunnel running on a second host, config-file style (not the Zero Trust
dashboard), sharing a tunnel with an unrelated hostname — see
`docker/README.md` for the auth/tunnel setup and `docker/nginx.conf`'s
comments for the reverse-proxy details (notably: nginx's own `$scheme` is
always `http` since it never terminates TLS itself, so the login-redirect
Location header has to derive the external scheme from
`X-Forwarded-Proto` via a `map`, not `$scheme`, or it wrongly redirects an
HTTPS tunnel visitor to `http://`).

## TL;DR status
- Web client controls the grill over BLE through an ESP32 ESPHome
  `bluetooth_proxy`: live temps (pushed, <1 s old), set temperature, power
  (two-step confirm), prime; light hidden on models without one.
- Alarms card on the grill page: a temp-target alarm (any sensor, ≥/≤) or a
  countdown timer, delivered as a Web Push (VAPID) notification — fires even
  with the tab closed, as long as the sidecar keeps running. Backend is
  `scripts/alarms.py` (`AlarmStore`, monitor loop polled from `main()`
  alongside the grill bridge); key pair + alarms + subscriptions persist next
  to `.grill_env`. Browser side: `wwwroot/js/push.js` (subscribe/permission)
  + `wwwroot/push-worker.js` (shared `push`/`notificationclick` handling,
  imported by both service workers).
- Works with the controller switched off (mains only) — remote power-on
  from cold verified. Needs the ESP32 powered within a few metres of the
  controller with a usable BLE RSSI (> -80 dBm comfortable, < -85 flaky).
- Test unit: model PBV5 P2, control board PBV2, ESP-IDF firmware 16.8.8
  (idf v5.5.1, app pbz_firmware). The grill's BLE address is random and
  rotates — match by the advertised name (= board id `PBV2-…`).

## Services to run (2 processes, bare-metal)
1. Sidecar: `.\.venv312\Scripts\python.exe scripts\grill_sidecar.py`
   → http://127.0.0.1:8091 (GET /health /state /info /models /probe-targets,
   POST /command, POST /setup; plus /login /logout /auth-check, only
   meaningful when AUTH_USERNAME/AUTH_PASSWORD are set — see below). Reads
   scripts/.grill_env (SECRET — never print/read it into chat):
   GRILL_PROXY_HOST/KEY required; GRILL_BOARD_ID / GRILL_PASSWORD /
   GRILL_MODEL filled in by /setup. Starts fine without them
   (`configured: false`).
2. Web app: `dotnet run --project OpenPit32\OpenPit32.csproj --urls http://localhost:5219`
3. Open http://localhost:5219. First run: "Fetch grill password" → the dialog
   posts the Pit Boss account email/password to the sidecar's /setup, which
   logs in once, saves board id + password + model to .grill_env and
   connects. Afterwards the Home card → /grill (status + controls),
   /health (link diagnostics).

Alternative: `docker compose up -d --build` runs both as containers behind
nginx on one host (docker/README.md) — same sidecar, same API, plus the
login gate. `OpenPit32/Program.cs` picks the sidecar base URL at compile
time via the `DOCKER_DEPLOY` constant (`docker/web.Dockerfile` sets it),
not at runtime, after two runtime-detection approaches both proved
unreliable (see PLAN.md history if resurrecting this).

## Architecture
OpenPit32 (Blazor WASM) → GrillRpcService (typed HttpClient) → sidecar
(aiohttp) → pytboss `PitBoss(BleConnection)` → habluetooth / bleak-esphome →
ESPHome native API (TCP 6053, noise-encrypted) → ESP32 `bluetooth_proxy` →
grill GATT (Mongoose OS RPC service).
- The ESPHome API link reconnects on its own (aioesphomeapi ReconnectLogic).
- The grill GATT link does not: `GrillBridge._ble_keepalive` rescans and
  calls `BleConnection.reset_device()` with 2→60 s backoff, woken early by
  the disconnect callback.
- `GrillBridge.configure()` (re)creates the PitBoss session; it starts with
  `BleConnection(None)` so the spec loads before the grill is heard.
- State arrives as `<==PB:` debug-log notifications (push); the 10 s poll is
  a backup. Never replace a good cache with an empty decode (the firmware
  blanks its frames right after a command).
- `esphome_ble.TelemetryProxyManager` subscribes to the ESP32's own
  wifi_signal / uptime sensors for /health.

## Verified 2026-09-05 at the grill
- proxy_scan: grill at -63 dBm; ble_probe --proxy: GATT connect, RPC.Ping,
  Sys.GetInfo; authenticated calls (probe targets, firmware) OK.
- Controller OFF: BLE still answers. power_on from cold ignited the grill;
  set_temp 225/230 took; power_off worked.
- ESP32 USB pull → /health proxy_connected false → recovered, no restart.
- Spurious `Unauthorized`: the password key is derived from uptime in 10 s
  buckets and the firmware accepts current-or-next only; slow BLE writes can
  land a bucket late. Sidecar retries commands / get_state once after 1 s
  and does not surface it from the post-command refresh.
- Readings at the test spot: WiFi -58…-63 dBm (fine); BLE varied -62…-87
  between reconnects — placement facing the controller matters.

## Gotchas (do not relearn)
1. Blazor timer polls don't re-render on their own: end timer-driven
   methods with an explicit StateHasChanged().
2. habluetooth outside Home Assistant: the bare BluetoothManager's
   `_discover_service_info` is a no-op (the startup WARNING is harmless),
   so `bleak.BleakScanner` callbacks / find_device_by_filter never fire.
   Discover via `manager.async_discovered_service_info(True)` →
   `info.device` (esphome_ble.find_grill). open_proxy must also re-point
   `pytboss.ble.BleakClient*` at habluetooth's patched classes.
3. The pytboss `_on_disconnected` callback runs synchronously inside bleak:
   only flag + wake the keepalive there, never await.
4. The grill stops advertising while connected, so the proxy-side BLE RSSI
   only refreshes between connections; proxy_scan.py must run with the
   sidecar stopped.
5. bleak-esphome 4.1.0 private hooks (`_on_connect`, `_cli`) are used for
   telemetry — re-check when bumping it.
6. ESP32 hardware notes: an ESP32-C3 tried first had a WiFi fault; the
   classic ESP32-D0WD-V3 dev board is the proxy. Its auto-reset does not
   enter download mode: hold BOOT, tap EN, release BOOT, flash with
   `--before no-reset`; OTA thereafter. mDNS (`grill-proxy.local`) does not
   resolve from Windows — use the IP.
7. Secrets: scripts/.grill_env and esphome/secrets.yaml are git-ignored;
   never print them. The sidecar never logs the grill or account password.

## Files map
- scripts/: grill_sidecar.py (bridge + HTTP API, incl. the login/session
  routes and the alarms/push routes), alarms.py (AlarmStore: temp/timer
  alarms, VAPID keys, Web Push delivery, the monitor loop), esphome_ble.py
  (proxy helper), pitboss_cloud.py (one-time password fetch; CLI + used by
  /setup), proxy_scan.py (what the ESP32 hears), ble_probe.py (RPC smoke
  test), ble_scan.py (PC adapter scan), .grill_env.example.
- esphome/: grill-proxy.yaml, secrets.yaml.example.
- OpenPit32/: Pages (Home, GrillDetail `/grill` incl. the Alarms card,
  BridgeHealth `/health`), Layout (MainLayout, NavMenu, SetupDialog),
  Services/GrillRpcService.cs, Services/IncludeCredentialsHandler.cs (makes
  WASM's HttpClient send the session cookie on background /api/ calls —
  top-level nav does this on its own, background fetches don't),
  wwwroot/js/push.js (push subscribe JS interop), wwwroot/push-worker.js
  (shared `push`/`notificationclick` handling, imported by both service
  workers).
- docker/: nginx.conf (single-origin reverse proxy + auth gate),
  web.Dockerfile, sidecar.Dockerfile, README.md (deploy + auth setup),
  .env.example. docker-compose.yml lives at the repo root.
- docs/: STATE.md (this), PLAN.md (history), PROTOCOL.md.
- requirements.txt (runtime, .venv312), requirements-esphome.txt (tooling, .venv).

## Ideas / next up
- A permanent home for the ESP32 (wall USB adapter, case) facing the
  controller panel.
- Probe-target UI, °F/°C toggle.
- Alarms card ships one-shot alarms only (no repeat/snooze); could add
  re-arming after a manual "done" ack if that turns out to matter in use.
- The grill was powered OFF at the end of the 2026-09-05 session; check
  `/state` moduleIsOn before assuming anything.

## Resume checklist
1. Start sidecar + web app; GET http://127.0.0.1:8091/health should show
   configured + connected + proxy_connected.
2. Safe tests only: set_temp / prime. NEVER power_on unless the user is
   ready (it ignites).
