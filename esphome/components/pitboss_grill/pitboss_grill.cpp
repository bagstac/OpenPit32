#include "pitboss_grill.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32

namespace esphome {
namespace pitboss_grill {

static const char *const TAG = "pitboss_grill";

// Mongoose OS RPC-over-GATT + debug-log service. These UUIDs are exactly the
// 16 raw ASCII bytes Mongoose uses as a 128-bit UUID (no hex decoding) — see
// pytboss/ble.py's _uuid() and docs/PROTOCOL.md section 2. Same scheme this
// project already relies on via esphome/grill-proxy.yaml's bluetooth_proxy,
// just consumed natively here instead of proxied to a PC-side sidecar.
static const std::string SERVICE_RPC = "_mOS_RPC_SVC_ID_";
static const std::string CHAR_RPC_DATA = "_mOS_RPC_data___";
static const std::string CHAR_RPC_TX_CTL = "_mOS_RPC_tx_ctl_";
static const std::string CHAR_RPC_RX_CTL = "_mOS_RPC_rx_ctl_";
static const std::string SERVICE_DEBUG = "_mOS_DBG_SVC_ID_";
static const std::string CHAR_DEBUG_LOG = "0mOS_DBG_log___0";

// A frame is a chunked GATT write: 4-byte big-endian length on the "ctl"
// characteristic, then the JSON body in <=20-byte pieces on "data" — ports
// pytboss/ble.py's _encode_len()/_send_prepared_command().
static const size_t RPC_CHUNK_SIZE = 20;

void PitbossGrill::setup() {
  BLEClientBase::setup();
  this->set_auto_connect(true);
}

void PitbossGrill::loop() { BLEClientBase::loop(); }

void PitbossGrill::dump_config() {
  ESP_LOGCONFIG(TAG, "Pitboss Grill (Phase 1 — bench de-risking):");
  ESP_LOGCONFIG(TAG, "  Advertised-name prefix: %s", this->name_prefix_.c_str());
}

bool PitbossGrill::parse_device(const espbt::ESPBTDevice &device) {
  // Mirrors BLEClientBase::parse_device()'s contract (state/DISCOVERED
  // handling) but matches by advertised-name prefix instead of a fixed MAC:
  // the grill's BLE address is random and rotates between connections (see
  // docs/PROTOCOL.md), exactly why the sidecar's esphome_ble.find_grill()
  // does the same thing today.
  if (this->state() != espbt::ClientState::IDLE)
    return false;
  const std::string &name = device.get_name();
  if (name.compare(0, this->name_prefix_.size(), this->name_prefix_) != 0)
    return false;

  ESP_LOGI(TAG, "Found grill '%s', rssi=%d", name.c_str(), device.get_rssi());
  this->set_state(espbt::ClientState::DISCOVERED);
  this->set_address(device.address_uint64());
  this->set_remote_addr_type(device.get_address_type());
  return true;
}

void PitbossGrill::resolve_characteristics_() {
  auto *data_chr = this->get_characteristic(ESPBTUUID::from_raw(SERVICE_RPC), ESPBTUUID::from_raw(CHAR_RPC_DATA));
  auto *tx_ctl_chr = this->get_characteristic(ESPBTUUID::from_raw(SERVICE_RPC), ESPBTUUID::from_raw(CHAR_RPC_TX_CTL));
  auto *rx_ctl_chr = this->get_characteristic(ESPBTUUID::from_raw(SERVICE_RPC), ESPBTUUID::from_raw(CHAR_RPC_RX_CTL));
  auto *debug_chr = this->get_characteristic(ESPBTUUID::from_raw(SERVICE_DEBUG), ESPBTUUID::from_raw(CHAR_DEBUG_LOG));

  if (data_chr == nullptr || tx_ctl_chr == nullptr || rx_ctl_chr == nullptr || debug_chr == nullptr) {
    ESP_LOGE(TAG, "Grill did not expose the expected Mongoose OS RPC/debug service — "
                  "found data=%d tx_ctl=%d rx_ctl=%d debug=%d",
             data_chr != nullptr, tx_ctl_chr != nullptr, rx_ctl_chr != nullptr, debug_chr != nullptr);
    return;
  }

  this->rpc_data_handle_ = data_chr->handle;
  this->rpc_tx_ctl_handle_ = tx_ctl_chr->handle;
  this->rpc_rx_ctl_handle_ = rx_ctl_chr->handle;
  this->debug_log_handle_ = debug_chr->handle;
  ESP_LOGI(TAG, "Resolved RPC service: data=0x%04x tx_ctl=0x%04x rx_ctl=0x%04x debug=0x%04x", this->rpc_data_handle_,
           this->rpc_tx_ctl_handle_, this->rpc_rx_ctl_handle_, this->debug_log_handle_);
  this->register_for_notifications_();
}

void PitbossGrill::register_for_notifications_() {
  // Both notify registrations must round-trip (ESP_GATTC_REG_FOR_NOTIFY_EVT)
  // before it's safe to write a request — otherwise a fast reply could beat
  // our own notify subscription and get silently dropped.
  this->notifies_expected_ = 2;
  this->notifies_confirmed_ = 0;
  auto status = esp_ble_gattc_register_for_notify(this->get_gattc_if(), this->get_remote_bda(),
                                                   this->rpc_rx_ctl_handle_);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "register_for_notify(rx_ctl) failed, status=%d", status);
  }
  status = esp_ble_gattc_register_for_notify(this->get_gattc_if(), this->get_remote_bda(), this->debug_log_handle_);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "register_for_notify(debug_log) failed, status=%d", status);
  }
}

