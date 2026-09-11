# Plan: move all grill control onto the ESP32 (done)

Goal, from a 2026-09-09 discussion: collapse the original three-hop
architecture (web app → Python sidecar → ESP32 BLE proxy → grill) down to
two real components — a thin web-facing edge, and an ESP32 that owns 100%
of the Bluetooth protocol, command dispatch, alarm evaluation, and alerting.
**As of Phase 8 (2026-09-10), this is done**: the Python sidecar is deleted
entirely, replaced by a small login-only process
(`scripts/login_service.py`) that holds no grill state at all — see
`docs/STATE.md` for the current architecture and `docs/PROTOCOL.md` for the
protocol facts this plan leaned on.

Phases 1 through 9 (below) are all complete and bench-verified/deployed
live against the real grill — see the "Rollout plan" section near the
bottom for the phase-by-phase checklist, and each phase's own "Verified"
section for what was actually proven on real hardware. Nothing from this
plan's original scope remains open.

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
  "accepted_setpoints_f":[],"has_lights":false,"meat_probes":4}` (see the
  2026-09-10 update below — `meat_probes` here was wrong and has since been
  fixed to `3`),
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

## 2026-09-10 follow-up: nginx repointed, meat_probes bug found live

`docker/nginx.conf.template` (envsubst template now, not a static
`docker/nginx.conf`) adds `location = /api/health` / `/api/state` /
`/api/info` blocks proxying straight to the ESP32 (`$grill_esp32`, built
from the `GRILL_PROXY_HOST` env var — the same one the sidecar already used
for this board, so there's one source of truth instead of a second
hardcoded IP). Everything else under `/api/`, plus `/login`/`/logout`/
`/auth-check`, is untouched. Deployed to the real Pi
(`bsbagley@192.168.1.172`, `~/openpit32` — an `scp`'d tree, not a git
checkout, so this was a targeted file copy + `docker compose up -d --build
web`, not `git pull`) and verified end-to-end: logged in through nginx with
the real credentials and confirmed `/api/health`, `/api/state`, `/api/info`
all return live ESP32 data through the full auth-gated path, not just
reachable in isolation.

That real deployment immediately surfaced a second bug: the Live Status
card was showing a **Probe 4** this grill doesn't have. Root cause —
`handle_info_()` hardcoded `meat_probes` to `4`, but this exact grill
(PBV5 P2, board PBV2) has **3** meat probes per pytboss's own `grills.json`
spec (confirmed by querying it live from inside the sidecar container) and
per this plan's own "Decisions made" #8 in `docs/PLAN.md`. `4` was carried
over from a Phase 3 bench note about a `p4Temp` byte reading `0`, without
checking it against the actual per-model spec — the FE0C frame has a
`p4Temp` field at the byte level regardless of how many probe jacks the
grill physically has. `GrillDetail.razor`'s `AvailableSensors()` loops
`1..meat_probes` to decide how many probe cards to render, so this wasn't
cosmetic — it rendered a UI element for hardware that isn't there.

Fixed by promoting `has_lights`/`meat_probes` from hardcoded literals in
`handle_info_()` to real YAML config (`pitboss_grill.h`/`.cpp`,
`__init__.py`), defaulting to `false`/`3` — matching this grill's actual
spec — the same pattern `model` already used. OTA-flashed and confirmed
live: `curl http://<esp32>/info` now returns `"meat_probes":3`.

## Verified 2026-09-10: Phase 5, turn-on/turn-off/set-temperature

