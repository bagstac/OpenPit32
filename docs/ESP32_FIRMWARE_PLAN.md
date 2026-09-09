# Plan: move all grill control onto the ESP32

Goal, from a 2026-09-09 discussion: collapse the current three-hop
architecture (web app → Python sidecar → ESP32 BLE proxy → grill) down to
two real components — a thin web-facing edge, and an ESP32 that owns 100%
of the Bluetooth protocol, command dispatch, alarm evaluation, and alerting.
Current state / why the sidecar exists at all today: `docs/STATE.md`.
Protocol facts this plan leans on: `docs/PROTOCOL.md`.

This is a plan, not yet started. Nothing here has been implemented.

## Decisions made (2026-09-09)

Asked as clarifying questions before writing this plan; answers below shape
everything that follows.

1. **Login/serving stays off the ESP32.** The web-app component keeps
   serving the static Blazor files, terminating the Cloudflare Tunnel, and
   gating login. The ESP32 owns 100% of BLE, command, and alarm logic —
   nothing web-facing-only lives there.
2. **Remote access is still required** — keep the Cloudflare Tunnel path
   working exactly as it does today (`docker/README.md`).
3. **Notifications: a webhook relay, not Web Push/VAPID.** Implementing
   RFC 8291's push encryption (ECDH + HKDF + AES-128-GCM) natively in C on
   the ESP32 was judged the single highest-effort, highest-risk piece of
   this whole migration for what it buys — a plain HTTPS POST to a relay
   service gets the same result with none of that. **Telegram's Bot API for
   now**, explicitly as a placeholder to prove the pattern out — the plan
   below keeps the sending code to one small, swappable function so moving
   to ntfy.sh or Pushover later is a one-function change, not a redesign.
4. **Static files: a bare-bones container on the Pi** (nginx, no app code),
   not an external static host — keeps the existing Cloudflare Tunnel /
   Docker Compose deployment model unchanged.
5. **Login stays on the web app** (decided 2026-09-09) — keep today's custom
   login page rather than switching to Cloudflare Access. The web-app
   container runs nginx plus a small login-only process: today's
   `scripts/grill_sidecar.py` login/session code (`/login`, `/logout`,
   `/auth-check`, the signed-cookie logic), everything grill-specific
   stripped out — roughly 150 lines. Password-manager-friendly form,
   30-day sessions, and the `AUTH_SECRET` invalidation story all carry over
   unchanged from today's implementation.

## Target architecture

```
Browser ──HTTPS──> Cloudflare Tunnel ──> nginx (Pi, "web" container)
                                            │  serves static Blazor files
                                            │  proxy_pass /api/* ──────────┐
                                            ▼                              ▼
                                    auth-check ──────>          ESP32 (custom firmware)
                                    (login/session,                       │
                                     tiny process,                        ├─ BLE session + Pit Boss RPC protocol
                                     "web" or its own                     ├─ REST JSON API (mirrors today's sidecar)
                                     small container)                     ├─ alarm monitor loop
                                                                          ├─ Telegram POST on alarm fire
                                                                          └─ one-time cloud password fetch
```

nginx keeps doing exactly what it does today; the only real change is what
`proxy_pass` points at. Because the ESP32's REST API mirrors the sidecar's
existing shape, **`OpenPit32/Services/GrillRpcService.cs` and the rest of the
Blazor frontend need no changes** — only `docker/nginx.conf`'s `set $sidecar`
target moves from the sidecar container to the ESP32's LAN address/port. The
ESP32 never needs its own CORS or auth handling: nginx is the only thing
that ever reaches it, and by the time a request gets there it has already
passed the login gate. `docker-compose.yml` goes from two services (web +
sidecar) to two services of a different shape (web + a much smaller
login-only auth process) — not the zero-app-code outcome Cloudflare Access
would have given, but keeps today's password-manager-friendly login exactly
as it is, which is what was decided.

## Protocol groundwork already done

Researched from `pytboss` (the library the current sidecar uses) so the
actual C++ port is scoped, not a reverse-engineering project. Full detail in
the chat history that produced this plan; the load-bearing facts:

- **Transport**: Mongoose OS RPC-over-GATT. Service `_mOS_RPC_SVC_ID_`;
  write a 4-byte big-endian length to `_mOS_RPC_tx_ctl_`, then the JSON
  payload in 20-byte chunks to `_mOS_RPC_data___`. A reply arrives as a
  length notification on `_mOS_RPC_rx_ctl_`, then that many bytes read back
  from `_mOS_RPC_data___`. Live state pushes arrive unsolicited on the debug
  log characteristic (`_mOS_DBG_SVC_ID_` / `0mOS_DBG_log___0`) as ASCII
  lines `<==PB: FE0B...` (status) / `<==PB: FE0C...` (temperatures) — this
  is what gives sub-second state updates without polling. All of this is a
  direct, mechanical port of `pytboss/ble.py`'s `BleConnection` (~350 lines,
  much of it bleak-specific plumbing that disappears once this runs as a
  native BLE central instead of a proxied one).
