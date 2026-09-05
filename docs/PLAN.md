# Project plan / history

This is the working plan the project was built from, kept as history. Phases
1–3 (understanding the app's API, a cloud-relay client) were the research
path; Phase 5 (Bluetooth via an ESP32 proxy) is what the repository ships.
The cloud-relay transport, the account login UI and all reverse-engineering
artifacts referenced below were removed before publishing — only the
one-time password fetch (`scripts/pitboss_cloud.py`) still touches the
Pit Boss API. Current state: STATE.md. Protocol summary: PROTOCOL.md.

STATUS (2026-09-05): Phases 1–3 and 5 DONE. Phase 4 (local relay, no
Dansons) proposed, not started, superseded by Phase 5.

Goal: Determine how the PitBoss Android app logs in so it can be reproduced outside the app.

## Toolchain (ready)
- adb (Android SDK platform-tools), jadx 1.5.6, Java 21 (Temurin) — used
  during the research phase; none of it is in this repository.

## Steps
1. [ ] Device visible in `adb devices` (BLOCKED: phone not detected at USB level — user to connect cable, allow RSA prompt, set USB mode to File Transfer)
2. [ ] Find package name (`adb shell pm list packages | grep -i pitboss` and vendor guess)
3. [ ] Pull APK (`adb shell pm path <pkg>`, `adb pull`)
4. [ ] Decompile with jadx
5. [ ] Static analysis: search for login/auth/token endpoints, Retrofit/OkHttp base URLs
6. [ ] Dynamic: logcat capture while logging in (dump HTTP, URLs, tokens)
7. [x] Reproduce login flow externally (curl/script) - endpoint verified live
8. [ ] Document findings in notes/
9. [x] User: install CA cert on phone (Settings > Security > Install cert > Downloads > mitmproxy-ca-cert.cer)
10. [x] User: log in via Pit Boss app while mitmdump captures
11. [x] TLS blocked (user CA untrusted, targetSdk 35) -> documented; Frida gadget is optional next step
12. [x] Write API contract: notes/API_CONTRACT.md
13. [x] Login reproduction script: scripts/pitboss_login.py
14. [x] Phone proxy reset to :0; mitmdump stopped

## Login response schema (from AuthReducer + login saga)
{token, token_expiration, customer:{id, shopify_customer_id, email, first_name, last_name, phone, created_at}}
Token storage: RNEncryptedStorage keys 'token' and 'token_expiration'; refresh via GET /token/refresh when expired.

## Key findings (CONFIRMED)
- Package: com.pitbossgrills.app v2.10.3 (versionCode 123), React Native + Hermes (bytecode v96)
- API base (PROD): https://api-prod.dansonscorp.com/api/v1 (+ /api/v2)
- API base (STAGE): https://api-stage.dansonscorp.com/api/v1
- Backend: gunicorn/Python on AWS ELB be.wholescale.net (reviews), Azure Front Door for dansonscorp API, Laravel-style rate limits (X-RateLimit-*), Server: nginx
- Auth: JWT Bearer; Shopify-backed accounts (multipass SSO)
- Common headers: Accept/Content-Type application/json, X-Localization: <lang>, x-country: <CC>, x-store: PB, Authorization: Bearer <jwt>
- Endpoints:
  POST /api/v1/login/app {"email","password"}
  GET  /api/v1/token/refresh (Bearer)
  POST /api/v1/register {first_name,last_name,email,state,country,phone,country_code,subscribe_marketing,password,device_id}
  POST /api/v1/password/forgot {"email"} -> 204
  POST /api/v1/login/otp/send {"email"}
  POST /api/v1/login/multipass/token {"email","code"} -> {token,url}
  POST /api/v1/login/multipass {"multipass_token"} -> Shopify token
- Wrong creds -> HTTP 404 {"status":"error","error_code":"UNIDENTIFIED_CUSTOMER",...}
- Snare/iovation fingerprint WebView: cdn-live.wholescale.net/snare.js (reviews flow)
- MITM: mitmdump running on PC :8080; phone proxy set; CA cert pushed to /sdcard/Download
- TLS note: targetSdk 35, no networkSecurityConfig -> user CA likely untrusted by app; Frida gadget is fallback

## Phase 2: PitBossWeb client (in progress)
Goal: minimal Blazor web client that logs in and lists/controls the account's grills,
as a stand-in for the app (see PitBossWeb/).

1. [x] Login page + AuthState/TokenStore (JWT persisted, refresh-on-401)
2. [x] GET /customer-grills wired up ("Your Grills" = Pages/Home.razor)
3. [x] Confirmed live schema for /customer-grills (2026-08-17) and fixed
       Models/ApiModels.cs `CustomerGrill` to match real field names
       (grill_nickname, board_id, password, grill_id, default_grill — see
       notes/API_CONTRACT.md). Previous fields (nickname, controller_name,
       serial_number, model, device_id, lastStatus) were guesses that never
       matched the live response.
4. [x] Redact `password` (grill board credential) alongside `token` in logged
       response bodies (PitBossApiClient.Redact)
5. [x] UI overhaul + status rendering (2026-08-17, second agent):
       - Home.razor cards: nickname, "Default" badge, status badge, per-field
         list (password shown only as "•••••••• (hidden)"), and — when a
         `lastStatus` decodes — temp tiles (grill temp, set point, first 2
         probes). 30 s auto-refresh timer.
       - GrillDetail.razor: pairing-record card, "Live Status" card (grill
         temp, set point, smoker, mode, probes, fan, smoke level, flags).
         Controls card still stubbed/disabled pending the RPC socket.
       - CSS: dark `.pb-card` cards, orange `.pb-big` temp numerals, hover
         lift (wwwroot/css/app.css).
       - Models: `CustomerGrill.Extra` captures unknown fields via
         `[JsonExtensionData]`; `lastStatus` is looked up from Extra;
         `GrillStatus.FromJsonElement` is lenient (object, JSON string,
         string-typed numbers). Build verified green (dotnet build exit 0).
6. [ ] Live grill status/telemetry source: still NOT present on
       /customer-grills (no `lastStatus` key). UI + decode pipeline are ready,
       but real-time data must still come from the RPC WebSocket
       (`wss://socket.dansonscorp.com/to/{board_id}`), not a REST endpoint.
7. [ ] Reverse-engineer + implement the RPC socket handshake (auth with the
       per-grill `password`, decode `RPC.Ping`/status messages) — this is what
       Pages/GrillDetail.razor's Controls card is stubbed out waiting for.
8. [ ] Wire real-time status into Home.razor / GrillDetail.razor from the
       socket once available (UI slots + DecodeStatus already exist)
9. [ ] Set-temperature / power controls once the RPC protocol is understood
10. [ ] GET /customer-grills/{id} detail: response shape unverified — the
       client already tolerates several wrappers (TryFindElement in
       PitBossApiClient), but confirm whether it returns the same row as the
       list or richer data (e.g. embedded lastStatus).

## Phase 3: Direct grill communication — options (pick one)
Protocol facts now decoded (2026-08-18, from notes/bundle_decompiled.js +
community sources):
- The grill RPC is Mongoose-OS style JSON-RPC, not custom binary:
  client → `wss://socket.dansonscorp.com/to/{board_id}` (the grill itself
  dials out to `https://socket.dansonscorp.com/from/{deviceId}`).
  Frames: `{id, src, method, params, app_id?}` → `{id, src, app_id, result|error}`.
- Auth = the `codec`/`getUpdatedKey` pair we decoded: a self-mutating XOR
  stream with a 0xFF marker + random padding; key = scramble of the constant
  [0x8f,0x80,0x19,0xcf,0x77,0x6c,0xfe,0xb7] by grill uptime/10 (uptime from
  `PB.GetTime` — matches the app's `getGrillTime`/`initSecurity`). The grill
  password travels as a hex `psw` field on each authenticated command.
- Status = hex frames `FE0B` (status) / `FE0C` (temps), decoded per grill
  model. Control = hex MCU commands (e.g. `FE0102FF` = off, `FE0201FF` =
  light on).
- The community already solved this: `dknowles2/pytboss` (Apache-2.0, active,
  Python ≥3.12) implements all three transports (cloud WSS relay, local
  `http://<grill-ip>/rpc`, Bluetooth LE) plus the codec and a 147-model
  command/status table. `dknowles2/ha-pitboss` (62★) wraps it;
  `xeudoxus/pitboss-grill-driver` does local-HTTP control in Lua;
  `osgjps/PitBossMQTT` does BLE→MQTT.

Options:
A. [x] Adopt pytboss behind a small Python sidecar that PitBossWeb calls over
       localhost. Reuses the battle-tested codec, state decoding and
       commands; we feed it our `board_id` + `password` from /customer-grills.
       **CHOSEN (2026-08-18, after C was ruled out — see below).**
B. [ ] Port the protocol to C# inside PitBossWeb (ClientWebSocket + our codec
       + FE0B/FE0C decoding + model tables). No Python dependency, full
       control, but the most code to write and maintain.
C. [ ] Direct LAN control: pytboss HttpConnection against `http://<grill-ip>/rpc`.
       **RULED OUT 2026-08-18:** this grill runs ESP-IDF firmware 16.8.8
       (`idf_version v5.5.1`, app `pbz_firmware`), which links no HTTP server
       at all — all ports closed, `Config.Get http` → null. LAN HTTP only
       exists on the Mongoose-OS firmware line.
D. [ ] Bluetooth LE: pytboss BleConnection from a PC/Pi within BLE range. No
       WiFi/cloud at all; browsers can't do this, so a local machine is
       required.
E. [ ] Continue from-scratch RE (Frida capture of live traffic) to validate
       the decoded codec, then hand-roll C#. Slowest; best value now is only
       as validation for B.

### Chosen: Phase 3A — WSS relay via Python bridge (pytboss)
(Revised 2026-08-18 after probing: the grill's ESP-IDF firmware has no LAN
HTTP server, so the cloud relay transport is the way in. BLE is the only
cloud-free alternative and needs a device near the grill — see option D.)

1. [x] Grill identified: LAN IP 192.168.1.240 (MAC 94:51:DC:XX:XX:XX ==
       board_id PBV2-XXXXXXXXXXXX).
2. [x] Python 3.12 + pytboss 2026.8.6 + aiohttp installed in `.venv312`
       (via uv; no global installs).
3. [x] WSS relay VERIFIED WORKING (grill on): `RPC.Ping` answers
       `{channel_info:'*', rssi:-69}`; `Sys.GetInfo`:
       `{app:"pbz_firmware", fw_version:"16.8.8", idf_version:"v5.5.1",
       id:PBV2-XXXXXXXXXXXX, wifi:{sta_ip:192.168.1.240, status:"got ip"}}`.
       `RPC.List` → 404 (ESP-IDF uses `RPC.ListEx`, password-gated).
       `PB.GetState` unauthenticated → 401 (password required).
       Note: with the grill OFF the relay answers nothing (firmware gates
       its relay session on module power) — it must be on for any transport.
4. [x] LAN HTTP ruled out: ESP-IDF 16.x links no HTTP server (all common
       ports closed; `Config.Get http` → null). Nothing to enable.
5. [x] BLE: grill advertises `PBV2-XXXXXXXXXXXX` (16:0A:1D:5E:07:14), but
       GATT connect times out from this PC (rssi -97 dBm — out of range).
       Feasible only from a device next to the grill (laptop/Pi/ESP32).
6. [x] Grill password obtained (user ran scripts/fetch_grill_secret.py; saved
       to scripts/.grill_env — never printed). Authed probe VERIFIED:
       RPC.ListEx (31 methods), PB.GetFirmwareVersion (16.8.8), PB.GetState
       returns FE0B/FE0C hex frames that pytboss decodes:
       moduleIsOn=true, grillTemp≈132, set 130, p2Temp≈96, fan on, no errors.
7. [x] `scripts/grill_sidecar.py` built and RUNNING on http://127.0.0.1:8091:
       GET /health, /state, /info, /probe-targets; POST /command (actions
       power_on/off — both require confirm:true, light_on/off, prime_on/off,
       set_temp, set_probe); CORS for localhost:5219; password from env,
       never logged. Live state verified over HTTP.
8. [x] Model identified: **PBV5 P2** (user, 2026-08-18). pytboss spec:
       board PBV2, no light, 3 meat probes, 130–420°F setpoints.
       Sidecar defaults to PBV5 P2 and /info now exposes has_lights +
       meat_probes; UI hides the Light button and shows only probes ≤3.
9. [x] PitBossWeb wired: GrillRpcService (typed HttpClient → sidecar),
       GrillDetail.razor Live Status card driven by sidecar + Controls card
       enabled (two-step confirm for Power, light/prime toggles, set temp),
       Home.razor cards show Live Grill/Set/Controller state. Build green,
       app boots with no console errors.
10. [x] Safety: sidecar refuses power_on/off without confirm; UI two-step
       confirm; setpoints snap inside pytboss; password never logged.
11. [ ] End-to-end UX check: user logs in at http://localhost:5219, opens
       /grill/<grill-id> and verifies live temps + controls (safe ones first).

Later (optional): BLE bridge — a small device near the grill runs pytboss's
BleConnection and relays to the sidecar, removing Dansons from the path.
Ultra-DIY alternative: tap the controller's UART (FE-prefixed hex frames)
with custom hardware that accepts its own remote commands.

## Phase 4 (proposed): point the grill at a LOCAL relay instead of Dansons
Facts discovered 2026-08-18 that make this plausible:
- The grill's relay host is compiled into pbz_firmware (ESP-IDF 16.8.8), NOT
  in Config (Config.Get only has app{} + wifi.sta; PBX.GetParameters only
  feature_flags). Full RPC list: PBX.Set/GetParameters, PB.SendMCUCommand,
  PB.GetFirmwareVersion, Config.Get/Set, OTA.*, FS.Get/Put/Remove,
  PB.Get/SetVirtualData, PB.GetState, PB.GetTime, PB.RenameDevice,
  PB.LoadMCUFirmware(+Status), PB.QueryMCUFirmwareVersion, PBL.GetLoaderVersion,
  PB.SetMCU_UpdateFrequency, PB.SetDevicePassword, PB.WiFiAwakeWDT,
  PB.SetWifiCredentials, Wifi.Scan, Sys.GetInfo, RPC.Ping, RPC.ListEx,
  Sys.Reboot.
- The app downloads a public cert
  https://firmware.dansonscorp.com/public/websocket_cert.pem (+ .json carrying
  an ECDSA-style {r,s} signature) and pushes it to the grill via setFileContent
  chunks (= FS.Put RPC, "certificate fix" flow for IDF firmware 16.x, which
  ours is). The grill presumably uses this file as the trust anchor for its
  outbound TLS to the relay.