void PitbossGrill::write_rpc_command_(const std::string &json) {
  auto *data_chr = this->get_characteristic(ESPBTUUID::from_raw(SERVICE_RPC), ESPBTUUID::from_raw(CHAR_RPC_DATA));
  auto *tx_ctl_chr = this->get_characteristic(ESPBTUUID::from_raw(SERVICE_RPC), ESPBTUUID::from_raw(CHAR_RPC_TX_CTL));
  if (data_chr == nullptr || tx_ctl_chr == nullptr) {
    ESP_LOGE(TAG, "Cannot send RPC command — characteristics not resolved");
    return;
  }

  uint8_t len_bytes[4] = {
      static_cast<uint8_t>((json.size() >> 24) & 0xFF),
      static_cast<uint8_t>((json.size() >> 16) & 0xFF),
      static_cast<uint8_t>((json.size() >> 8) & 0xFF),
      static_cast<uint8_t>(json.size() & 0xFF),
  };
  auto err = tx_ctl_chr->write_value(len_bytes, sizeof(len_bytes));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Writing RPC length failed, err=%d", err);
    return;
  }

  for (size_t i = 0; i < json.size(); i += RPC_CHUNK_SIZE) {
    size_t chunk_len = std::min(RPC_CHUNK_SIZE, json.size() - i);
    err = data_chr->write_value(reinterpret_cast<uint8_t *>(const_cast<char *>(json.data() + i)), chunk_len);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Writing RPC chunk %d failed, err=%d", static_cast<int>(i / RPC_CHUNK_SIZE), err);
      return;
    }
  }
  ESP_LOGD(TAG, "Sent RPC request (%d bytes): %s", static_cast<int>(json.size()), json.c_str());
}

void PitbossGrill::send_ping_() {
  // Unauthenticated per docs/PROTOCOL.md — no grill password needed, which
  // is exactly why this is the right first call for bench validation.
  this->write_rpc_command_("{\"method\":\"RPC.Ping\",\"id\":1}");
}

void PitbossGrill::request_next_reply_chunk_() {
  auto status = esp_ble_gattc_read_char(this->get_gattc_if(), this->get_conn_id(), this->rpc_data_handle_,
                                        ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "Reading RPC reply chunk failed, status=%d", status);
    this->rpc_reply_in_progress_ = false;
  }
}