- **Auth**: authenticated calls carry a `psw` field — the grill password
  XOR-obfuscated with a key derived from the grill's own uptime
  (`PB.GetTime`) in 10-second buckets, current-or-next bucket only accepted.
  `pytboss/codec.py`'s `timed_key()`/`encode()` are ~30 lines of pure byte
  manipulation — no crypto library needed, trivial to port as-is.
- **The command surface for this specific grill is small.** Your unit
  (PBV5 P2 on the **PBV2** control board — this project already hardcodes
  that board) needs exactly five commands, four of them fixed byte strings:
  - `get-status` → `FE0B01FF`
  - `get-temperatures` → `FE0C01FF`
  - `turn-off` → `FE0102FF`
  - `turn-on` → `FE0101FF` (undocumented in pytboss's own tables — no board
    declares a slug for it — but proven working; see `turn_grill_on()`'s
    docstring in `pytboss/api.py`)
  - `set-temperature` → the one non-fixed command, ~5 lines of arithmetic
    (round to nearest accepted setpoint, emit 3 BCD-style digits + a
    Fahrenheit/Celsius flag byte)

  Your tested unit has `lights: 0` in pytboss's own catalog (no light) and
  the PBV2 board declares no primer-motor slug, so — for this grill —
  light/prime control isn't needed at all. (A different model/board later
  would mean re-deriving its command table from `pytboss`'s `grills.json`
  the same way; this plan is explicitly scoped to the PBV2 board, matching
  what `grill_sidecar.py`'s `CONTROL_BOARD` constant already pins today.)
- **State decoding**: the FE0B (status) and FE0C (temperatures) frames
  decode via straightforward field extraction — `parts[n] === 1` booleans,
  three-digit BCD temperature triplets — about 30 lines total, not
  obfuscated, pulled directly from `pytboss`'s `grills.json` for board
  `PBV2`. A direct hand port, not a JS-engine-on-a-microcontroller exercise.
- **Cloud password fetch** (`scripts/pitboss_cloud.py` today): a couple of
  plain HTTPS JSON calls to `api-prod.dansonscorp.com` — moves onto the
  ESP32 essentially unchanged (ESP-IDF's `esp_http_client` + mbedTLS handles
  this fine, and it's the same TLS capability the Telegram POST needs
  anyway). Doing this fetch browser-side instead was considered and
  rejected: Dansons' API almost certainly has no CORS policy allowing
  arbitrary browser origins, which would just fail silently in the browser.

## What lives on the ESP32

- Firmware stays **ESPHome** (not a from-scratch ESP-IDF/Arduino rewrite):
  keeps WiFi management, OTA, and the existing YAML + `external_components`
  workflow this project already uses (`esphome/grill-proxy.yaml`). The work
  is a custom external component (real C++, not YAML) that:
  1. Connects to the grill by advertised name (`PBV2-` prefix) as a native
     BLE central — this alone removes the entire habluetooth/bleak-esphome
     proxy layer and, with it, gotchas #2–#5 in `docs/STATE.md`.
  2. Implements the RPC framing, the auth codec, the five commands, and the
     two frame decoders above.
  3. Registers extra HTTP routes on ESPHome's existing `web_server` httpd
     instance (rather than standing up a second HTTP server) for a REST API
     shaped like today's sidecar: `/health`, `/state`, `/info`, `/command`,
     `/setup`, `/alarms` (GET/POST), `/alarms/{id}` (DELETE). Reusing the
     same shape is what keeps the Blazor frontend untouched.
  4. Runs the alarm monitor loop (temp-target / timer, same semantics as
     today's `scripts/alarms.py`) and, on a fire, does one HTTPS POST to the
     Telegram Bot API — see below.
  5. Persists to NVS flash: the grill's BLE password (set once via a
     `/setup`-equivalent endpoint, mirroring today's "Pit Boss account"
     dialog), the alarm list (a handful of small JSON objects — no scale
     concern at this size), and the Telegram bot token + chat id.