- The app's uploadFile/downloadFile/verifyFile methods wrap setFileContent /
  getFileContent (FS.Put / FS.Get).

Plan (test-first, restorable):
1. [ ] FS.Get the current websocket_cert.pem from the grill (backup bytes).
2. [ ] Probe FS.Get/FS.Put argument shapes + auth requirements over the WSS
       relay (safe: read-only first).
3. [ ] Generate our own CA + cert for socket.dansonscorp.com (local IP SAN).
4. [ ] Upload our cert via FS.Put and watch whether the grill still answers
       RPC.Ping through the CLOUD relay: if it silently stops trusting, the
       firmware signature-checks the file -> restore the vendor cert
       immediately (we have the exact public bytes) and stop; if it still
       works (or fails only on hostname), continue.
5. [ ] Local DNS override for socket.dansonscorp.com -> local relay IP
       (router DNS entry / dnsmasq; user router = 192.168.1.1).
6. [ ] Local relay service: TLS (our cert) WebSocket server implementing the
       /from/{deviceId} (grill dials in) + /to/{deviceId} (clients) pairing,
       mirroring the cloud relay's frame routing. ~100 lines Python.
7. [ ] Point the sidecar at it: pytboss WebSocketConnection(base_url=local).
8. [ ] Verify end-to-end: commands + status with Dansons out of the path.
Risks: firmware may verify the cert signature (r/s json) -> plan stops at 4;
router DNS change needed; local relay must stay up or the grill has no
connectivity. Fallback: flash custom firmware via UART (hardware project).

