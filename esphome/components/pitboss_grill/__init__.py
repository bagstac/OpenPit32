"""Pitboss Grill — native BLE client for the grill's Mongoose OS RPC service.

Phase 1 of docs/ESP32_FIRMWARE_PLAN.md: connect and round-trip one
unauthenticated RPC.Ping to prove the transport. Phase 2 adds the auth
codec (pytboss/codec.py's timed_key()/encode(), ported to C++) and calls
authenticated PB.GetState.

Modelled on esphome/components/ble_client/__init__.py's registration
pattern, but as our own top-level BLEClientBase subclass rather than the
generic ble_client + ble_client_id two-tier system: there's exactly one
physical connection here, doing one job, so there is no second "node" to
attach.
"""

import esphome.codegen as cg
from esphome.components import esp32_ble, esp32_ble_client, esp32_ble_tracker, web_server_base
from esphome.components.esp32_ble import BTLoggers
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
import esphome.config_validation as cv
from esphome.const import CONF_ID

# web_server_base (not the full web_server: dashboard) gets us ESPHome's
# shared httpd instance with none of web_server's own entity-dashboard
# routes — see pitboss_grill.h/.cpp's AsyncWebHandler for what actually
# gets registered on it (Phase 4: /health, /state, /info).
AUTO_LOAD = ["esp32_ble_client", "json", "web_server_base"]
DEPENDENCIES = ["esp32_ble_tracker"]

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

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PitbossGrill),
            # The grill's BLE address rotates between connections (see
            # docs/PROTOCOL.md), so — like the sidecar's find_grill() today —
            # this matches by advertised-name prefix instead of a fixed MAC.
            cv.Optional(CONF_NAME_PREFIX, default="PBV2-"): cv.string,
            # The per-grill RPC password (today: scripts/.grill_env's
            # GRILL_PASSWORD, fetched once from the Pit Boss cloud). Phase 7
            # of the plan moves that fetch onto the ESP32 itself and
            # persists this to NVS instead of a compile-time secret; for
            # now, bench-testing the auth codec needs it available at all.
            cv.Required(CONF_GRILL_PASSWORD): cv.string_strict,
            # Reported as-is by /info (scripts/grill_sidecar.py's
            # DEFAULT_MODEL) — no autodetection, same as the sidecar today.
            cv.Optional(CONF_MODEL, default="PBV5 P2"): cv.string,
            # Phase 4: the shared httpd /health, /state, /info register on.
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
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

    web_server = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(var.set_web_server_base(web_server))
