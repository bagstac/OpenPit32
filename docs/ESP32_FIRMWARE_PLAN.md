# Plan: move all grill control onto the ESP32

Goal, from a 2026-09-09 discussion: collapse the current three-hop
architecture (web app → Python sidecar → ESP32 BLE proxy → grill) down to
two real components — a thin web-facing edge, and an ESP32 that owns 100%
of the Bluetooth protocol, command dispatch, alarm evaluation, and alerting.
Current state / why the sidecar exists at all today: `docs/STATE.md`.
Protocol facts this plan leans on: `docs/PROTOCOL.md`.

Phases 1 through 4 (below) are implemented and bench-verified against the
real grill; everything past that is still ahead.

## Verified 2026-09-09: Phase 1, native BLE connect + RPC.Ping

`esphome/components/pitboss_grill` (custom ESPHome external component) and
`esphome/grill-firmware.yaml` (a separate file from `grill-proxy.yaml` on
purpose — see "Rollout plan" below), OTA-flashed to the real grill-proxy
ESP32 and run against the actual grill:

- Connects by advertised-name prefix (`PBV2-`), matching this project's
  existing name-based approach rather than a fixed MAC, since the grill's
  BLE address rotates.
- Discovers the Mongoose OS RPC-over-GATT service and the debug-log
  service; registers for notifications on both.
- The debug-log channel streams the grill's own `<==PB: FE0B...` /
  `<==PB: FE0C...` push frames live and continuously — the same status/
  temperature push traffic `docs/PROTOCOL.md` describes.
- The RPC channel completes a full, repeated `RPC.Ping` round trip (write
  the chunked request, get the length notification, reassemble the chunked
  read reply): `{"id":1,"result":{"channel_info":"*","rssi":-70},
  "src":"PBV2-9451DC46B934"}` — a real reply from the real board id, RSSI
  varying between calls as expected.

**One real bug found and fixed along the way, worth remembering**:
`esphome::esp32_ble::ESPBTUUID::from_raw()` does a plain `memcpy` of the 16
raw bytes with no byte-order conversion. BLE's own wire format for a
128-bit UUID is byte-reversed relative to the human-readable/RFC4122 string
form; Python's `uuid.UUID(bytes=...)` (what `pytboss`'s `_uuid()` builds on)
uses the string-order convention, and `bleak` does the reversal internally
when it hands the UUID to the OS Bluetooth stack. `from_raw()` alone
connected and completed service discovery just fine (state legitimately
reached `ESTABLISHED`) but silently resolved zero characteristics — every
handle stayed `0x0000` because the service/characteristic UUID lookups
never matched. `ESPBTUUID::from_raw_reversed()` is the one that matches
what's actually on the wire; switching to it fixed every lookup at once.
Anyone porting more of `pytboss`'s raw-16-byte Mongoose UUIDs to C++ later
needs this same conversion — see the `mongoose_uuid()` helper in
`pitboss_grill.cpp`.

Separately, worth remembering for future bench sessions: don't leave more
than one `esphome logs` connection open against the same device — its API
server caps at 5 connections, and stale watchers left over from earlier
test cycles (plus the Pi sidecar's own reconnect attempts once this ESP32
stops running `bluetooth_proxy`) exhausted that limit twice during this
session, producing a rapid-reconnect symptom that looked like a firmware
crash but wasn't one.

## Verified 2026-09-09: Phase 2, auth codec + authenticated PB.GetState

`pitboss_grill.cpp` ports `pytboss/codec.py`'s `timed_key()`/`encode()` to
C++ (`timed_key()`/`pb_encode()`/`to_hex()`), derives the auth key from the
grill's own uptime (via an unauthenticated `PB.GetTime` each cycle, per
`docs/PROTOCOL.md`), and calls authenticated `PB.GetState` on a 15s
interval. OTA-flashed and run against the real grill:

- Repeated, real `PB.GetState` round trips decode successfully:
  `{"id":3,"result":{"sc_11":"FE0B...","sc_12":"FE0C..."},
  "src":"PBV2-9451DC46B934"}` — the same two raw status/temperature frames
  already streaming over the debug-log channel, confirming the codec and
  the authenticated call both work end-to-end.
- The occasional spurious `401 Unauthorized` `docs/PROTOCOL.md` warns about
  (a slow write landing in the wrong 10s key bucket) does happen on the
  bench; the next 15s cycle re-derives the key from fresh uptime and
  succeeds, exactly as expected — not treated as fatal.