## Phase 5 (DONE 2026-09-05): ESP32 BLE bridge via ESPHome proxy

### Why / feasibility
- The grill serves the SAME JSON-RPC over BLE GATT that the relay uses
  (Mongoose-OS-style RPC service; pytboss `ble.py` targets
  `_mOS_RPC_SVC_ID_` with `data`/`tx_ctl`/`rx_ctl` chars and the
  `_mOS_DBG_SVC_ID_` log characteristic). Auth (`codec`/`timed_key`,
  uptime via PB.GetTime) is identical; status arrives as debug-log
  notifications `<==PB: FE0B…` / `<==PB: FE0C…` which pytboss parses into
  the same StateDict. `PB.GetState` polling works too.
- Only blocker has ever been range (-97 dBm from the PC). An ESP32 within a
  few metres of the controller is purely a radio; no protocol work.
- Decision (user, 2026-09-04): **stock ESPHome `bluetooth_proxy` on a
  classic ESP32 (WROOM/WROVER)** — zero custom firmware. PC side uses
  `bleak-esphome` (Bleak backend that proxies BLE through an ESPHome ESP32
  over TCP, port 6053; needs `aioesphomeapi` + `habluetooth`). All grill
  logic stays in pytboss. Alternatives rejected for now: custom NimBLE
  bridge firmware; fully standalone ESP32 controller (would re-port
  pytboss to C; codec is ~40 lines, the rest is not).
