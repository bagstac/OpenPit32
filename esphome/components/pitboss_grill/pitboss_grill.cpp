#include "pitboss_grill.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/wifi/wifi_component.h"

#include <esp_random.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>

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

// BLE's own wire format for a 128-bit UUID is byte-reversed relative to its
// human-readable/RFC4122 string form; Python's uuid.UUID(bytes=...) (what
// pytboss's _uuid() builds on) uses that string-order convention, and bleak
// does the reversal for you when it hands the UUID to the OS Bluetooth
// stack. ESPBTUUID::from_raw() does a plain memcpy with no such reversal.
// Confirmed on the bench: from_raw() left every characteristic handle at
// 0x0000 despite a real, established connection and completed service
// discovery; switching to from_raw_reversed() resolved all four handles
// and a full RPC.Ping round trip immediately followed.
static ESPBTUUID mongoose_uuid(const std::string &raw16) {
  return ESPBTUUID::from_raw_reversed(reinterpret_cast<const uint8_t *>(raw16.data()));
}

// Max bytes per GATT write chunk on the "data" characteristic — ports
// pytboss/ble.py's _encode_len()/_send_prepared_command(). See
// write_rpc_command_() for the full frame format and why chunks are paced.
static const size_t RPC_CHUNK_SIZE = 20;

// Auth codec — a direct port of pytboss/codec.py, itself a port of the PB
// firmware's own codec() / getCodecKey() (see docs/PROTOCOL.md's
// Authentication section). KEY is the firmware's fixed key for the grill
// password specifically (a second WIFI_KEY exists for PB.SetWifiCredentials
// only, not needed here).
static const uint8_t CODEC_KEY[8] = {0x8F, 0x80, 0x19, 0xCF, 0x77, 0x6C, 0xFE, 0xB7};
static const size_t CODEC_PADDING_LEN = 16;

// Port of timed_key(): derives the per-request key from the grill's own
// uptime, in 10s buckets. Repeatedly pops an element out of a shrinking
// copy of KEY at a position derived from the bucket number — a std::vector
// erase() mirrors Python list.pop(index) exactly, including the shrink.
static std::vector<uint8_t> timed_key(double uptime) {
  std::vector<uint8_t> key(CODEC_KEY, CODEC_KEY + sizeof(CODEC_KEY));
  std::vector<uint8_t> ret;
  uint32_t n = static_cast<uint32_t>(std::floor(std::max(uptime - 5.0, 0.0) / 10.0));
  while (key.size() > 1) {
    size_t idx = n % key.size();
    uint8_t v = key[idx];
    key.erase(key.begin() + idx);
    ret.push_back(static_cast<uint8_t>(v ^ (n & 0xFF)));
    n = (n * v + v) & 0xFF;
  }
  ret.push_back(key[0]);
  return ret;
}

// Port of encode(): 16 random padding bytes + an 0xFF marker + the real
// data, then XORed byte-by-byte against a key whose bytes get rewritten as
// it goes (this key does NOT shrink, unlike timed_key()'s — its length
// stays 8 throughout). Narrowing casts to uint8_t truncate exactly like
// Python's explicit "& 0xFF" would.
static std::vector<uint8_t> pb_encode(const std::string &data, const std::vector<uint8_t> &key_in) {
  std::vector<uint8_t> key = key_in;
  std::vector<uint8_t> buf(CODEC_PADDING_LEN);
  esp_fill_random(buf.data(), buf.size());
  buf.push_back(0xFF);
  buf.insert(buf.end(), data.begin(), data.end());

  std::vector<uint8_t> ret;
  ret.reserve(buf.size());
  for (size_t i = 0; i < buf.size(); i++) {
    uint8_t k = key[i % key.size()];
    uint8_t m = static_cast<uint8_t>(buf[i] ^ k);
    ret.push_back(m);
    size_t k2 = (i + 1) % key.size();
    key[k2] = static_cast<uint8_t>((key[k2] ^ m) + i);
  }
  return ret;
}

// Splits a hex string into its byte values, one per two hex chars — port of
// pytboss's parseHexMessage(). No validity checking: callers only ever pass
// sc_11/sc_12 or a debug-log push, both of which are either empty or a
// well-formed hex string the grill itself produced.
static std::vector<uint8_t> parse_hex_bytes(const std::string &hex) {
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
  }
  return out;
}