- **No VAPID, no service worker push, no subscription state at all.** This
  also fixes the reliability problem hit repeatedly this session (a
  browser's push subscription silently going stale): alerts no longer
  depend on any specific browser tab or its subscription surviving —
  ESP32 → Telegram → phone, independent of whether the app is open.

### Telegram integration (placeholder, swappable)

Kept intentionally simple and isolated so switching to ntfy.sh or Pushover
later is a one-function change:

- One-time setup (outside this firmware): message
  [@BotFather](https://t.me/BotFather) on Telegram, `/newbot`, get a bot
  token; message the new bot once, then call
  `https://api.telegram.org/bot<TOKEN>/getUpdates` to read back your chat
  id. Both values get entered once via the web app's setup flow and stored
  on the ESP32 (NVS), same pattern as the grill password today.
- Sending an alert is a single call:
  `POST https://api.telegram.org/bot<TOKEN>/sendMessage` with
  `chat_id` and `text` form fields (or JSON body) — no signing, no
  encryption beyond the TLS the connection already needs.
- Write this as `notify(message: string)` in its own translation unit with
  no callers outside the alarm monitor — the planned swap to ntfy/Pushover
  later touches only that one function's body plus whatever config fields
  the setup flow collects.

## What's removed entirely

- The `sidecar` service from `docker-compose.yml` and `docker/`.
- `scripts/grill_sidecar.py`, `scripts/esphome_ble.py`, `scripts/alarms.py`,
  `scripts/pitboss_cloud.py` (logic migrates into the firmware; the cloud
  fetch keeps the same shape, just in C rather than Python).
- `requirements.txt`'s entire BLE stack: `pytboss`, `bleak`,
  `bleak-retry-connector`, `bleak-esphome`, `habluetooth`, `aioesphomeapi`.
- `OpenPit32/wwwroot/js/push.js`, `OpenPit32/wwwroot/push-worker.js`, and
  the Alarms card's notification-permission/subscription code in
  `GrillDetail.razor` — replaced by nothing on the frontend at all, since
  notification delivery no longer touches the browser.
- `esphome/grill-proxy.yaml`'s `bluetooth_proxy:` config — replaced by the
  new custom component, though see the rollout note below.

## Risks, stated plainly

- **Native BLE central GATT work on ESP-IDF (NimBLE) is genuinely fiddly** —
  MTU negotiation, connection parameters, notification timing. Trading
  today's well-documented Python/bleak gotchas for a new set of embedded
  ones is close to certain, not a risk to wish away.
- **This is a fire-capable appliance.** The ESP32 becomes the only thing
  between "the grill is controllable" and "it isn't" — not just the BLE
  link, but also its own HTTP server and command dispatch. Keep the
  confirm-before-power-on UX exactly as it is today, and prove read-only
  commands (status/temperatures) solid over real bench time before wiring
  up power-on/set-temperature.
- **Pinned to the PBV2 control board** — already true in practice
  (`CONTROL_BOARD = "PBV2"` is hardcoded today), just made explicit. A
  different model/board later means re-deriving its command table.
- **Slower iteration loop** than a Python REPL — flash + serial log instead
  of `ble_probe.py`'s quick round trips. Develop against the current
  sidecar's logged traffic as a reference rather than re-deriving protocol
  behavior from scratch.
- **A new secret-at-rest location**: the grill's BLE password and the
  Telegram bot token now live in the ESP32's flash, not just the Pi's
  `.grill_env`. No worse than the WiFi credentials already stored there
  today, but worth naming.

## Rollout plan

Keep `esphome/grill-proxy.yaml` (today's `bluetooth_proxy` config) working
as a fallback — e.g. a second yaml, or flip a config flag — until the new
firmware has proven itself, given what's at stake if it's wrong.

1. **Bench de-risking**: native BLE connect + GATT service discovery +
   an unauthenticated `RPC.Ping`/`Sys.GetInfo` round trip, logged over
   serial. Proves the framing before anything grill-specific.
2. Port the auth codec; call authenticated `PB.GetState`; log the decoded
   JSON and diff it against what the current sidecar logs for the same
   moment.
3. Wire status/temperature decoding into the component's internal state.
4. Add the REST endpoints (`/health`, `/state`, `/info`) — read-only —
   and repoint nginx's `proxy_pass` at the ESP32 to confirm the Blazor app
   renders live data with zero frontend changes.
5. Add `turn-on`/`turn-off`/`set-temperature` behind the same confirm
   semantics the sidecar enforces today.
6. Add the alarm monitor loop + Telegram `notify()`.
7. Add the `/setup` endpoint (cloud password fetch, run from the ESP32) and
   NVS persistence for password + alarms + Telegram config.
8. Port the login-only process (decision 5) from today's
   `grill_sidecar.py`; remove the Python BLE stack and the frontend's
   push/notification code.
9. Update `README.md`, `docs/STATE.md`, `docs/PROTOCOL.md` to describe the
   new architecture; retire this file's "not yet started" framing once
   Phase 1 begins.

## Still open

- Exact NVS storage layout for alarms/config (a flat JSON blob is likely
  fine at this scale; not decided).
- Whether `/probe-targets` (reading configured probe targets via
  `PB.Get/SetVirtualData`) is worth porting — not currently exposed in the
  Blazor UI, so may be dropped rather than ported.