- `PB.GetState`'s reply fields are `sc_11`/`sc_12` (raw hex frames), not
  named fields like `moduleIsOn`/`grillTemp` — those names only exist after
  `pytboss`'s per-control-board `parse_status()`/`parse_temperatures()`
  decode them (see `api.py`'s `get_state()`). Porting that bit-level decode
  is Phase 3, per the rollout plan below.

**One real bug found and fixed along the way, worth remembering**: firing a
multi-chunk GATT write's chunks back-to-back with no pacing crash-looped
the device — reliably, on every attempt, regardless of which task called
it or what the payload contained. `BLECharacteristic::write_value()`
defaults to `ESP_GATT_WRITE_TYPE_NO_RSP` (write-without-response), which
returns as soon as the BT controller's internal buffer pool *accepts* a
write, not once it's actually been sent — queuing several unpaced writes
before that pool drains exhausted it and took down the whole BT stack.
`RPC.Ping`/`PB.GetTime` only ever need ~3-4 total writes (one length write
+ 2-3 data chunks) and never hit this; `PB.GetState`'s longer JSON body
needs ~7 (one length write + ~6 data chunks) and hit it every single time.
Bisection ruled out the codec math, the JSON payload content, and the
BLE-callback-vs-main-loop call stack (wrapping the reply dispatch in
`defer()` did *not* fix it) before landing on write pacing as the actual
cause. Fixed by pacing each chunk off its own `ESP_GATTC_WRITE_CHAR_EVT`
completion event (see `write_next_rpc_chunk_()`/`on_rpc_write_complete_()`)
instead of firing them in a loop. Anyone adding a request with a longer
body later should keep using that same event-paced path rather than
looping over `write_value()` directly.

## Verified 2026-09-09: Phase 3, status/temperature decoding

`pitboss_grill.cpp`'s `parse_status_frame_()`/`parse_temperature_frame_()`
decode the sc_11/sc_12 (FE0B/FE0C) frames into a new `GrillState` struct
(`pitboss_grill.h`), exposed via `grill_state()`. The decode is ported from
pytboss's `grills.json` **"PBV2"** control board entry specifically (the
board `scripts/grill_sidecar.py`'s `CONTROL_BOARD` names) — not a general
decode: `grills.json` ships ~20 control boards as vendor JavaScript
(evaluated through a JS interpreter in `pytboss/grills.py`), each reading
different byte offsets, and this project only ever talks to one grill.
Fields are only pulled from wherever PBV2's own routine actually reads them
— e.g. probe/chamber temperatures come from FE0C only, since PBV2's FE0B
routine leaves that block commented out.

Both frame sources are wired to the same decoders: the authenticated
`PB.GetState` reply (`on_get_state_reply_()`, every 15s) and the grill's own
unauthenticated debug-log pushes (`on_debug_log_()`, `<==PB: FE0B…`/`<==PB:
FE0C…` lines, arriving every few seconds) — so `grill_state()` stays current
between polls rather than only updating once per cycle.

OTA-flashed and run against the real grill (bench: mains-powered, module
off, no probes connected):

- Status decodes correctly: `on=0 fan=0 igniter=0 auger=0 light=0 prime=0
  no_pellets=0 errs(...)=0/0/0/0/0/0/0/0` — matches the bench's actual
  state (grill on mains but not switched on at the module).
- Temperatures decode correctly: `Temps (F): grill=71/140 smoker=71 p1=-1
  p2=-1 p3=-1 p4=0` — grill ambient/setpoint and smoker sensor read real
  values, disconnected probes 1-3 report `-1` (not a bogus temperature),
  and probe 4 (present but reading near-zero on the bench) comes through as
  `0`. Independently hand-verified against the same captured hex frames
  before flashing.
- Ran stable for the full observation window with both decoders firing
  continuously (unauthenticated pushes every few seconds, authenticated
  `PB.GetState` every 15s) — `gattc_calls` climbed steadily (24→71+) with no
  crashes, disconnects, or dropped writes.

## Verified 2026-09-10: Phase 4, REST endpoints

`pitboss_grill.h`/`.cpp` now implement `AsyncWebHandler` directly and
register `/health`, `/state`, `/info` on ESPHome's shared `web_server_base`
httpd (`web_server_base_->init()`/`add_handler(this)` in `setup()` — the
same pattern `esphome/components/prometheus` uses, not a second HTTP
server). Field names and shapes deliberately mirror
`scripts/grill_sidecar.py`'s handlers exactly — see
`OpenPit32/Services/GrillRpcService.cs`'s `SidecarHealthResponse`/
`SidecarStateResponse`/`SidecarInfoResponse` for the DTOs this has to match
— so the Blazor frontend needs zero changes once nginx points here. Some
`/health` fields shift meaning now that this ESP32 plays both of the old
architecture's proxy roles: `proxy_connected` is unconditionally `true` and
`proxy_wifi_rssi`/`proxy_uptime_seconds` report this device's own WiFi
RSSI/uptime (previously a *second*, now-removed ESP32's) — see the comment
above `handle_health_()`. `configured` is unconditionally `true` for now
(the grill password is still compile-time — Phase 7 makes this conditional).
`/info`'s `firmware` and `accepted_setpoints_f` aren't populated yet
(Phase 5/7); `GrillDetail.razor` already treats their absence as "nothing to
show".

**Two real bugs found and fixed getting this onto real hardware, both worth
reading before touching this component again**:

1. `PitbossGrill` inherits `BLEClientBase`, whose default
   `get_setup_priority()` is `setup_priority::BLUETOOTH` (350) — *higher*
   than `wifi:`'s own (250), meaning `setup()` (and so
   `web_server_base_->init()`) ran *before* WiFi's own setup(). The very
   first real HTTP GET after OTA-flashing this crash-looped the device
   every time, reliably. Fixed by overriding `get_setup_priority()` to
   `setup_priority::WIFI - 1.0f` — the exact same fix (and reasoning)
   `esphome/components/prometheus`'s `PrometheusHandler` already uses for
   the identical problem. (BLE client registration itself doesn't depend on
   this ordering — `esp32_ble_tracker.register_client()`'s codegen emits a
   raw call outside any component's `setup()` — so moving our own setup()
   later cost nothing there.)
2. Once REST handlers exist, `grill_state_`/`last_error_`/`board_id_`
   (`std::string`/struct fields) get read from the httpd task — a *third*
   FreeRTOS task touching state that was already being written from two
   others (the BT stack's own task, via `gattc_event_handler()`/
   `on_debug_log_()`, which are NOT deferred to the main loop — only
   `on_rpc_read_()`'s reply dispatch is, and that's for stack-depth reasons,
   not thread safety; and the main loop task, via deferred `PB.GetState`
   replies) with zero synchronization the whole time. This is a real crash
   risk for heap-backed types like `std::string`, not just a theoretical
   one: after the priority fix above, hardware still crashed on the first
   `/health` hit, and once it had, the device's network/httpd stack never
   recovered on its own — it stopped accepting new HTTP/OTA connections
   entirely (looked like a reboot loop from the reconnect noise, but the
   ~30-150ms reconnect cadence was too fast for an actual reboot; it needed
   a manual power cycle to clear). Fixed with an `esphome::Mutex`/
   `LockGuard` (`state_mutex_`) guarding every field touched from more than
   one task — see `set_last_error_()`/`clear_last_error_()` and the
   snapshot-under-lock-then-build-JSON pattern in `handle_health_()`/
   `handle_state_()`/`handle_info_()`.

OTA-flashed and re-verified after the fixes:

- `curl http://<esp32>/health|/state|/info` all return correct, real data:
  `{"configured":true,"connected":true,"proxy_connected":true,"rssi":-41,
  "proxy_wifi_rssi":-56,"proxy_uptime_seconds":20,"state_age_seconds":1.1}`,
  `{"configured":true,"board_id":"PBV2-9451DC46B934","model":"PBV5 P2",
  "accepted_setpoints_f":[],"has_lights":false,"meat_probes":4}`,
  `{"state":{"moduleIsOn":false,"grillTemp":68,"grillSetTemp":140,
  "smokerActTemp":68,"p4Temp":0,...},"state_age_seconds":1.5}` — matching
  the Phase 3 bench readings (disconnected probes 1-3 omitted as `null`, as
  intended).
- 60 back-to-back requests across all three endpoints, then a further
  30-request burst run concurrently with live BLE traffic, all returned
  `200` with no dropped connections.
- Live logs during and after that traffic show continuous, correctly
  decoded status/temperature frames every ~2s and `gattc_calls` climbing
  steadily (262→272+) with no disconnects, resets, or dropped writes — the
  race-condition fix holds under real concurrent HTTP+BLE load, not just in
  isolation.

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
4. **Static files: a bare-bones container on the Pi** (nginx), not an
   external static host — keeps the existing Cloudflare Tunnel / Docker
   Compose deployment model unchanged.
5. **Login stays on the web app** (decided 2026-09-09) — keep today's custom
   login page rather than switching to Cloudflare Access. The web-app side
   runs nginx plus a small login-only process: today's
   `scripts/grill_sidecar.py` login/session code (`/login`, `/logout`,
   `/auth-check`, the signed-cookie logic), everything grill-specific
   stripped out — roughly 150 lines. Password-manager-friendly form,
   30-day sessions, and the `AUTH_SECRET` invalidation story all carry over
   unchanged from today's implementation. ("Bare-bones" in decision 4 above
   meant no *grill* logic, not literally zero server code — this login
   process is the one exception.)