// Port of pytboss's convertTemperature(): three decimal digits packed one
// per byte, big-endian (e.g. bytes 0,7,1 -> 71). 960 is the grill's own
// "disconnected probe" sentinel (digits 9-6-0) — returned here as -1 rather
// than a magic number the caller has to know about.
static int16_t convert_temperature(const std::vector<uint8_t> &parts, size_t start) {
  int temp = parts[start] * 100 + parts[start + 1] * 10 + parts[start + 2];
  return temp == 960 ? -1 : static_cast<int16_t>(temp);
}

static std::string to_hex(const std::vector<uint8_t> &data) {
  static const char *const DIGITS = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (uint8_t b : data) {
    out.push_back(DIGITS[b >> 4]);
    out.push_back(DIGITS[b & 0x0F]);
  }
  return out;
}

// Builds {"id": id, "method": method, "params": {"psw": psw_hex}} — matching
// pytboss/transport.py's _prepare_command() exactly, including sending
// "params": {} rather than omitting the key when psw_hex is empty (used for
// PB.GetTime, which needs no auth).
static std::string build_rpc_request(int id, const std::string &method, const std::string &psw_hex) {
  return esphome::json::build_json([&](JsonObject root) {
    root["id"] = id;
    root["method"] = method;
    JsonObject params = root["params"].to<JsonObject>();
    if (!psw_hex.empty()) {
      params["psw"] = psw_hex;
    }
  });
}

void PitbossGrill::setup() {
  BLEClientBase::setup();
  this->set_auto_connect(true);

  // Phase 4: register /health, /state, /info on ESPHome's shared httpd
  // (web_server_base) — same pattern web_server/prometheus/captive_portal
  // use (base->init() is refcounted, safe to call alongside theirs).
  this->web_server_base_->init();
  this->web_server_base_->add_handler(this);

  // Periodic rather than one-shot, and via the global scheduler rather than
  // loop() (which BLEClientBase disables once state reaches IDLE, since
  // parse_device() dispatch doesn't need per-tick polling): a one-shot log
  // here reliably lands in the instant right after boot, before a network
  // log client has finished the WiFi/API handshake to see it. Useful
  // ongoing bench visibility, not just a one-time check.
  this->set_interval("status", 10000, [this]() {
    ESP_LOGD(TAG, "status: state=%d rpc_data=0x%04x rpc_tx_ctl=0x%04x rpc_rx_ctl=0x%04x "
                  "debug_log=0x%04x notifies=%d/%d gattc_calls=%u",
             static_cast<int>(this->state()), this->rpc_data_handle_, this->rpc_tx_ctl_handle_,
             this->rpc_rx_ctl_handle_, this->debug_log_handle_, this->notifies_confirmed_,
             this->notifies_expected_, this->gattc_call_count_);
  });

  // Phase 2 (docs/ESP32_FIRMWARE_PLAN.md): repeats the authenticated
  // PB.GetTime -> PB.GetState sequence on the real RPC write/notify/read
  // channel, superseding Phase 1's plain RPC.Ping as the ongoing bench
  // check — this proves the auth codec and a real state read, not just
  // that the transport is up. send_get_state_cycle_() no-ops if a reply is
  // already in flight, so this is safe to leave on a fixed interval.
  this->set_interval("get_state", 15000, [this]() {
    if (this->state() == espbt::ClientState::ESTABLISHED && this->notifies_confirmed_ >= this->notifies_expected_) {
      this->send_get_state_cycle_();
    }
  });
}

void PitbossGrill::loop() { BLEClientBase::loop(); }

void PitbossGrill::set_last_error_(const std::string &message) {
  LockGuard lock(this->state_mutex_);
  this->last_error_ = message;
}

void PitbossGrill::clear_last_error_() {
  LockGuard lock(this->state_mutex_);
  this->last_error_.clear();
}

void PitbossGrill::dump_config() {
  ESP_LOGCONFIG(TAG, "Pitboss Grill (Phase 4 — REST endpoints):");
  ESP_LOGCONFIG(TAG, "  Advertised-name prefix: %s", this->name_prefix_.c_str());
}

