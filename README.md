# OpenPit32

Local, cloud-free control of a Pit Boss pellet grill over Bluetooth LE, from
a small web app on your PC, via an ESP32 sitting next to the grill — and,
as of this firmware, running **on** that ESP32, not proxied through it from
a PC-side process.

- Live grill / smoker / probe temperatures, pushed by the grill (no polling lag)
- Set temperature and power on/off — including **powering the grill on
  while its controller is switched off**, which the official cloud path
  cannot do
- No Pit Boss cloud dependency at run time; the cloud is used exactly once,
  to fetch the grill's Bluetooth password from your account — and that
  fetch now runs on the ESP32 itself, not a PC-side process
- Link diagnostics page (BLE and WiFi signal, link status)
- Alarms: set a target temperature (grill/smoker/any probe) or a countdown
  timer and get a **Telegram message** when it's reached — fires even with
  no browser open anywhere, since the grill's own ESP32 evaluates and
  delivers alarms directly
- Installable as a PWA (add to home screen / desktop) for a native-app feel;
  a redeploy takes over any open copy immediately rather than requiring
  everyone to close and reopen it first
- Optional Docker deployment with a real login form (password-manager
  friendly) for exposing it beyond your LAN — see
  [docker/README.md](docker/README.md)

Unofficial, unaffiliated with Dansons / Pit Boss. It talks to a real appliance
that makes fire — see [Safety](#safety).

## How it works

```
Browser ──> OpenPit32 (Blazor WebAssembly)
               │  HTTP/JSON
               ▼
         ESP32 running custom firmware (esphome/grill-firmware.yaml)
               │  native BLE central — Mongoose OS RPC-over-GATT
               ▼
         grill controller
```

No PC-side Bluetooth process of any kind — the browser talks straight to
the ESP32's own REST API (`/health`, `/state`, `/info`, `/command`,
`/alarms`, `/setup`), and the ESP32 is the thing that actually speaks the
grill's protocol, evaluates alarms on a timer, and POSTs to Telegram's Bot
API when one fires. (A Docker deployment adds one more small process purely
for the login gate — see [Deploying](#deploying) below — but it holds no
grill state of any kind.)

The grill controller is a Mongoose OS device that exposes JSON-RPC over
GATT. [pytboss](https://github.com/dknowles2/pytboss) was this project's
original reference for that protocol (codec, commands, per-model tables);
`esphome/components/pitboss_grill` is a from-scratch C++ port of the pieces
this specific grill needs, not a wrapper around the Python library, which
isn't a runtime dependency here at all anymore. Details in
[docs/PROTOCOL.md](docs/PROTOCOL.md); the full migration story — every
phase, every real-hardware bug found and fixed along the way — is in
[docs/ESP32_FIRMWARE_PLAN.md](docs/ESP32_FIRMWARE_PLAN.md).

Tested on a **PBV5 P2** (PBV2 control board, firmware 16.8.8) — this
firmware is pinned to that one grill/board (fixed command table, setpoints,
probe count); a different model or board generation would need its own
command table re-derived the same way.

## Hardware

- A Pit Boss grill with a WiFi/Bluetooth controller (board id `PBV2-…`).
- An ESP32 dev board (classic ESP32-WROOM/D0WD-V3 class, 4 MB flash) with
  USB power within a few metres of the controller, facing its front panel
  and clear of the grill's metal body.
- A phone with Telegram, if you want alarm notifications (optional — alarms
  still fire and clear on schedule either way, they just log instead of
  sending anywhere without a bot configured).

## Setup

### 1. Flash the ESP32

```
python -m venv .venv
.venv\Scripts\pip install -r requirements-esphome.txt
copy esphome\secrets.yaml.example esphome\secrets.yaml   # fill in WiFi + keys
.venv\Scripts\esphome run esphome\grill-firmware.yaml    # USB the first time
```

`api_key`/`ota_password` in `secrets.yaml` are yours to choose (`api_key`:
`openssl rand -base64 32`). `grill_password`/`telegram_bot_token`/
`telegram_chat_id` can be left blank — the web app's setup dialog and
`POST /config` can set them at runtime instead (see step 3), persisted on
the ESP32's own flash. Some boards need the BOOT/EN dance to enter download
mode — see the comments at the top of
[esphome/grill-firmware.yaml](esphome/grill-firmware.yaml). Note the IP the
board gets (from `esphome logs` or your router); mDNS is unreliable on
Windows.

### 2. Web app

Requires the [.NET 10 SDK](https://dotnet.microsoft.com/download). Edit
`GRILL_HOST` in [OpenPit32/Program.cs](OpenPit32/Program.cs) to the ESP32's
LAN IP from step 1, then:

```
dotnet run --project OpenPit32\OpenPit32.csproj --urls http://localhost:5219
```

Open http://localhost:5219. On first run click **Fetch grill password**: the
dialog sends your Pit Boss account email/password straight to the ESP32's
own `POST /setup`, which logs in once, reads the grill's password from your
account, saves it to its own flash, and discards the account session. The
browser and PC store nothing.

From then on: the Home card shows live temps; **Status & controls** is the
grill page (including the Alarms card); **Bridge health** shows the BLE and
WiFi links.

### Or with Docker

`docker-compose.yml` runs the web app and a small login-only service as two
containers on one host (skip the venv/.NET SDK above) — useful for exposing
the app beyond your LAN behind a real login. See
[docker/README.md](docker/README.md).

## Grill ESP32 HTTP API

Served directly by the ESP32 (`esphome/components/pitboss_grill`) — same
shape whether you're hitting it directly (bare-metal dev) or through
nginx's reverse proxy (Docker):

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | `configured`, `connected`, BLE `rssi`, WiFi `rssi`/uptime, last error |
| GET | `/state` | decoded grill state (temps, setpoint, probes, fan/igniter/auger, error flags) |
| GET | `/info` | board id, model, accepted setpoints, probe count, has_lights |
| POST | `/config` | `{"error_display_threshold"?, "telegram_bot_token"?, "telegram_chat_id"?}` |
| POST | `/command` | `{"action": "set_temp", "value": 225}` · `power_on` / `power_off` need `"confirm": true` |
| POST | `/setup` | `{"email","password","country"?,"grill_id"?}` — one-time password fetch, run from the ESP32 |
| GET | `/alarms` | `{alarms: [...]}` — active temp/timer alarms |
| POST | `/alarms` | `{"kind":"temp","sensor","comparison","target","label"?}` or `{"kind":"timer","duration_seconds","label"?}` |
| DELETE | `/alarms/{id}` | cancel one alarm |

## Safety

- `power_on` **ignites the grill**. The UI asks for a second click and the
  firmware refuses power commands without `confirm: true`, but nothing here
  can check that the lid is where it should be or that pellets are loaded.
- Keep `esphome/secrets.yaml` private: it's your WiFi password and OTA
  credential. Git-ignored. The grill's own Bluetooth password lives on the
  ESP32's flash (NVS) once fetched, not in this repo at all.
- The grill's password authentication uses a time-derived key; a slow BLE
  link occasionally gets a spurious `Unauthorized`, which the firmware
  retries once. Persistent failures usually mean a weak BLE signal — check
  Bridge health.

## Credits and sources

This project stands on other people's work:

- **[pytboss](https://github.com/dknowles2/pytboss)** by David Knowles —
  Apache-2.0. The original reference for the Pit Boss protocol this
  firmware ports into C++: the password codec, the MCU command and status
  tables, and the RPC-over-GATT framing. Its companion Home Assistant
  integration [ha-pitboss](https://github.com/dknowles2/ha-pitboss) showed
  the BLE-discovery patterns this project's earlier, now-retired PC-side
  BLE stack reused.
- **[ESPHome](https://esphome.io/)** — the framework `esphome/components/pitboss_grill`
  is built on (external component + native ESP-IDF/NimBLE BLE central), and
  the `http_request`/`time`/`web_server_base` components it reuses for the
  Telegram POST, real-time clock, and REST API respectively.
- **[aiohttp](https://github.com/aio-libs/aiohttp)** (Apache-2.0/MIT) for
  the login-only Docker service; **[Bootstrap](https://getbootstrap.com/)**
  (MIT) via the Blazor template.
- **[Mongoose OS](https://mongoose-os.com/docs/mongoose-os/api/rpc/rpc-gatts.md)**
  documentation of RPC-over-GATT, which is what the grill speaks.
- Community projects that documented the grill from other angles:
  [xeudoxus/pitboss-grill-driver](https://github.com/xeudoxus/pitboss-grill-driver)
  (local HTTP control on older firmware) and
  [osgjps/PitBossMQTT](https://github.com/osgjps/PitBossMQTT) (BLE → MQTT).
- The one-time account login mirrors the requests the official Pit Boss app
  makes (documented in [docs/PROTOCOL.md](docs/PROTOCOL.md)); no vendor code
  or assets are included.

## License

[MIT](LICENSE).