- Unknowns settled by step 1: (a) whether this ESP-IDF 16.8.8 firmware
  exposes the Mongoose GATT UUIDs (strong evidence: OTA.BluetoothStart/
  Finalize + PB.SetWifiCredentials BLE-provisioning exist in RPC.ListEx);
  (b) whether BLE RPC answers when the controller is OFF (relay does not);
  (c) `bleak-esphome`/`habluetooth` on Windows — TCP-only, so WSL2/Docker
  is a clean fallback if it misbehaves.

### Hardware
- ESP32-WROOM-32 / WROVER dev board (NOT ESP32-S2: no BLE). USB 5 V power,
  small IP65 box within ~3–5 m of the controller, WiFi must reach router.
  u.FL external-antenna variant if the grill is far from the house.
- DHCP reservation or mDNS name `grill-proxy.local` for a stable address.

### Progress log
- 2026-09-05 (later): Steps 4–6 DONE, all six verification items passed.
  Sidecar `--transport ble` (or GRILL_TRANSPORT=ble): /health
  {connected, transport, proxy_connected, rssi}, /info.transport;
  `_ble_keepalive` reconnect loop; starts with `BleConnection(None)` so
  the spec loads before the grill is heard. Verified: state pushed <1 s
  old with the controller OFF; **power_on from a cold controller ignited
  the grill** (user-requested); set_temp 225 → grillSetTemp 225; ESP32
  USB pull → proxy_connected false → recovered without a restart;
  authed RPCs (probe targets) work, so password auth is fine over BLE.
  Web UI: SidecarHealthResponse + GetHealthAsync, transport badge
  ("BLE -62 dBm" / "Cloud relay"), LinkProblem distinguishes proxy down /
  grill not heard / relay silent; footer no longer hardcodes the WSS URL.
  Unknowns settled: (a) fw 16.8.8 serves the Mongoose RPC GATT service
  unchanged; (b) BLE answers with the controller off; (c) -62 dBm with
  the ESP32 a couple of metres from the controller.