bool PitbossGrill::parse_device(const espbt::ESPBTDevice &device) {
  // Mirrors BLEClientBase::parse_device()'s contract (state/DISCOVERED
  // handling) but matches by advertised-name prefix instead of a fixed MAC:
  // the grill's BLE address is random and rotates between connections (see
  // docs/PROTOCOL.md), exactly why the sidecar's esphome_ble.find_grill()
  // does the same thing today.
  // One-shot diagnostic: parse_device() logging nothing at all (even for a
  // non-match) can't distinguish "never called" from "guard rejected it" —
  // this answers that unconditionally on the very first call, without
  // logging on every advertisement (the grill re-advertises every
  // ~20-30ms, and that volume of logging was enough on its own to
  // destabilize the WiFi/API connection during Phase 1 bring-up).
  static bool logged_first_parse_device = false;
  if (!logged_first_parse_device) {
    logged_first_parse_device = true;
    ESP_LOGD(TAG, "first parse_device() call: name='%s' state=%d", device.get_name().c_str(),
             static_cast<int>(this->state()));
  }

  if (this->state() != espbt::ClientState::IDLE)
    return false;
  const std::string &name = device.get_name();
  if (name.compare(0, this->name_prefix_.size(), this->name_prefix_) != 0)
    return false;

  ESP_LOGI(TAG, "Found grill '%s', rssi=%d", name.c_str(), device.get_rssi());
  this->set_state(espbt::ClientState::DISCOVERED);
  this->set_address(device.address_uint64());
  this->set_remote_addr_type(device.get_address_type());
  // The full advertised name (e.g. "PBV2-9451DC46B934") is this grill's
  // board id — same string the RPC replies' own "src" field carries, and
  // what /info reports as board_id. Advertising (and so this callback)
  // stops once connected, per the usual BLE behavior — see last_rssi_'s
  // comment in the header for why /health's rssi can go stale.
  {
    LockGuard lock(this->state_mutex_);
    this->board_id_ = name;
    this->last_rssi_ = device.get_rssi();
    this->has_rssi_ = true;
  }
  return true;
}

void PitbossGrill::resolve_characteristics_() {
  auto *data_chr = this->get_characteristic(mongoose_uuid(SERVICE_RPC), mongoose_uuid(CHAR_RPC_DATA));
  auto *tx_ctl_chr = this->get_characteristic(mongoose_uuid(SERVICE_RPC), mongoose_uuid(CHAR_RPC_TX_CTL));
  auto *rx_ctl_chr = this->get_characteristic(mongoose_uuid(SERVICE_RPC), mongoose_uuid(CHAR_RPC_RX_CTL));
  auto *debug_chr = this->get_characteristic(mongoose_uuid(SERVICE_DEBUG), mongoose_uuid(CHAR_DEBUG_LOG));

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

// A frame is: a 4-byte big-endian length write on "ctl", then the JSON body
// in <=20-byte pieces on "data". write_value() defaults to
// ESP_GATT_WRITE_TYPE_NO_RSP (write-without-response) — it returns as soon
// as the BT controller's internal buffer pool accepts the payload, not once
// the peer (or even the local radio) has actually sent it. Confirmed on the
// bench: firing all of a request's writes back-to-back with no pacing
// crashed the whole BT stack — reliably, regardless of payload content or
// which task called it — once a request needed more than ~4 total writes.
// RPC.Ping/PB.GetTime (~3-4 writes: this call + 2-3 chunks) never hit it;
// PB.GetState's longer body (~7 writes: this call + ~6 chunks) always did.
// So chunks are paced off each one's own ESP_GATTC_WRITE_CHAR_EVT (see
// on_rpc_write_complete_()) instead of being fired in a loop.
void PitbossGrill::write_rpc_command_(const std::string &json) {
  if (this->rpc_write_in_progress_) {
    ESP_LOGW(TAG, "Dropping RPC write — a previous one is still draining");
    return;
  }
  auto *tx_ctl_chr = this->get_characteristic(mongoose_uuid(SERVICE_RPC), mongoose_uuid(CHAR_RPC_TX_CTL));
  if (tx_ctl_chr == nullptr) {
    ESP_LOGE(TAG, "Cannot send RPC command — characteristics not resolved");
    return;
  }

  this->rpc_write_json_ = json;
  this->rpc_write_offset_ = 0;
  this->rpc_write_in_progress_ = true;

  uint8_t len_bytes[4] = {
      static_cast<uint8_t>((json.size() >> 24) & 0xFF),
      static_cast<uint8_t>((json.size() >> 16) & 0xFF),
      static_cast<uint8_t>((json.size() >> 8) & 0xFF),
      static_cast<uint8_t>(json.size() & 0xFF),
  };
  auto err = tx_ctl_chr->write_value(len_bytes, sizeof(len_bytes));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Writing RPC length failed, err=%d", err);
    this->rpc_write_in_progress_ = false;
    return;
  }
  // write_next_rpc_chunk_() continues once on_rpc_write_complete_() sees
  // this write's ESP_GATTC_WRITE_CHAR_EVT land on rpc_tx_ctl_handle_.
}

