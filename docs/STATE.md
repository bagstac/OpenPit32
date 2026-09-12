# OpenPit32 — Session Handoff / Current State

Read this first. `docs/ESP32_FIRMWARE_PLAN.md` has the full phase-by-phase
history of how this got here (each phase's real hardware verification, every
bug found and fixed along the way) and `docs/PROTOCOL.md` has the protocol
facts. `docs/PLAN.md` is the *original* sidecar-architecture planning doc —
left as historical record, not current state; everything it planned has
since been superseded by the ESP32 migration below. The README at the repo
root is the user-facing setup guide.

**Updated 2026-09-10 (Phase 8 complete): the Python sidecar is gone.** The
grill's own ESP32 (`esphome/grill-firmware.yaml`) now owns 100% of BLE,
command dispatch, alarm evaluation, and Telegram alerting directly — no
PC-side Bluetooth process, no `bluetooth_proxy`, no `pytboss`/`bleak`/
`habluetooth`/`aioesphomeapi` dependency at all anymore. What's left on the
Python side is `scripts/login_service.py`, ~150 lines that do nothing but
the session-cookie login Docker deployments gate behind (`/login`,
`/logout`, `/auth-check`) — unused entirely for a bare-metal/local-dev run,
since there's no login gate without nginx in front.

## TL;DR status
- Web client (Blazor WASM) talks to the grill's ESP32 directly over plain
  HTTP/JSON — `GET/POST` `/health`, `/state`, `/info`, `/config`,
  `/command`, `/alarms` (+`DELETE /alarms/{id}`), `/setup`. Live temps
  pushed by the grill's own debug-log channel (<1 s old), set temperature,
  power (two-step confirm), no light/prime on this grill (PBV5 P2 has
  neither).