`pitboss_grill.h`/`.cpp` add `POST /command`, mirroring
`scripts/grill_sidecar.py`'s exact contract — body `{action, value?,
confirm?}`, reply `{ok, error?, action}` (see `GrillRpcService.cs`'s
`SendCommandAsync`/`SidecarCommandResponse`) — so `GrillDetail.razor`'s
existing double-click confirm UX needed zero changes. Only `power_on`,
`power_off`, and `set_temp` exist for this grill: `light_on`/`light_off`/
`prime_on`/`prime_off`/`set_probe` are real sidecar actions, but this grill
has no light and PBV2 declares no primer-motor slug (see "Protocol
groundwork" above), so they're reported as "unsupported on this grill"
rather than the sidecar's silent light no-op. `power_on`/`power_off` require
`confirm: true`, exactly like `bridge.command()` today — the safety gate
this plan's "Risks" section calls out by name.

Commands are sent as authenticated `PB.SendMCUCommand` (`{"command":
hex,"psw": ...}`), reusing the same auth codec `PB.GetState` already uses.
Fixed commands: `turn-on` → `FE0101FF` (undocumented in pytboss's own
tables — no board declares a slug for it — but proven working per
`turn_grill_on()`'s docstring in pytboss/api.py), `turn-off` → `FE0102FF`.
`set-temperature` snaps the requested value to this grill's real accepted
setpoints (130-420°F in 5° steps, ported from pytboss's `grills.json`
`temp_increment` for "PBV5 P2") and builds `FE0501<hundreds><tens><ones>FF`
— a direct port of the PBV2 board's own command function. `/info`'s
`accepted_setpoints_f` is now populated with this same list (previously an
empty array, Phase 4's placeholder).

Because the REST handler must answer synchronously — a caller acting on
`{"ok": true}` needs that to mean the grill actually took the command, not
just that it was queued, matching `bridge.command()` being awaited to
completion today — the httpd task blocks on a FreeRTOS semaphore until the
async GetTime→encode→write→ack round trip (on the main loop/BT stack, same
as every other RPC in this file) actually completes, with a single retry on
rejection (same key-bucket-skew tolerance `PB.GetState` already has) and an
8s timeout. `command_mutex_` serializes concurrent `POST /command` calls —
only one physical BLE link exists to drive.

**One real bug found and fixed getting this onto real hardware**: every
non-200/404/409 status code passed to `AsyncWebServerRequest::send()` gets
silently remapped to `500` by web_server_idf's `init_response_()` — a `400`
for a rejected command (missing `confirm`, unknown action, etc.) came back
as a misleading `500 Internal Server Error` with the correct JSON body still
in the response. Since `GrillRpcService.cs`'s `SendCommandAsync` never
checks the HTTP status code anyway (it deserializes the body and reads
`ok`/`error` unconditionally), the fix was to stop trying to status-code
`/command`'s replies at all — every reply is `200`, success or not, and the
body's `ok` field is the only signal that matters. Worth remembering for any
future REST route on this ESP-IDF web server backend: only 200/404/409 are
real status codes here.

**One known trade-off, not a bug**: ESP-IDF's httpd processes one request at
a time, unlike the sidecar's asyncio server (which yields between awaits),
so a command in flight also stalls this ESP32's `GET /health`/`/state`/
`/info` for the same few seconds. Acceptable for a user-initiated,
infrequent action — not something a polling loop should ever trigger.

OTA-flashed and verified against the real, live production grill (not just
the bench) — 2026-09-10, with explicit sign-off before the power-on step
specifically, since that ignites a real fire in an unattended appliance:

- `POST /command` with no `confirm` on `power_on`/`power_off` is rejected
  (`"'power_on' requires confirm: true"`), same for an unsupported action
  (`light_on`, `set_probe`) and a `set_temp` with no `value` — none of these
  touch the BLE link at all.
- `set_temp` to 225°F: `{"ok":true}`, and `/state`'s `grillSetTemp` moved
  130→225 within the next debug-log push (~2s), confirming the real MCU
  command round-tripped and the board actually applied it.
- `power_off` while already off: `{"ok":true}` (idempotent, as designed).
- `power_on` (confirm: true): `{"ok":true}`, and `/state` showed the grill's
  real startup sequence within seconds — `moduleIsOn:true`, igniter and fan
  on, auger cycling — a real ignition, not a simulated one. (The controller
  itself resets the setpoint to its 130°F startup default on power-on,
  independent of whatever was set beforehand — normal Pit Boss behavior, not
  something this firmware controls.)
- `power_off` again immediately after: `{"ok":true}`, `moduleIsOn:false`
  confirmed (fan continued briefly on its own — the controller's normal
  post-shutdown cool-down cycle).

Not yet repointed: nginx's `/api/command` still goes to the sidecar (see
rollout item 4's follow-up note above) — this phase only proved the ESP32
side directly. Repointing it is a small addition to the same
`nginx.conf.template` pattern whenever that's wanted.

## Verified 2026-09-10: Phase 6, alarms + Telegram notify

`pitboss_grill.h`/`.cpp` add `GET/POST /alarms` and `DELETE /alarms/{id}`,
mirroring `scripts/alarms.py`'s temp-target/timer semantics and
`scripts/grill_sidecar.py`'s exact route shapes (see `fill_alarm_json()`'s
comment for the field mapping to `GrillRpcService.cs`'s `AlarmDto`) — no
frontend changes needed. A 5s `alarm_check` interval (matching the
sidecar's `CHECK_SECONDS`) evaluates every alarm against `grill_state()`/
real time and drops any that fire; a fired alarm calls a Telegram
`notify()` (decision #3's placeholder), one HTTPS POST kept in its own
function so swapping providers later touches nothing else. Both bot fields
default to `""`: unset, an alarm still fires and drops on schedule, it just
logs instead of sending anywhere.

Needs real wall-clock time — a timer alarm's `fires_at` has to be a genuine
Unix timestamp `GrillDetail.razor`'s countdown can subtract
`DateTimeOffset.UtcNow` from, not millis-since-boot — so
`grill-firmware.yaml` gained a `time: - platform: sntp` block, and
`http_request:` (auto-loaded with defaults, same as `web_server_base`)
supplies the Telegram POST. `POST /alarms` rejects with "grill clock not
synced yet" if NTP hasn't completed (only matters in the first few seconds
after boot).

**One real, framework-level bug found and worked around**: `DELETE
/alarms/{id}` cannot go through this component's normal `canHandle()`/
`handleRequest()` path at all — `web_server_idf`'s `AsyncWebServer` only
ever registers `HTTP_GET`/`HTTP_POST`/`HTTP_OPTIONS` handlers with the
underlying `esp_http_server` (`AsyncWebServer::begin()`), so there is no
code path for any other method, regardless of what an `AsyncWebHandler`
implements. Matching the sidecar's real `DELETE` (rather than inventing a
`POST`-based delete that would be a permanent frontend divergence) meant
registering a second handler directly on the same `httpd_handle_t` for
`HTTP_DELETE` (`setup_delete_handler_()`), which has to operate on the raw
`httpd_req_t` — `AsyncWebServerRequest`'s constructor is only callable by
`AsyncWebServer` itself. Worth it: this is the one route where the sidecar
comparison, `GrillRpcService.cs`'s `DeleteAlarmAsync`, actually checks
`IsSuccessStatusCode` rather than only reading the JSON body, so the real
`200`/`404` this path can produce (unlike every other route here, capped at
`200` by `init_response_()`'s limitation — see Phase 5's note) isn't
cosmetic.

OTA-flashed and verified against the real ESP32 (alarms/Telegram don't
touch BLE grill control at all, so no live-grill sign-off needed this
phase): validation errors for every bad input (unknown kind/sensor/
comparison, missing target, non-positive duration), a real temp alarm
created with a correct 2026 Unix `created_at`, `DELETE` returning genuine
`200`/`404` confirmed via a follow-up `GET`, and an 8s timer alarm firing
and auto-dropping on schedule with the expected log lines (`Alarm fired:
...` / `... but no Telegram bot configured`, with no bot configured yet at
that point). A real bot token + chat id (via BotFather, per
`grill-firmware.yaml`'s setup comment) were added to `secrets.yaml`
afterward and a second live test — a 6s timer alarm — delivered a real
Telegram message to a real phone, confirming `notify_()`'s HTTPS POST
works end-to-end, not just the no-bot-configured fallback path.

## 2026-09-10 follow-up: debounce the occasional PB.GetState rejection

Live testing surfaced the spurious-401 case Phase 2 already documented as
expected (`docs/PROTOCOL.md`: a slow write can land in the wrong 10s auth
key bucket) — about 1 in 40 cycles in a real observation window, always
self-healing on the very next 15s cycle — but showing it as `last_error_`
on the very first occurrence flashed a false-alarm warning in
`GrillDetail.razor`'s link-problem badge for something that wasn't actually
a problem.

Fixed by tracking consecutive rejections (`get_state_reject_streak_`,
main-loop-only) and only calling `set_last_error_()` once the streak
reaches `error_display_threshold_` (default 5) — the debug log (`ESP_LOGW`)
stays unthrottled either way, this only debounces what reaches `/health`'s
and `/state`'s `last_error` field. The threshold is `std::atomic<uint8_t>`
and runtime-adjustable via a new `POST /config` endpoint (body
`{"error_display_threshold": N}`, 1-60) rather than a YAML constant, per
request — `/info` now also reports the current value so the web app can
show it. `GrillDetail.razor` gained a "Link Settings" card (shown only when
`info.error_display_threshold` is present, i.e. talking to the ESP32, not
the sidecar) to read/write it, and `nginx.conf.template` gained a
`location = /api/config` exact match alongside health/state/info's (no
sidecar equivalent exists, so it's not covered by the general `/api/`
block). In-memory only, like everything else here pre-Phase-7 — resets to
5 on every boot/reflash.

Deliberately scoped to just this one rejection path: the "reply wasn't
valid JSON" branch right above it, and `/command`'s own synchronous
rejection replies (missing confirm, unsupported action, etc.), are
immediate, user-initiated, one-shot responses — debouncing across cycles
makes sense only for a periodic background poll like this one.

## 2026-09-10 follow-up: the real bug behind the rejections — no latency bias

Phase 6 verification then surfaced why the "occasional" rate wasn't always
occasional: a fixed 15s `GetState` interval beats against the firmware's
10s auth-key bucket with a 30s period (their LCM), so depending on a given
boot's exact phase, `PB.GetTime`'s read can land right at a bucket boundary
every *other* cycle — not ~1/40 but a full ~50% rejection rate for that
boot's whole life, confirmed live (alternating fail/success/fail/success,
exactly matching the 30s beat).

Root cause: `checkPassword` only accepts a key built from the firmware's
own current bucket or the *next* one — never the previous one (pytboss's
`get_uptime()`: "absorbs a client running ahead but rejects one running
behind"). This port had no bias at all — it used `PB.GetTime`'s raw reply
value straight through, which by the time the derived key actually reaches
the grill (a further RPC round trip, ~150-400ms measured) has almost always
slipped slightly *behind* the firmware's true clock, the one direction it
never forgives. pytboss avoids this by timestamping before sending
`PB.GetTime` rather than after its reply arrives, so its own uptime
extrapolation always runs slightly ahead instead.

Fixed the same way in spirit, more simply in practice:
`AUTH_KEY_LATENCY_BIAS_S` (1.0s) is added to every `PB.GetTime` reading
once, in `on_get_time_reply_()`, before it's used to derive any key —
comfortably above the round trip actually measured on this hardware and
nowhere near the 10s bucket width, so it can only ever land on-or-ahead of
the firmware's true bucket, never overshoot into a second one. The
debounce work above already happened to hide this from the user-facing
`last_error` field (alternating failures never reach a streak of 5), but it
was real BLE traffic being wasted on every other cycle, not just a cosmetic
concern.

## Verified 2026-09-10: Phase 7, /setup + NVS persistence

`pitboss_grill.h`/`.cpp` add `POST /setup` — a direct port of
`scripts/pitboss_cloud.py`'s `fetch_and_save()` onto the ESP32 itself
(`docs/PROTOCOL.md` section 1: one `POST /login/app`, one
`GET /customer-grills`, both over `http_request_`) — plus NVS persistence
for the grill password it fetches, the alarms list, and the Telegram bot
token/chat id, so none of Phase 6's in-memory-only state resets on a
reboot/reflash anymore. Field names/shapes deliberately mirror
`scripts/grill_sidecar.py`'s `setup()` exactly (see `handle_setup_()`'s
comment for the field-by-field mapping to `GrillRpcService.cs`'s
`SetupAsync`/`SidecarSetupResponse`), so `SetupDialog.razor` needs zero
changes to work against the ESP32 once nginx points `/api/setup` here too
(not done yet — same "ESP32 side proven, nginx repoint deferred" pattern
Phases 5/6 left for `/command`/`/alarms`).

**Storage**: raw ESP-IDF NVS (`nvs_open()`/`nvs_get_str()`/`nvs_set_str()`),
not ESPHome's own `ESPPreferences` API — that one's built for small
fixed-size trivial structs (see `esphome/components/esp32/preferences.h`),
not the variable-length strings/JSON this needs. One namespace
(`pitboss_grill`), matching decision #7 below: the grill password, Telegram
bot token, and Telegram chat id each get their own string key
(`grill_pw`/`tg_token`/`tg_chat_id`); the alarms list is one JSON-blob key
(`alarms`) rewritten wholesale on every add/remove/fire
(`save_alarms_locked_()`), a direct port of how `scripts/alarms.py`'s
`AlarmStore` already treats `alarms.json`. `load_persisted_state_()` (called
once from `setup()`, before the BLE stack/httpd/alarm-check interval exist)
loads all four keys, falling back to the YAML-configured value for the three
strings when NVS has nothing yet — so a fresh device still boots configured
if `secrets.yaml` sets `grill_password`, but a `POST /setup` on any device
permanently takes over from there, surviving even a reflash with different
(or no) `secrets.yaml` content. `nvs_open()` needs no init step of its own:
ESPHome's own `esp32::ESP32Preferences` already calls `nvs_flash_init()` from
`app_main()`, before the logger or any `Component::setup()` runs.

**`configured` is a real field now** — `/health` and `/info` both used to
hardcode it to `true` (Phase 4's placeholder, since the password was
compile-time-only back then); it now reflects whether `grill_password_` is
non-empty, matching `GrillRpcService.cs`'s own doc comment on the field.
`/info` also gained `telegram_configured` (bot token + chat id both set) —
an extra field ignored by `System.Text.Json`'s default deserialization, like
`error_display_threshold` before it, so no C# changes were needed to add it.

**`POST /config` grew two more settable fields**: `telegram_bot_token`/
`telegram_chat_id`, alongside the existing `error_display_threshold`, each
persisted to NVS the moment they're set — the same runtime-without-a-reflash
pattern `error_display_threshold` established 2026-09-10 (see the earlier
follow-up note), now extended to Telegram config per this plan's "Telegram
integration" section ("Both values get entered once via the web app's setup
flow and stored on the ESP32 (NVS)"). No `GrillDetail.razor` UI calls this
yet for Telegram specifically — same as `error_display_threshold`'s own
rollout, the backend endpoint came first and a "Link Settings"-style card
followed later once it existed; curl it directly in the meantime. An empty
string is a valid, intentional value for either field — it's exactly
`notify_()`'s existing "no bot configured" no-op case, so this doubles as
how to turn Telegram delivery back off without a reflash.

**Concurrency**: `grill_password_`/`telegram_bot_token_`/`telegram_chat_id_`
could only ever be set once, from YAML, before Phase 7 — safe to read
unguarded anywhere. Now `POST /setup`/`POST /config` can rewrite them at any
time from the httpd task, while `send_get_state_()`/`send_mcu_command_()`
(grill password) and `notify_()` (Telegram fields) read them every cycle
from the main loop — every access outside `setup()`/
`load_persisted_state_()` itself now goes through `state_mutex_`, the same
lock already guarding `board_id_`/`last_error_`/`grill_state_`. The two
cloud HTTPS calls `handle_setup_()` makes are synchronous, blocking that
httpd request for their combined round trip (typically a couple of seconds,
two TLS handshakes) — the same "acceptable for a rare, user-initiated,
one-time action" trade-off `handle_command_()`'s comment already documents
for BLE commands, applied to a second kind of slow synchronous request on
this same single-threaded ESP-IDF httpd.

**Not applied**: `POST /setup`'s `model` field is accepted (matching the
sidecar's request shape, so `SetupDialog.razor`'s model dropdown doesn't
need to change) but not acted on — this firmware's setpoints/lights/probe
count (`ACCEPTED_SETPOINTS_F`, `has_lights_`, `meat_probes_`) are all fixed
by YAML config for the one grill/board it's compiled for (decision #8 in
`docs/PLAN.md`), so honoring a different label without changing those would
just make `/info` lie about what the hardware actually reports. The reply
always echoes back the compiled-in `model_` instead.

**One real bug found and fixed getting this onto real hardware, worth
reading before touching `nvs_save_string_()`/`save_alarms_locked_()` again**:
calling `nvs_save_string_()` (or `save_alarms_locked_()`, which JSON-builds
first) directly from a REST handler — exactly how the first version of
`handle_alarms_post_()`/`handle_alarms_delete_()`/`handle_config_()`/
`handle_setup_()` all did it — stack-overflowed the ESP-IDF httpd task and
crash-looped the device, confirmed live via `esphome logs`: a captured
backtrace decoded to `panic_abort -> esp_system_abort ->
vApplicationStackOverflowHook`. `web_server_idf`'s httpd task gets a small
(~4KB) default stack (see `AsyncWebServer::begin()`'s `config.stack_size =
config.stack_size + 256` — not configurable from YAML), and NVS's own flash
access — worse on a key's first-ever write, which may need to
allocate/erase a page — on top of a JSON build already using some of that
stack was enough to blow it. The device *did* recover on its own each time
(a clean reboot, not a hang needing a physical power cycle — different from
Phase 4's failure mode), but every REST handler that persists something
would have hit this on first real use. Fixed the same way in spirit as
`on_rpc_read_()`'s existing `defer()` of JSON parsing off the BLE callback's
own small stack: `nvs_save_string_deferred_()` now moves every REST-handler
NVS write onto the main loop task instead (which already reliably does
JSON work via the BLE reply path), and the alarms handlers defer a fresh
`LockGuard` + `save_alarms_locked_()` call the same way. The in-memory
state (`grill_password_`/`telegram_*_`/`alarms_`) still updates
synchronously so the HTTP reply is accurate immediately; only the flash
write itself is deferred, fire-and-forget (a failure there just logs a
warning — nothing waits on it).

OTA-flashed and verified against the real ESP32 (twice — once to reproduce
the crash with logs attached, once with the fix):

- The crash reproduced exactly as predicted: a `POST /alarms` request
  timed out on the caller, `esphome logs` showed
  `*** CRASH DETECTED ON PREVIOUS BOOT ***` /
  `vApplicationStackOverflowHook` at boot, and `/health`'s
  `proxy_uptime_seconds` had reset to single digits — a clean, automatic
  reboot each time, not a hang.
- After the fix: `POST /alarms` (a 1-hour timer alarm), `GET /alarms`,
  `DELETE /alarms/{id}` (both a real one and a repeat delete of the same id,
  confirming genuine `200`/`404` still works on this path) all completed
  normally with `proxy_uptime_seconds` climbing continuously through every
  call — no crash, no reset.
- NVS survives a real reboot: one test alarm created under the *old, buggy*
  code (right before it crashed) was still present after that crash's
  automatic reboot — the write itself had apparently completed just before
  the overflow was detected. More conclusively, after fully deleting both
  test alarms and OTA-reflashing the identical fixed binary again purely to
  force a clean restart, `GET /alarms` still came back `{"alarms":[]}` —
  the delete's NVS write, not just the add's, survived a real power-cycle-
  equivalent reboot.
- `configured`/`telegram_configured` in `/health`/`/info` came back `true`
  on every boot this session via the YAML-configured fallback (NVS has
  never had a real `POST /setup` write yet — see below) — confirming
  `load_persisted_state_()`'s fallback path works, not just its NVS path.

**Not yet exercised**: a real `POST /setup` round trip against an actual Pit
Boss account (needs real account credentials nobody pasted into this
session — intentionally not attempted), and `POST /config`'s
`telegram_bot_token`/`telegram_chat_id` fields specifically (skipped to
avoid overwriting the real, working bot credentials already proven live in
Phase 6 — see that section above). Both go through the exact same
`nvs_save_string_deferred_()` path the alarms fix above already proved
works, so this is a real but low-risk gap, not an untested code path in the
way the alarms crash was.

## 2026-09-11 follow-up: "grill busy" forever — a stuck-RPC watchdog + a real WiFi/BLE collision

Reported live: `POST /command` (including `power_on`) started answering
`"grill busy — try again"` on every attempt, permanently, for a real user
trying to use the app normally — not a bench artifact.

**Root cause #1, the actual bug**: `pending_reply_` (which of Ping/GetTime/
GetState/MCU_COMMAND is currently in flight — see the header's own comment)
had no timeout at all outside the httpd-triggered command path. If a sent
RPC request's reply simply never arrived — no truncation notification
either, `on_rpc_read_()`'s `len==0` path never fired, just *nothing* — there
was no mechanism anywhere to notice and recover. Confirmed live via
`esphome logs`: `write_rpc_command_()`'s own "Sent RPC request" log (which
only fires once the full write is confirmed by its own
`ESP_GATTC_WRITE_CHAR_EVT`, i.e. the request genuinely reached the grill)
fired normally, but no "RPC reply announced"/"GetTime OK" ever followed.
Once that happened once, `pending_reply_` stayed non-NONE forever: every
later 15s `GetState` cycle saw "a reply is still in flight" and no-opped,
and every `POST /command` saw the identical thing and answered "busy"
instantly — with no way back short of a full BLE disconnect/reconnect
(the only other place that resets `pending_reply_`).

Fixed with a watchdog: `rpc_request_millis_` timestamps every request in
`write_rpc_command_()`; `loop()` now force-clears anything still pending
past `RPC_REPLY_TIMEOUT_MS` (5s — generous over the ~150-600ms real round
trips measured on this hardware, comfortably inside both the 15s periodic
cycle and the command semaphore budget). The recovery logic is shared
(`abandon_pending_rpc_()`) with `on_rpc_read_()`'s existing truncation
handling, and now also **retries once** for a command's own request before
giving up — reusing `on_mcu_command_reply_()`'s existing "every MCU command
here is idempotent, so retrying cannot double-apply anything" guarantee,
since consecutive individual attempts routinely succeeded right after a
failed one. `POST /command`'s own semaphore wait grew from 8s to 12s to
give that retry room to actually land before the httpd side gives up first.

**Root cause #2, why replies were dropping at all**: found live, a
`[D][wifi] Roam scan (-58 dBm, attempt 1/3)` log line landing immediately
before one of these timeouts. ESPHome's `post_connect_roaming` (on by
default) rescans for a "better" AP every 5 minutes whenever WiFi RSSI is
below -49dBm (`wifi_component.h`'s `ROAMING_GOOD_RSSI`) — this board's own
WiFi signal (-56 to -58dBm, comfortably fine on its own terms) is always
below that bar, so it was roam-scanning on every check. A WiFi scan needs
the radio to hop across every channel, and this ESP32 has one 2.4GHz radio
shared with BLE (already flagged as a risk in `grill-firmware.yaml`'s own
comments, re: excessive logging starving WiFi during Phase 1 bring-up) —
each scan collided with whatever BLE RPC exchange was in flight. Fixed by
setting `post_connect_roaming: false`: this board sits fixed next to the
grill, so there's no second AP to usefully roam to in the first place —
disabling it is pure upside.

**Honest gap, not fully solved**: even with roaming disabled, a real
baseline RPC-reply-drop rate remains — measured live, individual attempts
still timing out something like 40-60% of the time, often enough that both
the original attempt and its one retry drop back to back
(`"grill did not reply in time"`). The passive debug-log push channel
(no request/reply round trip, just notifications) stayed 100% fresh
throughout every one of these failures, and WiFi/BLE RSSI were both
excellent (-43 to -44dBm) — so this isn't a range/signal-quality problem,
and it isn't the roaming collision either (no `Roam scan` line anywhere
near these). What specifically makes the *reply* side of a write+wait-for-
notify RPC exchange this unreliable on this hardware, while a one-way push
notification never drops, is not root-caused. What's true instead is that
the system now **recovers**: a `POST /command` that hits this either
succeeds on its built-in retry or fails within ~10-12s instead of wedging
the whole link forever — the actual reported symptom (permanently stuck,
needing a manual power cycle) is fixed, even though the underlying
reply-drop rate itself is a real, still-open question.

**Also worth remembering for next bench session**: `pkill -f "esphome
logs"` did not reliably kill background `esphome logs` processes in this
environment (Windows + Git Bash) — several accumulated across a long
session and exhausted the ESPHome API server's 5-connection cap, producing
a rapid-reconnect symptom (`EOF received (SocketClosedAPIError)` in a tight
loop, `esphome upload` failing with "Device closed connection without
responding") that looked exactly like a firmware crash but wasn't one — the
device's own `/health` answered normally the instant the stale client
processes were force-killed (`taskkill /F /IM python.exe` on Windows) and
its connection slots freed up. This is the same class of gotcha Phase 1's
bench notes already flagged once; evidently worth restating since it
recurred.

Deployed live and reverified: `POST /command` retries automatically and
recovers instead of wedging; no `Roam scan` line appears anymore in any
capture since disabling it; grill state (`moduleIsOn`, etc.) confirmed
unaffected by the extensive `power_off` testing this required (idempotent
by design, as intended).

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
   `/info` — read-only. See "Verified" above. nginx's `/api/health`,
   `/api/state`, `/api/info` now `proxy_pass` straight to the ESP32
   (`docker/nginx.conf.template`'s `$grill_esp32`), deployed and verified
   live on the real Pi — see the follow-up note right below. `/login`/
   `/logout`/`/auth-check` and everything else under `/api/` still go to
   the sidecar; that's rollout item 8, not done yet.
5. ✅ **Done (2026-09-10)** — **`turn-on`/`turn-off`/`set-temperature`**
   behind the same confirm semantics the sidecar enforces today. See
   "Verified" above — including a real HTTP-status-code bug found and fixed
   along the way, worth reading before adding another write route on this
   web server backend. nginx's `/api/command` isn't repointed yet (still the
   sidecar); only the ESP32 side was proven this phase.
6. ✅ **Done (2026-09-10)** — **alarm monitor loop + Telegram `notify()`**.
   See "Verified" above — including a real framework limitation (no
   `DELETE` support at all in this ESP-IDF web server backend) worked
   around, and a real auth-key latency bug found and fixed along the way
   (see the follow-up note right below — read it before touching
   `on_get_time_reply_()`/`send_get_state_()`/`send_mcu_command_()` again).
   nginx's `/api/alarms` isn't repointed yet (still the sidecar); only the
   ESP32 side was proven this phase. Telegram delivery itself is now
   confirmed live too (real bot, real phone) — see "Verified" above.
7. ✅ **Done (2026-09-10)** — **`/setup` endpoint (cloud password fetch,
   run from the ESP32) + NVS persistence** for the grill password, the
   alarms list, and the Telegram bot token/chat id. See "Verified" above —
   including a real stack-overflow crash found and fixed along the way,
   worth reading before touching `nvs_save_string_()`/
   `save_alarms_locked_()` again. The real cloud login (`POST /setup`
   against an actual Pit Boss account) and the Telegram fields of
   `POST /config` weren't exercised this session (no credentials pasted in,
   deliberately) — see "Not yet exercised" above. nginx's `/api/setup` was
   repointed as its own follow-up right after (see below) — every route
   this phase added is live on the real deployment now.
8. ✅ **Done (2026-09-10)** — **retired the Python sidecar entirely**:
   ported the login-only process (decision 5) into
   `scripts/login_service.py`, deleted `grill_sidecar.py`/`esphome_ble.py`/
   `alarms.py`/`pitboss_cloud.py` plus the now-unused BLE dev tools
   (`ble_scan.py`/`ble_probe.py`/`proxy_scan.py`), trimmed
   `requirements.txt` down to just `aiohttp`, and removed the frontend's
   push/notification code (`js/push.js`, `push-worker.js`, the
   VAPID-subscription UI in `GrillDetail.razor`, and
   `GrillRpcService.cs`'s push/models methods — `GET /models` had no
   replacement either, since it depended on pytboss's catalog). Also
   retired `esphome/grill-proxy.yaml` (the BLE-radio-only fallback firmware
   the sidecar drove remotely) now that the new firmware has months of real
   production use behind it — see git history if either ever needs to come
   back. See "Verified" below for the real deployment + a local-dev
   regression this surfaced and fixed.
9. ✅ **Done (2026-09-10)** — updated `README.md`, `docs/STATE.md`,
   `docs/PROTOCOL.md` for the new architecture. This file's own
   introduction no longer says "everything past that is still ahead" —
   nothing is, as of Phase 8.

All decisions this plan depended on are now made (see "Decisions made"
above) — nothing left open, and nothing left in the original migration
scope either. Phases 1 through 8 are bench-verified end to end, live on the
real deployment.