void PitbossGrill::write_next_rpc_chunk_() {
  if (this->rpc_write_offset_ >= this->rpc_write_json_.size()) {
    ESP_LOGD(TAG, "Sent RPC request (%d bytes): %s", static_cast<int>(this->rpc_write_json_.size()),
             this->rpc_write_json_.c_str());
    this->rpc_write_in_progress_ = false;
    this->rpc_write_json_.clear();
    return;
  }
  auto *data_chr = this->get_characteristic(mongoose_uuid(SERVICE_RPC), mongoose_uuid(CHAR_RPC_DATA));
  if (data_chr == nullptr) {
    ESP_LOGE(TAG, "Cannot continue RPC write — data characteristic not resolved");
    this->rpc_write_in_progress_ = false;
    return;
  }
  size_t chunk_len = std::min(RPC_CHUNK_SIZE, this->rpc_write_json_.size() - this->rpc_write_offset_);
  auto err = data_chr->write_value(
      reinterpret_cast<uint8_t *>(const_cast<char *>(this->rpc_write_json_.data() + this->rpc_write_offset_)),
      chunk_len);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Writing RPC chunk at offset %d failed, err=%d", static_cast<int>(this->rpc_write_offset_), err);
    this->rpc_write_in_progress_ = false;
    return;
  }
  this->rpc_write_offset_ += chunk_len;
}

void PitbossGrill::on_rpc_write_complete_(uint16_t handle, esp_gatt_status_t status) {
  if (!this->rpc_write_in_progress_)
    return;
  if (handle != this->rpc_tx_ctl_handle_ && handle != this->rpc_data_handle_)
    return;
  if (status != ESP_GATT_OK) {
    ESP_LOGW(TAG, "RPC write failed for handle 0x%04x, status=%d", handle, status);
    this->rpc_write_in_progress_ = false;
    return;
  }
  this->write_next_rpc_chunk_();
}

void PitbossGrill::send_ping_() {
  // Unauthenticated per docs/PROTOCOL.md — Phase 1's original bench check.
  // No longer on the periodic timer (Phase 2's GetTime->GetState cycle
  // supersedes it as the ongoing check) but kept callable.
  this->pending_reply_ = PendingReply::PING;
  this->write_rpc_command_("{\"method\":\"RPC.Ping\",\"id\":1}");
}

void PitbossGrill::send_get_state_cycle_() {
  if (this->pending_reply_ != PendingReply::NONE) {
    ESP_LOGD(TAG, "Skipping this GetState cycle — a reply is still in flight");
    return;
  }
  this->send_get_time_();
}

void PitbossGrill::send_get_time_() {
  // Unauthenticated (see docs/PROTOCOL.md) — just reads the grill's uptime,
  // which timed_key() needs to derive this request's own auth key.
  this->pending_reply_ = PendingReply::GET_TIME;
  this->write_rpc_command_(build_rpc_request(2, "PB.GetTime", ""));
}

void PitbossGrill::send_get_state_(double uptime) {
  auto key = timed_key(uptime);
  auto encoded = pb_encode(this->grill_password_, key);
  this->pending_reply_ = PendingReply::GET_STATE;
  this->write_rpc_command_(build_rpc_request(3, "PB.GetState", to_hex(encoded)));
}

void PitbossGrill::on_get_time_reply_(const std::string &json) {
  JsonDocument doc = esphome::json::parse_json(json);
  if (doc.isNull()) {
    ESP_LOGW(TAG, "PB.GetTime reply was not valid JSON: %s", json.c_str());
    this->set_last_error_("PB.GetTime reply was not valid JSON");
    return;
  }
  if (!doc["error"].isNull()) {
    ESP_LOGW(TAG, "PB.GetTime error: %s", json.c_str());
    this->set_last_error_("PB.GetTime error");
    return;
  }
  double uptime = doc["result"]["time"] | -1.0;
  if (uptime < 0) {
    ESP_LOGW(TAG, "PB.GetTime reply missing result.time: %s", json.c_str());
    this->set_last_error_("PB.GetTime reply missing result.time");
    return;
  }
  ESP_LOGD(TAG, "GetTime OK, uptime=%.1f s", uptime);
  this->send_get_state_(uptime);
}