- Alarms card on the grill page: a temp-target alarm (any sensor, ≥/≤) or a
  countdown timer, delivered as a **Telegram message** — fires even with no
  browser open at all, anywhere, independent of any device/subscription
  state (no VAPID, no service-worker push, that whole approach was dropped
  — see the plan's decision #3). Evaluated by the ESP32 itself on a 5 s
  timer; alarms persist across a reboot/reflash (NVS).
- Works with the controller switched off (mains only) — remote power-on
  from cold verified. Needs the ESP32 powered within a few metres of the
  controller with a usable BLE RSSI (> -80 dBm comfortable, < -85 flaky).
- Test unit: model PBV5 P2, control board PBV2, ESP-IDF firmware 16.8.8
  (idf v5.5.1, app pbz_firmware), 3 meat probes, no light. The grill's BLE
  address is random and rotates — the ESP32 matches by advertised name
  prefix (`PBV2-`).

## Services to run (bare-metal, 1 process)
1. Flash `esphome/grill-firmware.yaml` to the ESP32 (see its own top
   comment for the USB BOOT/EN dance; OTA after that). `esphome/secrets.yaml`
   needs `wifi_ssid`/`wifi_password`/`api_key`/`ota_password` at minimum;
   `grill_password`/`telegram_bot_token`/`telegram_chat_id` are optional
   now (POST /setup / POST /config can set them at runtime instead, and NVS
   persists whichever wins — see the plan's Phase 7 writeup).
2. Web app: edit `GRILL_HOST` in `OpenPit32/Program.cs` to the ESP32's LAN
   IP (no reverse proxy in local dev, so the app talks to it directly —
   there's no login gate here either, that's Docker/nginx-only), then
   `dotnet run --project OpenPit32\OpenPit32.csproj --urls http://localhost:5219`.
3. Open http://localhost:5219. First run: **Fetch grill password** → the
   dialog posts the Pit Boss account email/password straight to the ESP32's
   own `POST /setup`, which logs in once, saves the password to its own
   flash (NVS), and forgets the account login. Nothing is stored in the
   browser or on the PC. Afterwards the Home card → `/grill` (status +
   controls), `/health` (link diagnostics).

Alternative: `docker compose up -d --build` runs the web app + a small
login-only auth service as two containers behind nginx on one host
(`docker/README.md`) — nginx fans `/api/*` out to the ESP32 directly for
every grill/alarm route, and to the auth container only for
`/login`/`/logout`/`/auth-check`. `OpenPit32/Program.cs` picks the API base
URL at compile time via the `DOCKER_DEPLOY` constant (`docker/web.Dockerfile`
sets it), not at runtime, after two runtime-detection approaches both
proved unreliable (see `docs/PLAN.md` history if resurrecting this).

## Architecture
```
OpenPit32 (Blazor WASM) → GrillRpcService (typed HttpClient)
    │ /health /state /info /config /command /alarms /setup
    ▼
grill's ESP32 (esphome/grill-firmware.yaml, pitboss_grill component)
    │ native BLE central, Mongoose OS RPC-over-GATT
    ▼
grill controller (Mongoose OS on ESP32, GATT)
```
Plus, only when deployed via Docker (nginx in front, gating `/login`,
`/logout`, `/auth-check` and nothing else): `scripts/login_service.py`, a
signed-session-cookie login form, holding no grill state of any kind.

- State arrives as `<==PB:` debug-log notifications (push, sub-second),
  decoded the same way as the authenticated 15 s `PB.GetState` poll that
  backs it up — never replaces a good cache with an empty decode (the
  firmware blanks its own frames right after a command).
- The BLE link auto-reconnects (`set_auto_connect(true)`); a disconnect
  clears in-flight RPC state so the next cycle recovers cleanly rather than
  wedging (`gattc_event_handler()`'s `ESP_GATTC_DISCONNECT_EVT` case).
- `pitboss_grill.cpp` is a from-scratch C++ port of `pytboss`'s protocol
  knowledge (auth codec, RPC framing, per-board frame decode) — not a
  wrapper around the Python library, which is gone from the runtime
  entirely. Credit belongs to `pytboss` regardless; see README.md.

## Gotchas (do not relearn)
1. Blazor timer polls don't re-render on their own: end timer-driven
   methods with an explicit `StateHasChanged()`.
2. ESP32 hardware notes: an ESP32-C3 tried first had a WiFi fault; the
   classic ESP32-D0WD-V3 dev board is what's deployed. Its auto-reset does
   not enter download mode: hold BOOT, tap EN, release BOOT, flash with
   `--before no-reset` (full command in `grill-firmware.yaml`'s header);
   OTA thereafter. mDNS (`grill-firmware.local`) does not reliably resolve
   from Windows — use the IP.
3. Secrets: `esphome/secrets.yaml` is git-ignored; never print it. The
   grill/account passwords are never logged (ESP32 side: `handle_setup_()`
   clears local password copies as soon as they're used; Python side:
   `login_service.py` never touches a grill password at all anymore).
4. PWA service worker updates: without `skipWaiting()` (on install) and
   `clients.claim()` (on activate), a browser that already has this app's
   service worker keeps serving the OLD cached shell after a redeploy until
   every open tab/PWA window is fully closed — Blazor fingerprints framework
   files per publish, so the stale shell fetches files that no longer exist
   and the app looks like it's simply broken (seen in Chrome on Android
   right after a rebuild; Vivaldi worked because it had never cached this
   origin before). Both service workers call both, so a redeploy takes over
   immediately instead of requiring a manual "clear site data".

   **Part two of this, found live 2026-09-12**: `skipWaiting()`/
   `clients.claim()` only help once the browser actually *notices* there's a
   new service worker to install — and it wasn't, even across multiple
   plain refreshes. `service-worker.js` registered with no `updateViaCache`
   option (default: `'imports'`), which lets `service-worker.published.js`'s
   `importScripts('./service-worker-assets.js')` — the one file that
   actually changes on every publish (new content-hashed filenames);
   `service-worker.js` itself is a static wrapper whose bytes never change —
   be served from the browser's own HTTP cache during the update check. The
   browser's required byte-comparison could then see "nothing changed" even
   though the whole point of that file is that it did, so the
   skipWaiting()/clients.claim() fix above never even got triggered. Fixed
   with `register('service-worker.js', { updateViaCache: 'none' })` in
   `index.html`, plus an explicit `Cache-Control: no-cache` on both service
   worker files in `docker/nginx.conf.template` as a second layer. Real
   catch: this fix lives inside the same `index.html` that was itself stuck
   being served stale, so anyone already wedged by this needs one manual
   cache-clear to receive the fix that prevents it recurring — that's a
   one-time cost, not evidence the fix didn't work.
5. **NVS writes must never happen directly on the ESP32's httpd request
   task** — confirmed live 2026-09-10: `POST /alarms` calling
   `nvs_set_str()`/`nvs_commit()` inline stack-overflowed the ESP-IDF
   httpd task (small, ~4KB default stack, not configurable from YAML) and
   crash-looped the device (it self-recovered each time, no physical
   power-cycle needed — different from an earlier Phase 4 failure mode).
   Every REST handler that persists something now defers the actual flash
   write to the main loop task instead (`nvs_save_string_deferred_()` /
   `save_alarms_locked_()`'s "call from the main loop only" contract) — see
   the Phase 7 writeup in `docs/ESP32_FIRMWARE_PLAN.md` for the full
   root-cause/fix writeup before touching that code again.
6. The Docker `auth` container's persisted session-signing secret
   (`AUTH_SECRET_PATH`, default `/data/.auth_secret`) deliberately uses the
   *same* path inside the *same* `grill-data` volume the old `sidecar`
   container used for the identical file — changing either would silently
   invalidate every existing login session on the next deploy. Keep them in
   sync if either ever moves.
7. **A stuck RPC reply used to wedge `POST /command` with "grill busy"
   forever** — fixed 2026-09-11 with a `loop()` watchdog
   (`RPC_REPLY_TIMEOUT_MS`) that force-clears an in-flight request past 5s
   and retries a command's own request once. Root cause: this firmware
   collapsed the old sidecar's two-chip split (a PC with no radio
   constraints, plus a *dedicated* quiet BLE-only proxy ESP32) onto one
   ESP32 with one shared 2.4GHz radio serving both the grill's BLE link
   *and* every bit of WiFi traffic (the browser's poll, mDNS, SNTP,
   Telegram) — an RPC write using `ESP_GATT_WRITE_TYPE_NO_RSP` (confirmed
   the *only* write type this GATT server actually tolerates — switching to
   WRITE_TYPE_RSP caused real disconnects, reverted) that loses that
   radio-time race is just silently gone. `post_connect_roaming` (ESPHome's
   periodic "check for a better AP" WiFi scan, which this board's signal
   always triggered) was one concrete collision, disabled outright; cutting
   general WiFi chatter — `mdns: disabled: true`, and `GrillDetail.razor`/
   `Home.razor`'s poll interval 5s→10s — closed the rest of it: 10/10
   `POST /command` calls succeeded live afterward with zero timeouts. See
   the 2026-09-11 follow-up in `docs/ESP32_FIRMWARE_PLAN.md` for the full
   writeup, including a real mistake made verifying this (ran `power_off`
   tests without checking `/state` first — no cook was actually
   interrupted, but check current state before assuming a command is
   "safe to test," never assume from earlier in the same conversation).
8. `pkill -f "esphome logs"` does not reliably kill background log
   processes in this environment (Windows + Git Bash) — several piling up
   exhausts the ESPHome API's 5-connection cap and produces a
   rapid-reconnect symptom that looks exactly like a firmware crash but
   isn't (confirmed: `/health` answered normally the instant the stale
   client processes were force-killed). `taskkill /F /IM python.exe`
   (or killing by tracked PID) actually works; `pkill -f` does not.

## Files map
- `scripts/login_service.py` — the entire Python surface now: login form +
  session cookie + nginx's `/auth-check` target. No grill/BLE code of any
  kind.
- `esphome/grill-firmware.yaml`, `esphome/components/pitboss_grill/`
  (`__init__.py`, `pitboss_grill.h`/`.cpp`) — the grill firmware itself;
  owns everything grill-related. `esphome/secrets.yaml.example`.
- `OpenPit32/`: Pages (Home, GrillDetail `/grill` incl. the Alarms card,
  BridgeHealth `/health`), Layout (MainLayout, NavMenu, SetupDialog),
  `Services/GrillRpcService.cs`, `Services/IncludeCredentialsHandler.cs`
  (makes WASM's HttpClient send the session cookie on background `/api/`
  calls, Docker-only — top-level nav does this on its own, background
  fetches don't; harmless no-op against the ESP32 directly in local dev,
  which has no cookies to send).
- `docker/`: `nginx.conf.template` (single-origin reverse proxy: most
  routes straight to the ESP32, `/login`/`/logout`/`/auth-check` to
  `auth`), `web.Dockerfile`, `auth.Dockerfile`, `README.md` (deploy + auth
  setup), `.env.example`. `docker-compose.yml` lives at the repo root.
- `docs/`: STATE.md (this), `ESP32_FIRMWARE_PLAN.md` (the real history —
  every phase, every bug found and fixed, all bench-verified against the
  real grill), `PLAN.md` (superseded original sidecar plan, historical
  only), `PROTOCOL.md`.
- `requirements.txt` (runtime, `.venv312` — just `aiohttp` now),
  `requirements-esphome.txt` (tooling, `.venv`).

## Ideas / next up
- A permanent home for the ESP32 (wall USB adapter, case) facing the
  controller panel.
- Probe-target UI, °F/°C toggle (see the plan's decision #6 on why
  probe-targets specifically isn't planned — alarms already cover the same
  need).
- Alarms ship one-shot only (no repeat/snooze); could add re-arming after a
  manual "done" ack if that turns out to matter in use.
- `POST /config`'s `telegram_bot_token`/`telegram_chat_id` fields have no
  web-app UI yet (curl them directly) — a "Notifications" card mirroring
  the existing "Link Settings" card (`error_display_threshold`) would be
  the natural next step, whenever wanted.

## Resume checklist
1. `curl http://<esp32-ip>/health` should show `configured: true`,
   `connected: true`.
2. Safe tests only: `set_temp` / status reads. NEVER `power_on` unless the
   user is ready (it ignites a real appliance).