void PitbossGrill::on_rpc_notify_(const uint8_t *data, uint16_t len) {
  if (len < 4) {
    ESP_LOGW(TAG, "rx_ctl notification too short (%d bytes)", len);
    return;
  }
  uint32_t reply_len = (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | data[3];
  ESP_LOGD(TAG, "RPC reply announced: %u bytes", reply_len);
  this->rpc_reply_buffer_.clear();
  this->rpc_reply_expected_ = reply_len;
  this->rpc_reply_in_progress_ = reply_len > 0;
  if (this->rpc_reply_in_progress_) {
    this->request_next_reply_chunk_();
  }
}

void PitbossGrill::on_rpc_read_(const uint8_t *data, uint16_t len) {
  if (!this->rpc_reply_in_progress_)
    return;
  if (len == 0) {
    ESP_LOGW(TAG, "Abandoning truncated RPC reply: got %d of %u bytes",
             static_cast<int>(this->rpc_reply_buffer_.size()), this->rpc_reply_expected_);
    this->rpc_reply_in_progress_ = false;
    return;
  }
  this->rpc_reply_buffer_.insert(this->rpc_reply_buffer_.end(), data, data + len);
  if (this->rpc_reply_buffer_.size() >= this->rpc_reply_expected_) {
    this->rpc_reply_in_progress_ = false;
    std::string reply(this->rpc_reply_buffer_.begin(), this->rpc_reply_buffer_.end());
    // Phase 1 goal: prove a real reply comes back. Later phases parse this
    // as JSON instead of just logging it.
    ESP_LOGI(TAG, "RPC reply (%d bytes): %s", static_cast<int>(reply.size()), reply.c_str());
  } else {
    this->request_next_reply_chunk_();
  }
}

void PitbossGrill::on_debug_log_(const uint8_t *data, uint16_t len) {
  // Not parsed yet (that's the FE0B/FE0C status/temperature work for a
  // later phase) — logged so a real grill's push traffic is visible now.
  std::string text(reinterpret_cast<const char *>(data), len);
  ESP_LOGD(TAG, "Debug log: %s", text.c_str());
}

bool PitbossGrill::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  if (!BLEClientBase::gattc_event_handler(event, gattc_if, param))
    return false;

  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Connected to grill — discovering services...");
      }
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT: {
      ESP_LOGW(TAG, "Disconnected from grill");
      this->rpc_data_handle_ = this->rpc_tx_ctl_handle_ = this->rpc_rx_ctl_handle_ = this->debug_log_handle_ = 0;
      this->notifies_expected_ = this->notifies_confirmed_ = 0;
      this->rpc_reply_in_progress_ = false;
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      this->resolve_characteristics_();
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "register_for_notify failed for handle %d, status=%d", param->reg_for_notify.handle,
                 param->reg_for_notify.status);
        break;
      }
      this->notifies_confirmed_++;
      ESP_LOGD(TAG, "Notify registered for handle 0x%04x (%d/%d)", param->reg_for_notify.handle,
               this->notifies_confirmed_, this->notifies_expected_);
      if (this->notifies_confirmed_ >= this->notifies_expected_) {
        // node_state is BLEClientNode's concept (the two-tier ble_client
        // system this class deliberately bypasses) — set our own base
        // class's state instead, which is what connected() checks.
        this->set_state(espbt::ClientState::ESTABLISHED);
        ESP_LOGI(TAG, "Grill link established — sending RPC.Ping");
        this->send_ping_();
      }
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle == this->rpc_rx_ctl_handle_) {
        this->on_rpc_notify_(param->notify.value, param->notify.value_len);
      } else if (param->notify.handle == this->debug_log_handle_) {
        this->on_debug_log_(param->notify.value, param->notify.value_len);
      }
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.handle != this->rpc_data_handle_)
        break;
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Reading RPC reply chunk failed, status=%d", param->read.status);
        this->rpc_reply_in_progress_ = false;
        break;
      }
      this->on_rpc_read_(param->read.value, param->read.value_len);
      break;
    }
    default:
      break;
  }
  return true;
}

}  // namespace pitboss_grill
}  // namespace esphome

#endif
