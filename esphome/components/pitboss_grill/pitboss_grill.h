#pragma once

// Connects to the grill's Mongoose OS RPC-over-GATT service natively (no
// proxy, no PC-side sidecar) — a direct port of the connection half of
// pytboss/ble.py's BleConnection (see docs/PROTOCOL.md section 2 and
// docs/ESP32_FIRMWARE_PLAN.md for the protocol facts this leans on) — run
// as a native BLE central instead of driven remotely through an ESP32
// proxy.
//
// Phase 1: connect, discover the RPC/debug-log characteristics, register
// for notifications, round-trip one unauthenticated RPC.Ping. Phase 2 adds
// the auth codec (pytboss/codec.py's timed_key()/encode(), ported to C++)
// and an authenticated PB.GetState call. Phase 3 decodes the sc_11/sc_12
// (FE0B/FE0C) status/temperature frames into GrillState below — both the
// authenticated PB.GetState reply and the grill's own unauthenticated
// debug-log pushes carry the same two frames, decoded the same way. All
// three phases are bench-verified against real hardware as of 2026-09-09.
//
// NOT yet implemented here (later phases): MCU commands (set-temperature,
// turn-on/off), and exposing GrillState as actual ESPHome entities /
// REST endpoints rather than just an internal struct + log lines.

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

  /// The per-grill RPC password (today: scripts/.grill_env's
  /// GRILL_PASSWORD). See Phase 2 notes in docs/ESP32_FIRMWARE_PLAN.md —
  /// Phase 7 replaces this compile-time secret with a fetch-and-persist
  /// flow run from the ESP32 itself.
  void set_grill_password(const std::string &password) { this->grill_password_ = password; }

  // Decoded status/temperature state — the bit-level fields inside the
  // sc_11/sc_12 (FE0B/FE0C) frames, decoded per this project's specific
  // control board (see parse_status_frame_()/parse_temperature_frame_() in
  // the .cpp, ported from pytboss's grills.json "PBV2" control board entry).
  // A different control board reads different byte offsets — grills.json
  // ships ~20 of them as vendor JS, run through pytboss's own JS
  // interpreter — so this is deliberately hardcoded to the one board this
  // project's grill uses (scripts/grill_sidecar.py's CONTROL_BOARD), not a
  // general decode.
  struct GrillState {
    bool has_status{false};
    bool has_temperatures{false};

    // -- from the FE0B status frame --
    bool module_is_on{false};
    bool err1{false};
    bool err2{false};
    bool err3{false};
    bool high_temp_err{false};
    bool fan_err{false};
    bool hot_err{false};
    bool motor_err{false};
    bool no_pellets{false};
    bool er_l{false};
    bool fan_state{false};
    bool hot_state{false};
    bool motor_state{false};
    bool light_state{false};
    bool prime_state{false};
    uint8_t recipe_step{0};
    uint32_t recipe_time_s{0};

    // -- from the FE0C temperature frame --
    // Degrees, in whatever unit the grill itself is set to (is_fahrenheit
    // says which — both frames are converted to match it, mirroring
    // pytboss's own ftoc() step). -1 means "no reading": a disconnected
    // probe, or the grill's own 960 sentinel — see convert_temperature_()
    // in the .cpp. p1-p4 are meat probes; the rest are the chamber itself.
    int16_t p1_temp{-1};
    int16_t p2_temp{-1};
    int16_t p3_temp{-1};
    int16_t p4_temp{-1};
    int16_t smoker_act_temp{-1};
    int16_t grill_temp{-1};
    int16_t grill_set_temp{-1};
    bool is_fahrenheit{true};
  };

  /// Latest decoded status/temperature state — see GrillState above. Updated
  /// from both the 15s authenticated PB.GetState cycle and the grill's own
  /// unauthenticated debug-log pushes, whichever arrives; has_status/
  /// has_temperatures are false until the first frame of each kind lands.
  const GrillState &grill_state() const { return this->grill_state_; }

 protected:
  void resolve_characteristics_();
  void register_for_notifications_();
  void send_ping_();
  void send_get_state_cycle_();
  void send_get_time_();
  void send_get_state_(double uptime);
  void on_get_time_reply_(const std::string &json);
  void on_get_state_reply_(const std::string &json);
  void on_rpc_notify_(const uint8_t *data, uint16_t len);
  void on_rpc_read_(const uint8_t *data, uint16_t len);
  void on_debug_log_(const uint8_t *data, uint16_t len);
  void request_next_reply_chunk_();
  void write_rpc_command_(const std::string &json);
  void write_next_rpc_chunk_();
  void on_rpc_write_complete_(uint16_t handle, esp_gatt_status_t status);
  void parse_status_frame_(const std::string &hex);
  void parse_temperature_frame_(const std::string &hex);

  std::string name_prefix_{"PBV2-"};
  std::string grill_password_;

  // Only one RPC request is ever in flight at a time (see write_rpc_command_
  // and the reply-reassembly fields below) — this says which one, so
  // on_rpc_read_'s completion handler knows how to interpret the reply
  // without needing to track/match request ids of our own.
  enum class PendingReply { NONE, PING, GET_TIME, GET_STATE };
  PendingReply pending_reply_{PendingReply::NONE};

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

  // Outbound request chunking, paced off each chunk's own
  // ESP_GATTC_WRITE_CHAR_EVT rather than fired back-to-back: write_value()
  // defaults to write-without-response, which only hands the payload to the
  // BT controller's internal buffer pool — queuing several chunks before
  // that pool drains crashed the whole BT stack (confirmed on the bench,
  // see write_rpc_command_()'s comment). GetTime's ~2-3 total writes never
  // hit this; GetState's ~7 always did.
  std::string rpc_write_json_;
  size_t rpc_write_offset_{0};
  bool rpc_write_in_progress_{false};

  GrillState grill_state_;
};

}  // namespace pitboss_grill
}  // namespace esphome

#endif