- 2026-09-05: Step 2 GATE PASSED at the grill (ESP32 on USB next to the
  controller). proxy_scan: grill at -63 dBm. ble_probe --proxy: GATT
  connect OK, RPC.Ping ok (grill-side rssi -47), Sys.GetInfo fw 16.8.8
  idf v5.5.1, RPC.ListEx = Unauthorized (probe sends no password — expected).
  **Controller OFF (mains plugged in): BLE still answers identically**
  (-59 dBm, Ping/GetInfo ok) — unlike the relay, which is silent when off.
  Fix needed: `find_grill` via `BleakScanner.find_device_by_filter` never
  matched (bare habluetooth manager has a no-op `_discover_service_info`,
  so bleak scanner-wrapper callbacks don't fire); now polls
  `manager.async_discovered_service_info(True)` and returns `info.device`.
- 2026-09-04: Steps 1 and 3 DONE, step 2 pending (need to be near the grill).
  - Hardware actually used: **classic ESP32-D0WD-V3, CP2102 on COM4, 4 MB**
    (an ESP32-C3 was tried first, has a WiFi fault — don't reuse it).
    This board's auto-reset does NOT enter download mode: hold BOOT, tap
    EN, release BOOT, then flash with `--before no-reset` (command in the
    yaml header). Once on WiFi, `esphome run` flashes OTA instead.
  - Proxy is on WiFi: **192.168.1.114**, name grill-proxy, API port 6053
    with encryption; `grill-proxy.local` mDNS does NOT resolve from the PC,
    so use the IP (GRILL_PROXY_HOST in .grill_env). Early
    `Authentication Failed` log lines were transient — ignore them.
  - PC side installed in .venv312: bleak-esphome 4.1.0, aioesphomeapi
    46.3.0, habluetooth 6.26.11 (bleak-retry-connector bumped to 4.7.0).
    esphome 2026.x in .venv (tooling).
  - `scripts/esphome_ble.py` self-test connected + registered the scanner
    OK. Grill not heard because the ESP32 was still at the PC (-97 dBm
    spot) — not a fault.
  - habluetooth gotcha (handled in esphome_ble.open_proxy): it monkey-
    patches bleak.BleakClient / bleak_retry_connector.BleakClientWithServiceCache,
    but pytboss.ble bound the originals at import; open_proxy re-points them.
  - The `BluetoothManager: does not implement _discover_service_info`
    warning is harmless (no-op subclass hook; bleak callbacks still fire).

### Steps
1. [x] Flash ESPHome proxy. `esphome/grill-proxy.yaml` (secrets in
       `esphome/secrets.yaml`, git-ignored like scripts/.grill_env):
       ```yaml
       esphome: { name: grill-proxy }
       esp32: { board: esp32dev, framework: { type: esp-idf } }
       wifi: { ssid: !secret wifi_ssid, password: !secret wifi_password }
       api: { encryption: { key: !secret api_key } }
       ota: [ { platform: esphome, password: !secret ota_password } ]
       logger:
       esp32_ble_tracker: { scan_parameters: { active: true } }
       bluetooth_proxy: { active: true }
       ```
       `pip install esphome` in `.venv` (tooling venv), then
       `esphome run esphome/grill-proxy.yaml` over USB. Verify with
       `esphome logs`: advertisement for PBV2-XXXXXXXXXXXX at > -80 dBm;
       move the board until it is.
2. [x] GATE (PASSED 2026-09-05, see progress log). At the grill, with
       the ESP32 on USB power within a few metres of the controller and the
       controller powered ON:
         a. `.venv312\Scripts\python.exe scripts\proxy_scan.py --seconds 20`
            — lists everything the proxy hears; the grill line is flagged
            `<== GRILL` with its RSSI. Move the board until > -80 dBm.
         b. `.venv312\Scripts\python.exe scripts\ble_probe.py --proxy 192.168.1.114`
            — RPC.Ping / Sys.GetInfo / RPC.ListEx through the proxy. Expect
            the 31-method list from STATE.md. If BleConnection.connect
            fails it prints the GATT table (exit 2) — save that to notes/
            and stop.
         c. Repeat (b) with the controller OFF to learn whether BLE answers
            when the relay would not.
       Optional: `esphome logs esphome\grill-proxy.yaml --device 192.168.1.114`
       (OTA log over WiFi) prints `PBV2-… rssi=… dBm` lines too.
3. [x] `scripts/esphome_ble.py` helper (written; `open_proxy`, `find_grill`,
       `grill_rssi`, `python scripts/esphome_ble.py <host> --key …` self-test):
       - `open_proxy(host, port=6053, noise_key=None) -> ProxyHandle`:
         `APIClient(...)`, `connect(login=True)`, `device_info()`,
         `bleak_esphome.connect_scanner(cli, device_info, available=True)`,
         `scanner.async_setup()`, register with `habluetooth.get_manager()`
         (create the BluetoothManager once / `set_manager()` first — pin
         the exact calls against the installed habluetooth version).
         `close()` = set unavailable, fire disconnect callbacks, unregister.
       - `find_grill(name, timeout) -> BLEDevice` via
         `BleakScanner.find_device_by_filter` on `d.name == name`
         (same filter as ble_probe.py:46-48).
       Add to `.venv312`: `bleak-esphome`, `aioesphomeapi`, `habluetooth`
       (bleak 3.0.2 + bleak_retry_connector already present). Pin versions
       in STATE.md.
4. [x] `scripts/grill_sidecar.py`: transport switch. `GrillBridge.__init__`
       hardcodes `PitBoss(WebSocketConnection(board_id), ...)`. Add
       `GRILL_TRANSPORT=wss|ble` (default wss), `GRILL_PROXY_HOST`,
       `GRILL_PROXY_PORT`, `GRILL_PROXY_KEY` read via existing `load_env()`.
       BLE path: `open_proxy()` → `find_grill(board_id)` →
       `PitBoss(BleConnection(device, disconnect_callback=...), model,
       password, control_board="PBV2")`.
       RECONNECT: `BleConnection` does NOT reconnect after a drop
       (ble.py `_on_disconnected` only flags it). In `_poll_loop`, when not
       connected and transport is ble: rescan with `find_grill` and call
       `conn.reset_device(device)` with backoff. Keep the "never overwrite
       cache with an empty decode" rule.
       `/health` gains `transport`, `proxy_connected`, last RSSI; `/info`
       gains `transport`. `command()`, `_refresh_now`, routes, CORS are
       transport-agnostic — unchanged.
5. [x] PitBossWeb: `GrillRpcService.cs` DTOs get `transport` /
       `proxy_connected`; `GrillDetail.razor` shows a transport badge and a
       clearer "proxy down" vs "grill off" message. No control changes.
6. [x] Docs: STATE.md architecture + services-to-run + BLE facts learned
       (answers to unknowns a/b/c, RSSI achieved); mark this phase done.

### Verification
1. `esphome logs` shows the grill advertisement with usable RSSI.
2. `.venv312\Scripts\python.exe scripts\ble_probe.py --proxy grill-proxy.local`
   → RPC.Ping ok, Sys.GetInfo fw 16.8.8, RPC.ListEx = 31 methods.
3. Sidecar with `GRILL_TRANSPORT=ble`: `/health` connected + transport ble;
   `/state` live temps updating (state callback should fire from BLE log
   notifications without polling).
4. Safe command only: `POST /command {action:set_temp, value:225}` and
   watch the controller display. NEVER power_on in testing (ignites).
5. Pull ESP32 power → `/health` shows proxy down; restore → sidecar
   reconnects within backoff, no restart.
6. Web UI /grill/<grill-id> shows live temps with the cloud relay unused.

### Later (not in this phase)
- Custom ESP32 firmware / standalone controller — only if the ESPHome +
  habluetooth stack proves unreliable.
- Actual alarm/threshold/notification logic (still not started; a local
  transport makes a 24/7 monitor practical).
