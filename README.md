# OpenPit32

Local, cloud-free control of a Pit Boss pellet grill over Bluetooth LE, from
a small web app on your PC, via an ESP32 sitting next to the grill.

- Live grill / smoker / probe temperatures, pushed by the grill (no polling lag)
- Set temperature, prime, light (where fitted), and power on/off — including
  **powering the grill on while its controller is switched off**, which the
  official cloud path cannot do
- No Pit Boss cloud dependency at run time; the cloud is used exactly once, to
  fetch the grill's Bluetooth password from your account
- Link diagnostics page (BLE and WiFi signal, proxy status)

Unofficial, unaffiliated with Dansons / Pit Boss. It talks to a real appliance
that makes fire — see [Safety](#safety).

## How it works

```
Browser ──> OpenPit32 (Blazor WebAssembly, localhost:5219)
               │  HTTP/JSON
               ▼
         grill_sidecar.py (aiohttp, 127.0.0.1:8091)
               │  pytboss BleConnection ──> habluetooth / bleak-esphome
               │  ESPHome native API (TCP 6053, encrypted, over WiFi)
               ▼
         ESP32 running ESPHome `bluetooth_proxy`   ── BLE ──>   grill controller
```

The grill controller is a Mongoose OS device that exposes JSON-RPC over
GATT. [pytboss](https://github.com/dknowles2/pytboss) implements that
protocol (codec, commands, per-model tables); this project only adds the
ESP32 hop, a tiny HTTP sidecar, and the web UI. Details in
[docs/PROTOCOL.md](docs/PROTOCOL.md).

Tested on a **PBV5 P2** (PBV2 control board, firmware 16.8.8). Other
PBV2-board models known to pytboss are selectable in the setup dialog;
other board generations are untested.

## Hardware

- A Pit Boss grill with a WiFi/Bluetooth controller (board id `PBV2-…`).
- An ESP32 dev board (classic ESP32-WROOM class, 4 MB flash) with USB power
  within a few metres of the controller, facing its front panel and clear of
  the grill's metal body. An ESP32-C3 should also work (see the yaml).
- A Windows/Linux/macOS PC on the same WiFi. The PC does not need Bluetooth.

## Setup

### 1. Flash the ESP32 proxy

```
python -m venv .venv
.venv\Scripts\pip install -r requirements-esphome.txt
copy esphome\secrets.yaml.example esphome\secrets.yaml   # fill in WiFi + keys
.venv\Scripts\esphome run esphome\grill-proxy.yaml       # USB the first time
```

`api_key` in `secrets.yaml` is a base64 32-byte key (`openssl rand -base64 32`).
Some boards need the BOOT/EN dance to enter download mode — see the comments
at the top of [esphome/grill-proxy.yaml](esphome/grill-proxy.yaml). Note the
IP the board gets (from `esphome logs` or your router); mDNS is unreliable on
Windows.

### 2. Sidecar

```
python -m venv .venv312            # Python 3.12+
.venv312\Scripts\pip install -r requirements.txt
copy scripts\.grill_env.example scripts\.grill_env   # set GRILL_PROXY_HOST / GRILL_PROXY_KEY
.venv312\Scripts\python scripts\grill_sidecar.py
```

Check what the ESP32 hears first if you like:
`.venv312\Scripts\python scripts\proxy_scan.py` lists advertisements with
RSSI and flags the grill. Aim for better than −80 dBm.

### 3. Web app

Requires the [.NET 10 SDK](https://dotnet.microsoft.com/download).

```
dotnet run --project OpenPit32\OpenPit32.csproj --urls http://localhost:5219
```

Open http://localhost:5219. On first run click **Fetch grill password**: the
dialog sends your Pit Boss account email/password to the local sidecar, which
logs in once, reads the grill's password from your account, writes it to
`scripts/.grill_env`, and discards the account session. The browser stores
nothing. (Command line alternative: `scripts/pitboss_cloud.py` with
`PITBOSS_EMAIL` / `PITBOSS_PASSWORD` in the environment.)

From then on: the Home card shows live temps; **Status & controls** is the
grill page; **Bridge health** shows the BLE and WiFi links.

### Or with Docker

docker-compose.yml runs the sidecar and web app as two containers
on one host (skip the venvs/`.NET SDK` above). See
[docker/README.md](docker/README.md).

## Sidecar HTTP API

| Method | Path | Purpose |
|---|---|---|
| GET | `/health` | `configured`, `connected`, `proxy_connected`, BLE `rssi`, `proxy_wifi_rssi`, last error |
| GET | `/state` | decoded grill state (temps, setpoint, probes, fan/igniter/auger, error flags) |
| GET | `/info` | board id, model, firmware, accepted setpoints, probe count, has_lights |
| GET | `/models` | grill models pytboss knows for the PBV2 board |
| POST | `/command` | `{"action": "set_temp", "value": 225}` · `power_on` / `power_off` need `"confirm": true` · `prime_on/off`, `light_on/off`, `set_probe` |
| POST | `/setup` | `{"email","password","country"?,"grill_id"?,"model"?}` — one-time password fetch |

Binds to 127.0.0.1 by default (`GRILL_SIDECAR_HOST` to change it); CORS is
allowed for `http://localhost:*` plus whatever `GRILL_SIDECAR_ORIGINS` lists.

## Safety

- `power_on` **ignites the grill**. The UI asks for a second click and the
  sidecar refuses power commands without `confirm: true`, but nothing here can
  check that the lid is where it should be or that pellets are loaded.
- Keep `scripts/.grill_env` and `esphome/secrets.yaml` private: between them
  they are your WiFi password and the key to your grill. Both are git-ignored.
- The grill's password authentication uses a time-derived key; a slow BLE
  link occasionally gets a spurious `Unauthorized`, which the sidecar retries
  once. Persistent failures usually mean a weak BLE signal — check Bridge
  health.

## Credits and sources

This project stands on other people's work:

- **[pytboss](https://github.com/dknowles2/pytboss)** by David Knowles —
  Apache-2.0. The Pit Boss protocol implementation: the password codec, the
  MCU command and status tables for 140+ models, and the WebSocket / BLE
  transports. Its companion Home Assistant integration
  [ha-pitboss](https://github.com/dknowles2/ha-pitboss) showed the
  BLE-discovery patterns reused here.
- **[ESPHome](https://esphome.io/)** — the `bluetooth_proxy` and
  `esp32_ble_tracker` components that turn a bare ESP32 into a network BLE
  radio, and [aioesphomeapi](https://github.com/esphome/aioesphomeapi) (MIT).
- **[bleak-esphome](https://github.com/bluetooth-devices/bleak-esphome)**
  (MIT) and **[habluetooth](https://github.com/bluetooth-devices/habluetooth)**
  (Apache-2.0) from the Bluetooth-Devices / Home Assistant community — the
  Bleak backend that drives an ESPHome proxy, and the Bluetooth manager it
  plugs into. Also [bleak](https://github.com/hbldh/bleak) and
  [bleak-retry-connector](https://github.com/bluetooth-devices/bleak-retry-connector)
  (MIT).
- **[aiohttp](https://github.com/aio-libs/aiohttp)** (Apache-2.0/MIT) for the
  sidecar; **[Bootstrap](https://getbootstrap.com/)** (MIT) via the Blazor
  template.
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
