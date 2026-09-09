"""Pitboss Grill — native BLE client for the grill's Mongoose OS RPC service.

Phase 1 of docs/ESP32_FIRMWARE_PLAN.md: connect and round-trip one
unauthenticated RPC.Ping to prove the transport, before anything
grill-specific (auth, commands, state decoding) gets built on it.

Modelled on esphome/components/ble_client/__init__.py's registration
pattern, but as our own top-level BLEClientBase subclass rather than the
generic ble_client + ble_client_id two-tier system: there's exactly one
physical connection here, doing one job, so there is no second "node" to
attach.
"""

import esphome.codegen as cg
from esphome.components import esp32_ble, esp32_ble_client, esp32_ble_tracker
from esphome.components.esp32_ble import BTLoggers
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = ["esp32_ble_client"]
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

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PitbossGrill),
            # The grill's BLE address rotates between connections (see
            # docs/PROTOCOL.md), so — like the sidecar's find_grill() today —
            # this matches by advertised-name prefix instead of a fixed MAC.
            cv.Optional(CONF_NAME_PREFIX, default="PBV2-"): cv.string,
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