void PitbossGrill::on_get_state_reply_(const std::string &json) {
  JsonDocument doc = esphome::json::parse_json(json);
  if (doc.isNull()) {
    ESP_LOGW(TAG, "PB.GetState reply was not valid JSON: %s", json.c_str());
    this->set_last_error_("PB.GetState reply was not valid JSON");
    return;
  }
  if (!doc["error"].isNull()) {
    // Per docs/PROTOCOL.md: a slow BLE write can land in the wrong 10s
    // uptime bucket and draw a spurious Unauthorized — not necessarily a
    // wrong password. The next 15s cycle re-derives the key from fresh
    // uptime, so one rejection here isn't treated as fatal (it does still
    // surface on /health's last_error until the next cycle clears it).
    ESP_LOGW(TAG, "PB.GetState rejected (bad password, or key-bucket skew — "
                  "next cycle re-derives the key): %s",
             json.c_str());
    this->set_last_error_("PB.GetState rejected (bad password, or key-bucket skew)");
    return;
  }
  this->clear_last_error_();
  // The reply's own top-level fields are "sc_11" (status, FE0B) and "sc_12"
  // (temperatures, FE0C) — the same two raw frames the grill also *pushes*
  // unauthenticated over the debug-log channel (see on_debug_log_()); both
  // sources are decoded the same way, by parse_status_frame_()/
  // parse_temperature_frame_() below. Either frame can legitimately be
  // blank here — the firmware clears both the instant it forwards a command
  // to the MCU and refills them from the next reply — so a blank one isn't
  // an error, just a poll that landed in that window.
  std::string sc_11 = doc["result"]["sc_11"] | "";
  std::string sc_12 = doc["result"]["sc_12"] | "";
  if (!sc_11.empty())
    this->parse_status_frame_(sc_11);
  if (!sc_12.empty())
    this->parse_temperature_frame_(sc_12);
  if (sc_11.empty() && sc_12.empty())
    ESP_LOGD(TAG, "PB.GetState OK, both frames blank (poll landed mid-command)");
}

// Decodes an FE0B status frame — ported from pytboss's grills.json "PBV2"
// control board (control_boards.PBV2.status_function; see grills.py for how
// pytboss itself evaluates it, through a JS interpreter). PBV2's own routine
// leaves the probe/chamber-temperature block (bytes 2-22) commented out —
// those all come from the FE0C temperature frame instead, decoded in
// parse_temperature_frame_() below — so this only ever fills in the
// on/off/error/state flags and the recipe timer.
void PitbossGrill::parse_status_frame_(const std::string &hex) {
  if (hex.compare(0, 4, "FE0B") != 0) {
    ESP_LOGW(TAG, "Status frame missing FE0B header: %s", hex.c_str());
    return;
  }
  auto parts = parse_hex_bytes(hex);
  if (parts.size() < 44) {
    ESP_LOGW(TAG, "Status frame too short (%d bytes, need 44): %s", static_cast<int>(parts.size()), hex.c_str());
    return;
  }
  LockGuard lock(this->state_mutex_);
  auto &s = this->grill_state_;
  s.has_status = true;
  this->last_frame_millis_ = millis();
  this->has_frame_millis_ = true;
  s.module_is_on = parts[24] == 1;
  s.err1 = parts[25] == 1;
  s.err2 = parts[26] == 1;
  s.err3 = parts[27] == 1;
  s.high_temp_err = parts[28] == 1;
  s.fan_err = parts[29] == 1;
  s.hot_err = parts[30] == 1;
  s.motor_err = parts[31] == 1;
  s.no_pellets = parts[32] == 1;
  s.er_l = parts[33] == 1;
  s.fan_state = parts[34] == 1;
  s.hot_state = parts[35] == 1;
  s.motor_state = parts[36] == 1;
  s.light_state = parts[37] == 1;
  s.prime_state = parts[38] == 1;
  s.recipe_step = parts[40];
  s.recipe_time_s = static_cast<uint32_t>(parts[41]) * 3600 + parts[42] * 60 + parts[43];
  ESP_LOGI(TAG, "Status: on=%d fan=%d igniter=%d auger=%d light=%d prime=%d no_pellets=%d "
                "errs(1/2/3/hi_temp/fan/hot/motor/erL)=%d/%d/%d/%d/%d/%d/%d/%d",
           s.module_is_on, s.fan_state, s.hot_state, s.motor_state, s.light_state, s.prime_state, s.no_pellets,
           s.err1, s.err2, s.err3, s.high_temp_err, s.fan_err, s.hot_err, s.motor_err, s.er_l);
}