6. **Probe-target setting is not being ported** (decided 2026-09-09).
   Purpose, for future reference: it lets you store a desired temperature
   for a meat probe on the grill's own controller (e.g. "probe 1 → 203°F"),
   which the official Pit Boss app shows next to that probe's live reading.
   On this board it is **not enforced by the grill** — it's a generic
   "virtual data" scratch value (`PB.Get/SetVirtualData`) the firmware just
   remembers and hands back, not something that triggers a shutoff or alert
   on its own; some other boards wire it to a real MCU command, PBV2
   doesn't. Today's sidecar only ever reads this back (`GET /probe-targets`)
   — nothing in this app has ever written it — and it's functionally
   superseded by this project's own alarms feature (a probe-sensor alarm
   gets you the same "tell me when the meat hits X" outcome, except it
   actually fires a notification instead of sitting inert). Revisit if a
   future need shows up that the alarms feature doesn't cover — e.g.
   wanting the *official* Pit Boss app to display a target this app set.
7. **NVS layout: one JSON blob for the alarms list** (decided 2026-09-09),
   for now. A single key (e.g. `alarms`) holds the whole array as a JSON
   string, rewritten on every add/remove/fire — a direct port of what
   `scripts/alarms.py` already does with `alarms.json` today. Config values
   (grill password, Telegram bot token, Telegram chat id) each get their
   own individual string key. Considered and set aside: a key per alarm
   (avoids rewriting the whole list, but needs a second key just to track
   which alarm-keys exist) and a fixed-size binary struct array (marginally
   more efficient, but commits to a max alarm count up front and loses
   JSON's inspectability) — neither is worth the complexity at the scale
   this runs at (a handful of alarms, changed rarely).

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

1. ✅ **Done (2026-09-09)** — **Bench de-risking**: native BLE connect + GATT
   service discovery + an unauthenticated `RPC.Ping` round trip. See
   "Verified" above; logged over the network API rather than serial in the
   end (no USB access to the deployed board), which cost real debugging
   time chasing timing artifacts in one-shot logs before switching to
   periodic/counter-based diagnostics — worth doing that from the start
   next time.
2. ✅ **Done (2026-09-09)** — **Auth codec + authenticated `PB.GetState`**:
   port the auth codec, call `PB.GetState` on a timer, log the decoded
   JSON. See "Verified" above — including a real BLE write-pacing bug
   found and fixed along the way, worth reading before touching
   `write_rpc_command_()`.
3. ✅ **Done (2026-09-09)** — **Status/temperature decoding**: decode the
   `sc_11`/`sc_12` (FE0B/FE0C) frames' bit-level fields into the
   component's internal state. See "Verified" above.
4. ✅ **Done (2026-09-10)** — **REST endpoints**: `/health`, `/state`,
   `/info` — read-only. See "Verified" above. Repointing nginx's
   `proxy_pass` at the ESP32 is deliberately deferred — `/login`/`/logout`/
   `/auth-check` still need the sidecar's login process, which doesn't move
   over until rollout item 8, and repointing now would break login on the
   live deployment.
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

All decisions this plan depended on are now made (see "Decisions made"
above) — nothing left open. Phases 1 through 4 are done; ready for Phase 5
(turn-on/turn-off/set-temperature) whenever you want to start.
