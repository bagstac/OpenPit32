#pragma once

// Phase 1 of docs/ESP32_FIRMWARE_PLAN.md: connect to the grill's Mongoose OS
// RPC-over-GATT service natively (no proxy, no PC-side sidecar) and
// round-trip one unauthenticated RPC.Ping, to de-risk the BLE framing before
// anything grill-specific gets built on it. A direct port of the connection
// half of pytboss/ble.py's BleConnection (see docs/PROTOCOL.md section 2 and
// docs/ESP32_FIRMWARE_PLAN.md for the protocol facts this leans on) — run as
// a native BLE central instead of driven remotely through an ESP32 proxy.
//
// NOT yet implemented here (later phases): the auth codec, JSON request/
// reply parsing, MCU commands, and FE0B/FE0C state decoding. This phase only
// proves the transport: connect, discover the RPC characteristics, register
// for notifications, and get one readable reply back.

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#include <esp_gattc_api.h>
#include <string>
#include <vector>

namespace esphome {
namespace pitboss_grill {

namespace espbt = esphome::esp32_ble_tracker;
using esphome::esp32_ble::ESPBTUUID;
using namespace esp32_ble_client;

class PitbossGrill : public BLEClientBase {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  bool parse_device(const espbt::ESPBTDevice &device) override;
  bool gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  /// Advertised-name prefix to match — the grill's BLE address rotates, so
  /// (like the sidecar's esphome_ble.find_grill() today) this matches by
  /// name instead of a fixed MAC. Defaults to this project's tested board.
  void set_name_prefix(const std::string &prefix) { this->name_prefix_ = prefix; }

 protected:
  void resolve_characteristics_();
  void register_for_notifications_();
  void send_ping_();
  void on_rpc_notify_(const uint8_t *data, uint16_t len);
  void on_rpc_read_(const uint8_t *data, uint16_t len);
  void on_debug_log_(const uint8_t *data, uint16_t len);
  void request_next_reply_chunk_();
  void write_rpc_command_(const std::string &json);

  std::string name_prefix_{"PBV2-"};

  // Mongoose OS RPC-over-GATT + debug-log characteristic handles, resolved
  // once per connection from ESP_GATTC_SEARCH_CMPL_EVT.
  uint16_t rpc_data_handle_{0};
  uint16_t rpc_tx_ctl_handle_{0};
  uint16_t rpc_rx_ctl_handle_{0};
  uint16_t debug_log_handle_{0};
  uint8_t notifies_expected_{0};
  uint8_t notifies_confirmed_{0};

  // Ongoing bench liveness indicator (see the periodic "status" log in
  // setup()): a plain counter rather than a per-call log line, both to
  // avoid flooding the log at the BLE stack's own event rate and to stay
  // immune to any possibility of a dropped/throttled log line.
  uint32_t gattc_call_count_{0};

  // In-flight reply reassembly: an rx_ctl notification announces a 4-byte
  // length, then that many bytes are read back off rpc_data one GATT read at
  // a time (the characteristic is too small to carry the whole reply).
  std::vector<uint8_t> rpc_reply_buffer_;
  uint32_t rpc_reply_expected_{0};
  bool rpc_reply_in_progress_{false};
};

}  // namespace pitboss_grill
}  // namespace esphome

#endif