// Decodes an FE0C temperature frame — ported from pytboss's grills.json
// "PBV2" control board (control_boards.PBV2.temperature_function). Values
// are converted to Celsius when the grill itself is set to Celsius,
// matching pytboss's own ftoc() step, so grill_state() always reports in
// whatever unit is_fahrenheit says it's in.
void PitbossGrill::parse_temperature_frame_(const std::string &hex) {
  if (hex.compare(0, 4, "FE0C") != 0) {
    ESP_LOGW(TAG, "Temperature frame missing FE0C header: %s", hex.c_str());
    return;
  }
  auto parts = parse_hex_bytes(hex);
  if (parts.size() < 27) {
    ESP_LOGW(TAG, "Temperature frame too short (%d bytes, need 27): %s", static_cast<int>(parts.size()), hex.c_str());
    return;
  }
  LockGuard lock(this->state_mutex_);
  auto &s = this->grill_state_;
  s.has_temperatures = true;
  this->last_frame_millis_ = millis();
  this->has_frame_millis_ = true;
  s.p1_temp = convert_temperature(parts, 5);
  s.p2_temp = convert_temperature(parts, 8);
  s.p3_temp = convert_temperature(parts, 11);
  s.p4_temp = convert_temperature(parts, 14);
  s.smoker_act_temp = convert_temperature(parts, 17);
  s.grill_set_temp = convert_temperature(parts, 20);
  s.grill_temp = convert_temperature(parts, 23);
  s.is_fahrenheit = parts[26] == 1;
  if (!s.is_fahrenheit) {
    // -1 is the "no reading" sentinel (see convert_temperature()), left
    // alone rather than run through the conversion below — pytboss's own
    // vendor JS has a bug here (it compares against 960 *after* that's
    // already been mapped to null, so the comparison never fires); pytboss
    // patches it at load time (grills.py's _FTOC_SENTINEL_RE) to skip null
    // too, which is what this -1 check reproduces.
    auto ftoc = [](int16_t t) { return t < 0 ? t : static_cast<int16_t>(std::floor((t - 32) / 1.8)); };
    s.p1_temp = ftoc(s.p1_temp);
    s.p2_temp = ftoc(s.p2_temp);
    s.p3_temp = ftoc(s.p3_temp);
    s.p4_temp = ftoc(s.p4_temp);
    s.smoker_act_temp = ftoc(s.smoker_act_temp);
    s.grill_set_temp = ftoc(s.grill_set_temp);
    s.grill_temp = ftoc(s.grill_temp);
  }
  ESP_LOGI(TAG, "Temps (%s): grill=%d/%d smoker=%d p1=%d p2=%d p3=%d p4=%d", s.is_fahrenheit ? "F" : "C", s.grill_temp,
           s.grill_set_temp, s.smoker_act_temp, s.p1_temp, s.p2_temp, s.p3_temp, s.p4_temp);
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
    // Also clear this, or send_get_state_cycle_()'s in-flight guard would
    // wrongly believe a reply is still pending forever after a truncation.
    this->pending_reply_ = PendingReply::NONE;
    return;
  }
  this->rpc_reply_buffer_.insert(this->rpc_reply_buffer_.end(), data, data + len);
  if (this->rpc_reply_buffer_.size() >= this->rpc_reply_expected_) {
    this->rpc_reply_in_progress_ = false;
    std::string reply(this->rpc_reply_buffer_.begin(), this->rpc_reply_buffer_.end());
    PendingReply kind = this->pending_reply_;
    this->pending_reply_ = PendingReply::NONE;
    // Deferred to the next main loop() tick rather than dispatched inline:
    // this callback runs on the BLE stack's own small dedicated task stack,
    // and on_get_state_reply_()/on_get_time_reply_() both call into
    // json::parse_json(), which allocates a JsonDocument on the caller's
    // stack (see json_util.h/.cpp). The crash actually root-caused on this
    // bench (2026-09-09) was a *write*-side issue — write_rpc_command_()
    // firing too many unpaced BLE writes, not this dispatch — but keeping
    // parsing off the BLE callback's stack costs nothing and stays cheap
    // insurance against a real stack-depth problem here later.
    this->defer([this, reply, kind]() {
      switch (kind) {
        case PendingReply::GET_TIME:
          this->on_get_time_reply_(reply);
          break;
        case PendingReply::GET_STATE:
          this->on_get_state_reply_(reply);
          break;
        case PendingReply::PING:
        case PendingReply::NONE:
        default:
          ESP_LOGI(TAG, "RPC reply (%d bytes): %s", static_cast<int>(reply.size()), reply.c_str());
          break;
      }
    });
  } else {
    this->request_next_reply_chunk_();
  }
}

void PitbossGrill::on_debug_log_(const uint8_t *data, uint16_t len) {
  std::string text(reinterpret_cast<const char *>(data), len);
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
    text.pop_back();
  // The grill also *pushes* its own status/temperature frames here
  // unprompted (docs/PROTOCOL.md section 2), as lines like "<==PB: FE0B...".
  // Decoded the same way as PB.GetState's sc_11/sc_12 (see
  // on_get_state_reply_()), so grill_state() stays current between the 15s
  // GetState polls instead of only updating once per cycle. Anything else
  // on this channel (Mongoose's own boot/debug chatter) is just logged.
  size_t frame_pos = text.find("FE0B");
  if (frame_pos != std::string::npos) {
    this->parse_status_frame_(text.substr(frame_pos));
    return;
  }
  frame_pos = text.find("FE0C");
  if (frame_pos != std::string::npos) {
    this->parse_temperature_frame_(text.substr(frame_pos));
    return;
  }
  ESP_LOGD(TAG, "Debug log: %s", text.c_str());
}

