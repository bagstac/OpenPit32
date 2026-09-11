"""Pitboss Grill — native BLE client for the grill's Mongoose OS RPC service.

Phase 1 of docs/ESP32_FIRMWARE_PLAN.md: connect and round-trip one
unauthenticated RPC.Ping to prove the transport. Phase 2 adds the auth
codec (pytboss/codec.py's timed_key()/encode(), ported to C++) and calls
authenticated PB.GetState. Phase 7 adds POST /setup (fetches the grill
password from the Pit Boss cloud, run from the ESP32 itself) and NVS
persistence, so CONF_GRILL_PASSWORD below is now just the initial/fallback
value — see set_grill_password()'s header comment.

Modelled on esphome/components/ble_client/__init__.py's registration
pattern, but as our own top-level BLEClientBase subclass rather than the
generic ble_client + ble_client_id two-tier system: there's exactly one
physical connection here, doing one job, so there is no second "node" to
attach.
"""

import esphome.codegen as cg
from esphome.components import (
    esp32_ble,
    esp32_ble_client,
    esp32_ble_tracker,
    http_request,
    web_server_base,
)
from esphome.components import time as time_
from esphome.components.esp32_ble import BTLoggers
from esphome.components.http_request import CONF_HTTP_REQUEST_ID
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TIME_ID

# web_server_base (not the full web_server: dashboard) gets us ESPHome's
# shared httpd instance with none of web_server's own entity-dashboard
# routes — see pitboss_grill.h/.cpp's AsyncWebHandler for what actually
# gets registered on it (Phase 4: /health, /state, /info; Phase 6:
# GET/POST /alarms). http_request is Phase 6's Telegram POST — both are
# self-contained, all-default components AUTO_LOAD can create silently (no
# explicit YAML block needed), same as web_server_base today. `time` can't
# work that way — there's no single default platform (sntp/homeassistant/
# gps/...) for AUTO_LOAD to pick — so it's a DEPENDENCIES entry instead,
# forcing grill-firmware.yaml to declare one explicitly (see its `time:`
# block); cv.use_id(time_.RealTimeClock) below then binds to whichever one.
AUTO_LOAD = ["esp32_ble_client", "json", "web_server_base", "http_request"]
DEPENDENCIES = ["esp32_ble_tracker", "time"]

pitboss_grill_ns = cg.esphome_ns.namespace("pitboss_grill")
# Matches pitboss_grill.h's actual inheritance: our own BLEClientBase
# subclass, the same base esphome/components/ble_client's own BLEClient
# extends (see that component's __init__.py) — not the generic
# ble_client + ble_client_id two-tier system, since there is exactly one
# physical connection here doing one job.
PitbossGrill = pitboss_grill_ns.class_(
    "PitbossGrill", esp32_ble_client.BLEClientBase
)

CONF_NAME_PREFIX = "name_prefix"
CONF_GRILL_PASSWORD = "grill_password"
CONF_MODEL = "model"
CONF_HAS_LIGHTS = "has_lights"
CONF_MEAT_PROBES = "meat_probes"
CONF_TELEGRAM_BOT_TOKEN = "telegram_bot_token"
CONF_TELEGRAM_CHAT_ID = "telegram_chat_id"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PitbossGrill),
            # The grill's BLE address rotates between connections (see
            # docs/PROTOCOL.md), so — like the sidecar's find_grill() today —
            # this matches by advertised-name prefix instead of a fixed MAC.
            cv.Optional(CONF_NAME_PREFIX, default="PBV2-"): cv.string,
            # The per-grill RPC password. Phase 7 moved the real
            # fetch-and-persist flow onto the ESP32 itself (POST /setup ->
            # NVS — see pitboss_grill.cpp's handle_setup_()/
            # load_persisted_state_()), so this is now only the *initial*
            # fallback used until the first successful /setup: NVS wins
            # once it has a value, same as it already does for a device
            # reflashed with different (or no) secrets.yaml content. May be
            # left "" — a bench unit with nothing in NVS yet and no YAML
            # value here just starts unconfigured (see handle_health_()'s
            # `configured` field) until POST /setup runs.
            cv.Optional(CONF_GRILL_PASSWORD, default=""): cv.string_strict,
            # Reported as-is by /info — no autodetection, this project only
            # ever targets the one grill/board it's compiled for.
            cv.Optional(CONF_MODEL, default="PBV5 P2"): cv.string,
            # Also reported as-is by /info — pytboss's grills.json spec for
            # the model above was the original reference for these; defaults
            # here match that same spec for "PBV5 P2" (board PBV2: no light,
            # 3 meat probes — see docs/PLAN.md's "Decisions made" #8). Getting
            # meat_probes wrong isn't cosmetic: GrillDetail.razor's
            # AvailableSensors() uses it to decide how many probe cards to
            # render, so a wrong count here shows a probe the grill doesn't
            # have (found live, 2026-09-10 — was hardcoded to 4 in the .cpp).
            cv.Optional(CONF_HAS_LIGHTS, default=False): cv.boolean,
            cv.Optional(CONF_MEAT_PROBES, default=3): cv.int_range(min=0, max=8),
            # Phase 4: the shared httpd /health, /state, /info (and Phase 6's
            # /alarms) register on.
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
            # Phase 6: alarm timer/temp-target monitor + Telegram notify().
            # Both bot fields default to "" — see grill-firmware.yaml's
            # comment on what an empty value does (alarms still fire and
            # drop on schedule; nothing is sent anywhere).
            cv.Optional(CONF_TELEGRAM_BOT_TOKEN, default=""): cv.string_strict,
            cv.Optional(CONF_TELEGRAM_CHAT_ID, default=""): cv.string_strict,
            cv.GenerateID(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(
                http_request.HttpRequestComponent
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
)


async def to_code(config):
    esp32_ble.register_bt_logger(BTLoggers.GATT, BTLoggers.SMP)
    cg.add_define("USE_ESP32_BLE_UUID")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_client(var, config)
    cg.add(var.set_name_prefix(config[CONF_NAME_PREFIX]))
    cg.add(var.set_grill_password(config[CONF_GRILL_PASSWORD]))
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_has_lights(config[CONF_HAS_LIGHTS]))
    cg.add(var.set_meat_probes(config[CONF_MEAT_PROBES]))
    cg.add(var.set_telegram_bot_token(config[CONF_TELEGRAM_BOT_TOKEN]))
    cg.add(var.set_telegram_chat_id(config[CONF_TELEGRAM_CHAT_ID]))

    web_server = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(var.set_web_server_base(web_server))

    time_var = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(time_var))
    http_request_var = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
    cg.add(var.set_http_request(http_request_var))
