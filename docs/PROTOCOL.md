# Protocol notes: Pit Boss cloud API and grill RPC

What this project relies on, written up from observation of the official
Android app's network behaviour (v2.10.3, August 2026) and from live testing
against a PBV2-board grill. No vendor code is reproduced here. Example values
are placeholders.

## 1. Cloud API — used once, to fetch the grill password

Base URL: `https://api-prod.dansonscorp.com/api/v1`. Accounts are JWT-bearer;
the same headers go on every call:

```
Accept: application/json
Content-Type: application/json
X-Localization: en
x-country: US            # 2-letter code the account was created with
x-store: PB
Authorization: Bearer <jwt>   # after login
```

### POST /login/app
Body `{"email": "...", "password": "..."}`.

- 200 → `{"status":"success","data":{"token":"<jwt>","token_expiration":"<iso8601>","customer":{...}}}`
- 404 → `{"status":"error","error_code":"UNIDENTIFIED_CUSTOMER", ...}` for wrong credentials
  (yes, 404 — not 401).

### GET /customer-grills
Bearer token required. Returns the account's *paired* grills — no telemetry:

```json
{"status":"success","data":{"customer_grills":[
  {"id": 1234567, "board_id": "PBV2-XXXXXXXXXXXX", "grill_nickname": "...",
   "password": "<grill password>", "grill_id": 137, "default_grill": 1, ...}
]}}
```

- `board_id` — the controller's identity: `PBV2-` + 12 hex digits of its MAC.
  It is also the name the grill advertises over Bluetooth LE.
- `password` — the per-grill credential that authenticates RPC commands
  (set in the app when the grill was paired). This is the only thing this
  project needs from the cloud. Treat as a secret.
- `grill_id` — a catalogue id, not a serial number.

Other endpoints exist (`/token/refresh`, `/register`, `/password/forgot`,
OTP / Shopify-multipass login, `/customer-grills/{id}` CRUD) but are not used.
`/customer-grills/{id}` answered 401 on the test account.

### Cloud relay (not used)
The app controls the grill through a WebSocket relay,
`wss://socket.dansonscorp.com/to/{board_id}`, carrying the same JSON-RPC as
below. The grill dials *out* to the relay and only keeps that session while
the controller is switched on, so the relay is silent for an off grill.
This project talks to the grill over Bluetooth instead; the relay transport
is still available in pytboss (`WebSocketConnection`) if ever needed.

## 2. Grill RPC — Mongoose OS JSON-RPC over BLE

The controller runs Mongoose OS on an ESP32 (ESP-IDF firmware 16.8.8 on the
test unit) and exposes the standard Mongoose RPC-over-GATT service:

- Service `_mOS_RPC_SVC_ID_`; characteristics `_mOS_RPC_data___`,
  `_mOS_RPC_tx_ctl_`, `_mOS_RPC_rx_ctl_`. A frame is a 4-byte big-endian
  length on the ctl characteristic followed by the JSON in 20-byte chunks.
- Debug-log service `_mOS_DBG_SVC_ID_` / `0mOS_DBG_log___0`: the grill
  *pushes* its status here as lines `<==PB: FE0B…` (status) and
  `<==PB: FE0C…` (temperatures), so a BLE client gets live state without
  polling.
- The grill answers over BLE **with the controller switched off** (mains
  only), which is what makes remote power-on possible.

[pytboss](https://github.com/dknowles2/pytboss) (`BleConnection`, `PitBoss`)
was this project's original reference for all of this — the protocol
knowledge below is now ported directly into `esphome/components/pitboss_grill`
(C++, running on the grill's own ESP32), not a runtime dependency on the
Python library anymore (see `docs/ESP32_FIRMWARE_PLAN.md`'s "Protocol
groundwork" section for the specific pytboss source each piece was ported
from):

- **Authentication.** Authenticated methods carry `psw`, the grill password
  obfuscated with a key derived from the grill's uptime (`PB.GetTime`) in
  10-second buckets. The firmware accepts the current bucket or the next one
  only, so a request that arrives late can draw a spurious `Unauthorized`;
  this project's firmware retries once, plus biases its own uptime reading
  forward slightly to land on-or-ahead of the firmware's true bucket rather
  than risk landing behind it (a real, measured bug found live — see the
  Phase 6 follow-up notes in `docs/ESP32_FIRMWARE_PLAN.md`).
- **Commands.** `PB.SendMCUCommand` with hex MCU commands (e.g. power on /
  off, set temperature) — this project only ever talks to one grill/board
  (PBV5 P2 on PBV2), so its command table is the fixed handful that one
  model needs, not pytboss's full 140+-model catalog.
- **State.** `PB.GetState` (authenticated) or the pushed FE0B/FE0C frames,
  decoded per control board.

Methods seen on the test firmware via `RPC.ListEx` (authenticated):
`PB.GetState`, `PB.GetTime`, `PB.SendMCUCommand`, `PB.GetFirmwareVersion`,
`PB.Get/SetVirtualData` (probe targets), `PBX.Get/SetParameters`,
`Config.Get/Set`, `FS.Get/Put/Remove`, `OTA.*`, `Wifi.Scan`,
`PB.SetWifiCredentials`, `PB.SetDevicePassword`, `PB.RenameDevice`,
`Sys.GetInfo`, `Sys.Reboot`, `RPC.Ping`, `RPC.ListEx`.
Unauthenticated: `RPC.Ping`, `Sys.GetInfo`, `PB.GetTime`.

There is no LAN HTTP endpoint on this firmware (the ESP-IDF build links no
HTTP server), which is why Bluetooth is the local path.

## 3. Bluetooth: a native ESP32 central, right next to the grill

An ESP32 running custom firmware (`esphome/grill-firmware.yaml`,
`esphome/components/pitboss_grill`) sits within a few metres of the grill
and connects to it directly as a native BLE central (ESP-IDF's NimBLE stack
via ESPHome's `esp32_ble_client`) — there is no PC-side Bluetooth process
and no proxy hop of any kind; the ESP32 *is* the thing everything above
talks to. This replaced an earlier design (a PC-side Python process reaching
the grill through a second ESP32 running ESPHome's stock `bluetooth_proxy`,
via `bleak-esphome`/`habluetooth`) — see `docs/ESP32_FIRMWARE_PLAN.md` for
why, and its "Protocol groundwork" section for the specific bugs found
porting each piece of this from Python to C (byte-order in 128-bit BLE
UUIDs, GATT write pacing, and the auth-key timing bias above).

Facts that hold regardless of which side of the wire implements them:

- The grill's BLE address is random and rotates — match by advertised name
  (`PBV2-` + 12 hex digits, the board id).
- The grill stops advertising while connected, so an RSSI reading only
  refreshes between connections (`/health`'s `rssi` field can go stale while
  connected — expected, not a bug).
- The grill answers over BLE **with the controller switched off** (mains
  only), which is what makes remote power-on possible.