// -- REST endpoints (Phase 4) --
//
// Registered on ESPHome's shared httpd via web_server_base — see setup()
// and set_web_server_base(). Route matching follows the same canHandle()/
// handleRequest() idiom esphome/components/web_server and .../prometheus
// use (see web_server_base/web_server_idf.h): canHandle() only ever sees
// GET (the sidecar's /health, /state, /info are all read-only; write
// commands are Phase 5+), so handleRequest() doesn't need to re-check it.

bool PitbossGrill::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() != HTTP_GET)
    return false;
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  return url == "/health" || url == "/state" || url == "/info";
}

void PitbossGrill::handleRequest(AsyncWebServerRequest *request) {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  if (url == "/health") {
    this->handle_health_(request);
  } else if (url == "/state") {
    this->handle_state_(request);
  } else if (url == "/info") {
    this->handle_info_(request);
  }
}

// Mirrors grill_sidecar.py's health(): {configured, connected,
// proxy_connected, proxy_host, rssi, proxy_wifi_rssi, proxy_uptime_seconds,
// state_age_seconds, last_error} — see GrillRpcService.cs's
// SidecarHealthResponse for the exact shape the Blazor frontend expects.
// proxy_* used to describe a *second* ESP32 (the sidecar's BLE proxy hop);
// that hop is gone now — this firmware IS that ESP32 — so proxy_connected
// is unconditionally true and proxy_wifi_rssi/proxy_uptime_seconds map onto
// this device's own WiFi RSSI/uptime, which is what they always actually
// measured.
void PitbossGrill::handle_health_(AsyncWebServerRequest *request) {
  bool connected = this->state() == espbt::ClientState::ESTABLISHED;
  // Snapshot everything shared with the BT-stack/main-loop tasks up front,
  // under the lock, then build the JSON from local copies — see
  // state_mutex_'s header comment for why this needs a lock at all.
  bool has_rssi;
  int8_t rssi = 0;
  bool has_age;
  uint32_t frame_millis = 0;
  std::string last_error;
  {
    LockGuard lock(this->state_mutex_);
    has_rssi = this->has_rssi_;
    rssi = this->last_rssi_;
    has_age = this->has_frame_millis_;
    frame_millis = this->last_frame_millis_;
    last_error = this->last_error_;
  }
  std::string body = esphome::json::build_json([&](JsonObject root) {
    // Always true today: the grill password is compiled in (set_grill_
    // password()). Phase 7 makes this conditional once that becomes a
    // runtime /setup flow instead of a compile-time secret.
    root["configured"] = true;
    root["connected"] = connected;
    root["proxy_connected"] = true;
    if (has_rssi)
      root["rssi"] = rssi;
    if (esphome::wifi::global_wifi_component != nullptr)
      root["proxy_wifi_rssi"] = esphome::wifi::global_wifi_component->wifi_rssi();
    root["proxy_uptime_seconds"] = static_cast<int64_t>(millis() / 1000);
    if (has_age)
      root["state_age_seconds"] = (millis() - frame_millis) / 1000.0;
    if (!last_error.empty())
      root["last_error"] = last_error;
  });
  request->send(200, "application/json", body.c_str());
}

// Mirrors grill_sidecar.py's state(): {state, state_age_seconds, last_error}
// — see GrillRpcService.cs's SidecarStateResponse/SidecarState. "state" is
// null (omitted) until both the status and temperature frames have decoded
// at least once, same as bridge.state being None until the sidecar's first
// merged decode — a partial object (e.g. temperatures with default/zeroed
// status flags) would just be misleading. -1 temperatures (see
// convert_temperature()) come through as JSON null via add_temp() below,
// matching pytboss's own null-for-disconnected-probe convention.
void PitbossGrill::handle_state_(AsyncWebServerRequest *request) {
  // Snapshot under the lock (see state_mutex_'s header comment), then build
  // the JSON from the local copy — grill_state_ itself is written from both
  // the BT-stack task (debug-log pushes) and the main loop (deferred
  // GetState replies), neither of which is this httpd request's own task.
  GrillState s;
  bool has_age;
  uint32_t frame_millis = 0;
  std::string last_error;
  {
    LockGuard lock(this->state_mutex_);
    s = this->grill_state_;
    has_age = this->has_frame_millis_;
    frame_millis = this->last_frame_millis_;
    last_error = this->last_error_;
  }
  bool has_state = s.has_status && s.has_temperatures;
  std::string body = esphome::json::build_json([&](JsonObject root) {
    if (has_state) {
      JsonObject state = root["state"].to<JsonObject>();
      auto add_temp = [&](const char *key, int16_t t) {
        if (t >= 0)
          state[key] = t;
      };
      state["moduleIsOn"] = s.module_is_on;
      add_temp("grillTemp", s.grill_temp);
      add_temp("grillSetTemp", s.grill_set_temp);
      add_temp("smokerActTemp", s.smoker_act_temp);
      add_temp("p1Temp", s.p1_temp);
      add_temp("p2Temp", s.p2_temp);
      add_temp("p3Temp", s.p3_temp);
      add_temp("p4Temp", s.p4_temp);
      state["fanState"] = s.fan_state;
      state["hotState"] = s.hot_state;
      state["motorState"] = s.motor_state;
      state["lightState"] = s.light_state;
      state["primeState"] = s.prime_state;
      state["isFahrenheit"] = s.is_fahrenheit;
      state["noPellets"] = s.no_pellets;
      state["err1"] = s.err1;
      state["err2"] = s.err2;
      state["err3"] = s.err3;
      state["highTempErr"] = s.high_temp_err;
      state["fanErr"] = s.fan_err;
      state["hotErr"] = s.hot_err;
      state["motorErr"] = s.motor_err;
      state["erL"] = s.er_l;
      // Not part of GrillRpcService.cs's SidecarState yet — harmless extra
      // fields, ignored by System.Text.Json's default deserialization.
      state["recipeStep"] = s.recipe_step;
      state["recipeTimeSeconds"] = s.recipe_time_s;
    }
    if (has_age)
      root["state_age_seconds"] = (millis() - frame_millis) / 1000.0;
    if (!last_error.empty())
      root["last_error"] = last_error;
  });
  request->send(200, "application/json", body.c_str());
}

// Mirrors grill_sidecar.py's info(): {configured, board_id, model, firmware,
// accepted_setpoints_f, has_lights, meat_probes} — see GrillRpcService.cs's
// SidecarInfoResponse. Unlike the sidecar, this never 404s (configured is
// always true here — see handle_health_()). "firmware" (the sidecar's own
// one-time Pit Boss cloud fetch) and "accepted_setpoints_f" (Phase 5's
// set-temperature) aren't ported yet, so they're left out entirely;
// GrillDetail.razor's `info.firmware is { } fw` check already treats a
// missing/null value as "nothing to show". has_lights/meat_probes are
// hardcoded to this specific tested unit (see docs/ESP32_FIRMWARE_PLAN.md's
// "Decisions made" #6 and the command-surface section) — no light, 4 probes.
void PitbossGrill::handle_info_(AsyncWebServerRequest *request) {
  // board_id_ is written from parse_device(), on the BLE tracker's own task
  // — see state_mutex_'s header comment. model_ is set once in setup() from
  // compile-time config and never mutated again, so it's safe to read
  // unguarded, but there's no cost to being consistent here either.
  std::string board_id;
  {
    LockGuard lock(this->state_mutex_);
    board_id = this->board_id_;
  }
  std::string body = esphome::json::build_json([&](JsonObject root) {
    root["configured"] = true;
    if (!board_id.empty())
      root["board_id"] = board_id;
    if (!this->model_.empty())
      root["model"] = this->model_;
    root["accepted_setpoints_f"].to<JsonArray>();
    root["has_lights"] = false;
    root["meat_probes"] = 4;
  });
  request->send(200, "application/json", body.c_str());
}

bool PitbossGrill::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  this->gattc_call_count_++;  // liveness indicator, see the header comment
  bool accepted = BLEClientBase::gattc_event_handler(event, gattc_if, param);
  if (!accepted)
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
      this->set_last_error_("grill BLE link dropped — reconnecting");
      this->rpc_data_handle_ = this->rpc_tx_ctl_handle_ = this->rpc_rx_ctl_handle_ = this->debug_log_handle_ = 0;
      this->notifies_expected_ = this->notifies_confirmed_ = 0;
      this->rpc_reply_in_progress_ = false;
      this->rpc_write_in_progress_ = false;
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
        this->clear_last_error_();
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
    case ESP_GATTC_WRITE_CHAR_EVT: {
      this->on_rpc_write_complete_(param->write.handle, param->write.status);
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
