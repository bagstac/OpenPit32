#include "pitboss_grill.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/wifi/wifi_component.h"

#include <esp_http_server.h>
#include <esp_random.h>
#include <nvs.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

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

// Found live 2026-09-11: nothing previously bounded how long a sent RPC
// request (Ping/GetTime/GetState/MCU command) could sit waiting for a reply
// that never comes — a single dropped BLE notification (root cause never
// pinned down; the write itself always completed fine per the "Sent RPC
// request" log, so this is specifically the *reply* going missing, not the
// request) wedged pending_reply_ non-NONE forever, silently breaking every
// later GetState cycle and every POST /command with no way to recover short
// of a full disconnect/reconnect. loop() now force-clears a request stuck
// past this deadline — see rpc_request_millis_'s comment. Real round trips
// measured on this hardware are ~150-600ms even for GetState's longer body;
// 5s is generous headroom while still recovering well inside both the 15s
// periodic cycle and POST /command's own 8s semaphore budget.
static const uint32_t RPC_REPLY_TIMEOUT_MS = 5000;

// Auth codec — a direct port of pytboss/codec.py, itself a port of the PB
// firmware's own codec() / getCodecKey() (see docs/PROTOCOL.md's
// Authentication section). KEY is the firmware's fixed key for the grill
// password specifically (a second WIFI_KEY exists for PB.SetWifiCredentials
// only, not needed here).
static const uint8_t CODEC_KEY[8] = {0x8F, 0x80, 0x19, 0xCF, 0x77, 0x6C, 0xFE, 0xB7};
static const size_t CODEC_PADDING_LEN = 16;

// Root-caused live 2026-09-10: a fixed 15s GetState interval beats against
// the firmware's 10s auth-key bucket with a 30s period (their LCM), so
// depending on this boot's exact phase, the PB.GetTime read can land right
// at a bucket boundary every OTHER cycle — not "occasionally" (~1/40, the
// pre-existing/expected case docs/PROTOCOL.md and on_get_state_reply_()'s
// comment describe) but a full ~50% rejection rate for the life of that
// boot. checkPassword only accepts a key built from the firmware's own
// current bucket or the NEXT one — never the previous one (see
// pytboss/api.py's get_uptime(): "absorbs a client running ahead but
// rejects one running behind") — and this port had no bias at all: it used
// PB.GetTime's raw reply value straight through, which by the time the
// derived key actually reaches the grill (a further RPC round trip's worth
// of latency, ~150-400ms measured) has almost always slipped slightly
// *behind* the firmware's true clock, the one direction it never forgives.
// pytboss avoids this by timestamping *before* sending GetTime rather than
// after the reply arrives, so its own extrapolation always runs slightly
// ahead instead. This does the same job more simply: a fixed forward bias,
// comfortably above the round trip actually measured on this hardware and
// nowhere near the 10s bucket width (so it can only ever land on-or-ahead
// of the true bucket, never overshoot into a second one).
static const double AUTH_KEY_LATENCY_BIAS_S = 1.0;

// Phase 7 NVS layout (docs/ESP32_FIRMWARE_PLAN.md's "Decisions made" #7):
// one namespace, config values each under their own string key, the alarms
// list under one JSON-blob key rewritten wholesale on every change. Key/
// namespace names are well under NVS's 15-char limit.
static const char *const NVS_NAMESPACE = "pitboss_grill";
static const char *const NVS_KEY_GRILL_PW = "grill_pw";
static const char *const NVS_KEY_TG_TOKEN = "tg_token";
static const char *const NVS_KEY_TG_CHAT = "tg_chat_id";
static const char *const NVS_KEY_ALARMS = "alarms";

// Pit Boss cloud API (docs/PROTOCOL.md section 1) — a direct port of
// scripts/pitboss_cloud.py's BASE/_headers()/BOARD_PREFIX. Only ever hit
// once per POST /setup call, on demand — nothing here runs periodically.
static const char *const CLOUD_BASE = "https://api-prod.dansonscorp.com/api/v1";
static const char *const CLOUD_BOARD_PREFIX = "PBV2";

// Phase 5's fixed MCU commands — pytboss's grills.json "PBV2" control board
// entry, hardcoded per docs/PROTOCOL.md's "command surface" note (this
// project only ever talks to one grill model/board; a different one needs
// its own table). turn-on has no declared slug on ANY of pytboss's 137
// models — see turn_grill_on()'s docstring in pytboss/api.py for why FE0101FF
// is used anyway (proven working on real hardware, collides with no other
// command). turn-off is universal (FE0102FF, "turn-off" slug).
static const char *const CMD_TURN_ON = "FE0101FF";
static const char *const CMD_TURN_OFF = "FE0102FF";

// Grill setpoints the PBV2 board honours for this model (PBV5 P2) — pytboss's
// grills.json temp_increment, 130-420F in 5-degree steps. Fixed here for the
// same reason the commands above are: this project is pinned to one grill.
// A value outside this list is rejected by the board, so incoming
// set-temperature requests are snapped to the nearest entry here first,
// exactly like pytboss's own accepted_setpoints()/set_grill_temperature().
static const int16_t ACCEPTED_SETPOINTS_F[] = {
    130, 135, 140, 145, 150, 155, 160, 165, 170, 175, 180, 185, 190, 195, 200, 205, 210, 215, 220,
    225, 230, 235, 240, 245, 250, 255, 260, 265, 270, 275, 280, 285, 290, 295, 300, 305, 310, 315,
    320, 325, 330, 335, 340, 345, 350, 355, 360, 365, 370, 375, 380, 385, 390, 395, 400, 405, 410,
    415, 420,
};

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

// Builds {"id","method":"PB.SendMCUCommand","params":{"command","psw"}} —
// matches pytboss/api.py's _send_hex_command(): every MCU command carries
// the same encoded-grill-password "psw" PB.GetState does, just alongside a
// "command" field instead of alone.
static std::string build_mcu_command_request(int id, const std::string &command_hex, const std::string &psw_hex) {
  return esphome::json::build_json([&](JsonObject root) {
    root["id"] = id;
    root["method"] = "PB.SendMCUCommand";
    JsonObject params = root["params"].to<JsonObject>();
    params["command"] = command_hex;
    params["psw"] = psw_hex;
  });
}

static std::string to_trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

static std::string to_lower_trim(const std::string &s) {
  std::string out = to_trim(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
  return out;
}

// Phase 7: country codes only ("US" etc.) — pytboss_cloud.py's own
// `.strip().upper()` on the /setup request's "country" field.
static std::string to_upper_trim(const std::string &s) {
  std::string out = to_trim(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
  return out;
}

// Snaps `requested` (in the grill's current display unit) to the nearest
// setpoint ACCEPTED_SETPOINTS_F's board actually honours, then builds the
// "set-temperature" MCU command — a direct port of pytboss's grills.json
// PBV2 command function:
//   let temp = arguments[1] === false
//     ? Math.round(((arguments[0] * 1.8) + 32) / 5) * 5 : arguments[0];
//   ... 'FE0501' + formatHex(hundreds) + formatHex(tens) + formatHex(ones) + 'FF'
// The wire command always carries a Fahrenheit value (arguments[1] is only
// true/false to tell the board's own routine whether to convert first) — so
// this always resolves to a Fahrenheit snap, converting back if the grill is
// in Celsius, matching pytboss's set_grill_temperature()/accepted_setpoints().
// This grill declares no celsius_temp_increment of its own, so — like
// pytboss — the Celsius table here is derived from ACCEPTED_SETPOINTS_F via
// floor((f-32)/1.8), not a second hardcoded list.
static std::string build_set_temperature_command(double requested, bool fahrenheit) {
  int snapped_f;
  if (fahrenheit) {
    int best = ACCEPTED_SETPOINTS_F[0];
    double best_diff = std::fabs(requested - best);
    for (int16_t f : ACCEPTED_SETPOINTS_F) {
      double diff = std::fabs(requested - f);
      if (diff < best_diff) {
        best_diff = diff;
        best = f;
      }
    }
    snapped_f = best;
  } else {
    int best_c = static_cast<int>(std::floor((ACCEPTED_SETPOINTS_F[0] - 32) / 1.8));
    double best_diff = std::fabs(requested - best_c);
    for (int16_t f : ACCEPTED_SETPOINTS_F) {
      int c = static_cast<int>(std::floor((f - 32) / 1.8));
      double diff = std::fabs(requested - c);
      if (diff < best_diff) {
        best_diff = diff;
        best_c = c;
      }
    }
    snapped_f = static_cast<int>(std::lround((best_c * 1.8 + 32) / 5.0)) * 5;
  }
  uint8_t hundreds = (snapped_f / 100) % 10;
  uint8_t tens = (snapped_f / 10) % 10;
  uint8_t ones = snapped_f % 10;
  return "FE0501" + to_hex({hundreds, tens, ones}) + "FF";
}

// Reads a POST body directly off the raw httpd_req_t. Needed because the
// ESP-IDF web_server_idf backend only parses
// application/x-www-form-urlencoded bodies into arg()/getParam() (see
// AsyncWebServer::request_post_handler()) — a JSON body (what
// GrillRpcService.cs's PostAsJsonAsync sends) falls through to the plain
// GET-style handler with the body still unread on the socket, so it has to
// be pulled here instead. Capped well above any real /command payload;
// oversized or unreadable bodies fail rather than blocking on a partial read.
static bool read_json_body(AsyncWebServerRequest *request, std::string &out) {
  size_t len = request->contentLength();
  if (len == 0 || len > 512)
    return false;
  out.resize(len);
  httpd_req_t *raw = *request;
  size_t received = 0;
  while (received < len) {
    int r = httpd_req_recv(raw, &out[received], len - received);
    if (r <= 0)
      return false;
    received += static_cast<size_t>(r);
  }
  return true;
}

// -- Phase 7 (cloud password fetch) helpers --
//
// A direct port of scripts/pitboss_cloud.py's _headers()/_call() onto
// http_request::HttpRequestComponent — see docs/PROTOCOL.md section 1 for
// why each header is there.

static std::vector<http_request::Header> cloud_headers(const std::string &country, const std::string &token) {
  std::vector<http_request::Header> headers{
      {"Accept", "application/json"},
      {"Content-Type", "application/json"},
      {"X-Localization", "en"},
      {"x-country", country},
      {"x-store", "PB"},
      {"User-Agent", "OpenPit32-ESP32/1.0 (grill-password-fetch)"},
  };
  if (!token.empty())
    headers.push_back({"Authorization", "Bearer " + token});
  return headers;
}

// Reads an HTTP response body fully into a string, capped at max_len bytes
// — the same read()/is_read_complete() loop http_request.h's own
// "capture_response" action support uses internally (see that header's
// HttpRequestSendAction::play_complex()), via the http_read_fully() helper
// it ships for exactly this. Needed here (unlike notify_()'s Telegram POST,
// which never reads a reply body) because both cloud calls below need the
// JSON body, not just the status code. total_size is passed as the cap
// rather than content_length: http_read_fully() stops as soon as
// is_read_complete() says so regardless, and the cloud API's replies are
// small enough that 4KB is never actually reached in practice.
static bool read_http_body(http_request::HttpContainer *container, std::string &out, size_t max_len = 4096) {
  std::vector<uint8_t> buf(max_len);
  auto result = http_request::http_read_fully(container, buf.data(), max_len, 512, 8000);
  out.assign(reinterpret_cast<char *>(buf.data()), container->get_bytes_read());
  if (result.status != http_request::HttpReadStatus::OK) {
    ESP_LOGW(TAG, "Cloud response body read incomplete (%d bytes so far)", static_cast<int>(out.size()));
    return false;
  }
  return true;
}

// -- Phase 6 (alarms) helpers --
//
// Sensor keys/labels match scripts/alarms.py's SENSORS dict exactly — these
// are the same names GET /state's fields use (see handle_state_()), which
// is deliberate: a temp alarm's "sensor" is just one of those field names.

static const char *sensor_label(const std::string &sensor) {
  if (sensor == "grillTemp")
    return "Grill Temp";
  if (sensor == "smokerActTemp")
    return "Smoker Temp";
  if (sensor == "p1Temp")
    return "Probe 1";
  if (sensor == "p2Temp")
    return "Probe 2";
  if (sensor == "p3Temp")
    return "Probe 3";
  if (sensor == "p4Temp")
    return "Probe 4";
  return nullptr;  // unknown sensor
}

// -1 (see GrillState's comment) covers both "unknown sensor name" and "no
// reading yet" — check_alarms_() treats both the same way a missing dict key
// does in scripts/alarms.py's _check_once(): skip this alarm this cycle,
// don't fire, don't error.
static int16_t sensor_value(const PitbossGrill::GrillState &s, const std::string &sensor) {
  if (sensor == "grillTemp")
    return s.grill_temp;
  if (sensor == "smokerActTemp")
    return s.smoker_act_temp;
  if (sensor == "p1Temp")
    return s.p1_temp;
  if (sensor == "p2Temp")
    return s.p2_temp;
  if (sensor == "p3Temp")
    return s.p3_temp;
  if (sensor == "p4Temp")
    return s.p4_temp;
  return -1;
}

// A hex id the same shape as Python's uuid.uuid4().hex (32 lowercase hex
// chars) — collision odds from 16 random bytes are astronomically low at
// the handful-of-alarms scale this runs at, so no uniqueness check is done.
static std::string generate_alarm_id() {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  return to_hex(std::vector<uint8_t>(bytes, bytes + sizeof(bytes)));
}

// Fills one alarm's fields into an existing JsonObject — shared by
// handle_alarms_get_()'s array and handle_alarms_post_()'s single-alarm
// reply. sensor/comparison/target are omitted for a "timer" alarm and
// duration_seconds/fires_at for a "temp" one (rather than emitted as JSON
// null) — GrillRpcService.cs's AlarmDto fields are all nullable, so a
// missing key deserializes the same as an explicit null.
static void fill_alarm_json(JsonObject o, const PitbossGrill::Alarm &a) {
  o["id"] = a.id;
  o["kind"] = a.kind;
  o["label"] = a.label;
  if (a.kind == "temp") {
    o["sensor"] = a.sensor;
    o["comparison"] = a.comparison;
    o["target"] = a.target;
  } else {
    o["duration_seconds"] = a.duration_seconds;
    o["fires_at"] = a.fires_at;
  }
  o["created_at"] = a.created_at;
}

void PitbossGrill::setup() {
  BLEClientBase::setup();
  this->set_auto_connect(true);

  // Phase 7: before anything else can touch grill_password_/telegram_*_/
  // alarms_ — see this method's own comment for why no locking is needed
  // here specifically.
  this->load_persisted_state_();

  // Phase 5: the httpd task waiting inside handle_command_() is woken by
  // this once the main loop has a real result — see that method's comment.
  this->command_done_sem_ = xSemaphoreCreateBinary();

  // Phase 4: register /health, /state, /info (Phase 6: /alarms) on
  // ESPHome's shared httpd (web_server_base) — same pattern web_server/
  // prometheus/captive_portal use (base->init() is refcounted, safe to call
  // alongside theirs).
  this->web_server_base_->init();
  this->web_server_base_->add_handler(this);
  // DELETE /alarms/{id} can't go through add_handler() above — see
  // setup_delete_handler_()'s comment (and handle_alarms_delete_()'s
  // declaration in the header) for why. Safe to call now: init() just
  // called AsyncWebServer::begin(), which starts the httpd synchronously.
  this->setup_delete_handler_();

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

  // Phase 6: matches scripts/alarms.py's CHECK_SECONDS — needs no BLE
  // connection itself (check_alarms_() reads grill_state()'s last-known
  // values, whatever they are), same as the sidecar's monitor() running
  // "independent of bridge.configured".
  this->set_interval("alarm_check", 5000, [this]() { this->check_alarms_(); });
}

void PitbossGrill::loop() {
  BLEClientBase::loop();
  // Stuck-RPC watchdog — see RPC_REPLY_TIMEOUT_MS's and rpc_request_millis_'s
  // comments for why this exists at all: found live 2026-09-11, a sent
  // request whose reply just never arrives (no truncation notification
  // either — on_rpc_read_()'s len==0 path never fires) wedged
  // pending_reply_ permanently with nothing to recover it. millis() wraps
  // every ~49.7 days; a wrapped subtraction here briefly reads as a huge
  // elapsed time (always >= the timeout, never the reverse), so at worst
  // this fires one tick early right at the wrap instead of silently
  // failing to fire — the safe direction for a watchdog to be wrong in.
  if (this->pending_reply_ != PendingReply::NONE &&
      millis() - this->rpc_request_millis_ > RPC_REPLY_TIMEOUT_MS) {
    ESP_LOGW(TAG, "RPC reply timed out after %ums with no reply or truncation at all — recovering",
             static_cast<unsigned>(millis() - this->rpc_request_millis_));
    this->abandon_pending_rpc_("grill did not reply in time");
  }
}

// See the header comment. `kind` is captured before pending_reply_ is
// cleared so the pending_after_time_/command_mutex_ check below still knows
// what was actually in flight.
//
// A command's own request (kind GET_TIME or MCU_COMMAND, with
// pending_after_time_ still MCU_COMMAND) gets exactly one retry here before
// giving up — the same "retry once" idempotency guarantee
// on_mcu_command_reply_()'s comment already documents for an explicit
// rejection (every MCU command this project sends is idempotent, so
// retrying cannot double-apply anything) applies just as well to a request
// that got no reply at all. Confirmed live 2026-09-11: consecutive
// individual attempts routinely succeeded right after a failed one, so a
// same-request retry recovers real cases handle_command_()'s caller would
// otherwise have to notice and retry by hand.
void PitbossGrill::abandon_pending_rpc_(const char *reason) {
  PendingReply kind = this->pending_reply_;
  this->pending_reply_ = PendingReply::NONE;
  this->rpc_reply_in_progress_ = false;
  this->rpc_write_in_progress_ = false;
  // A truncation/timeout NOT tied to an in-flight command has nothing more
  // to do — the next *periodic* GetTime cycle just tries again in its own
  // time.
  if (this->pending_after_time_ != PendingReply::MCU_COMMAND)
    return;
  if ((kind == PendingReply::GET_TIME || kind == PendingReply::MCU_COMMAND) && !this->command_retried_) {
    ESP_LOGW(TAG, "%s — retrying once", reason);
    this->command_retried_ = true;
    this->send_get_time_();
    return;
  }
  // Either already retried once, or this wasn't actually the command's own
  // request (shouldn't happen — pending_reply_ during a command flow is
  // always GET_TIME or MCU_COMMAND — kept as a defensive fallback). Must
  // not leave pending_after_time_ pointing at MCU_COMMAND with a now-
  // abandoned command_pending_hex_, or the next *periodic* GetTime cycle
  // would reissue that stale command with no caller waiting on it. Waking
  // handle_command_() here (instead of leaving it to its own semaphore
  // timeout) also fails it fast.
  this->pending_after_time_ = PendingReply::GET_STATE;
  if (kind == PendingReply::GET_TIME || kind == PendingReply::MCU_COMMAND) {
    this->command_result_error_ = reason;
    xSemaphoreGive(this->command_done_sem_);
  }
}

void PitbossGrill::set_last_error_(const std::string &message) {
  LockGuard lock(this->state_mutex_);
  this->last_error_ = message;
}

void PitbossGrill::clear_last_error_() {
  LockGuard lock(this->state_mutex_);
  this->last_error_.clear();
}

// -- Phase 7: NVS persistence --
//
// Raw ESP-IDF NVS rather than ESPHome's own ESPPreferences (built for small
// fixed-size trivial structs — see esphome/components/esp32/preferences.h —
// not the variable-length strings/JSON this needs), one namespace, values
// as plain NVS strings (nvs_get_str()/nvs_set_str()) — see the key/
// namespace constants above. NVS itself is guaranteed initialized by now:
// esphome::esp32::ESP32Preferences calls nvs_flash_init() from app_main(),
// before the logger or any Component::setup() runs (see that component's
// preferences.cpp) — so nvs_open() below never needs an init step of its
// own, and a missing namespace (nothing ever saved yet) is just
// ESP_ERR_NVS_NOT_FOUND, not a real failure.
bool PitbossGrill::nvs_load_string_(const char *key, std::string &out) const {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    if (err != ESP_ERR_NVS_NOT_FOUND)
      ESP_LOGW(TAG, "nvs_open(readonly) failed: %s", esp_err_to_name(err));
    return false;
  }
  size_t required = 0;
  err = nvs_get_str(handle, key, nullptr, &required);
  if (err != ESP_OK || required == 0) {
    nvs_close(handle);
    return false;
  }
  std::vector<char> buf(required);
  err = nvs_get_str(handle, key, buf.data(), &required);
  nvs_close(handle);
  if (err != ESP_OK)
    return false;
  out.assign(buf.data());
  return true;
}

bool PitbossGrill::nvs_save_string_(const char *key, const std::string &value) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "nvs_open(readwrite) failed: %s", esp_err_to_name(err));
    return false;
  }
  err = nvs_set_str(handle, key, value.c_str());
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to persist NVS key '%s': %s", key, esp_err_to_name(err));
    return false;
  }
  return true;
}

// See the header comment — every REST handler (httpd task) that persists a
// plain string calls this instead of nvs_save_string_() directly, moving
// the actual flash write onto the main loop task's stack. `key` is always a
// static string literal (the NVS_KEY_* constants), so capturing the raw
// pointer is safe; `value` is captured by copy since the caller's local
// goes out of scope long before this runs. Fire-and-forget: nvs_save_string_
// () already logs its own warning on failure, so there's nothing further
// for a caller to check.
void PitbossGrill::nvs_save_string_deferred_(const char *key, const std::string &value) {
  this->defer([this, key, value]() { this->nvs_save_string_(key, value); });
}

// Runs once from setup(), before the BLE stack, httpd, or the alarm_check
// interval exist — nothing else can be touching grill_password_/
// telegram_*_/alarms_ concurrently yet, so this needs no locking of its own
// despite writing fields other code later guards with state_mutex_/
// alarms_mutex_.
void PitbossGrill::load_persisted_state_() {
  std::string saved;
  if (this->nvs_load_string_(NVS_KEY_GRILL_PW, saved)) {
    this->grill_password_ = saved;
    ESP_LOGI(TAG, "Loaded grill password from NVS (a prior POST /setup persisted it)");
  } else if (!this->grill_password_.empty()) {
    ESP_LOGI(TAG, "No NVS password yet — using the YAML-configured grill_password until POST /setup runs");
  }

  if (this->nvs_load_string_(NVS_KEY_TG_TOKEN, saved))
    this->telegram_bot_token_ = saved;
  if (this->nvs_load_string_(NVS_KEY_TG_CHAT, saved))
    this->telegram_chat_id_ = saved;

  std::string alarms_json;
  if (!this->nvs_load_string_(NVS_KEY_ALARMS, alarms_json))
    return;
  JsonDocument doc = esphome::json::parse_json(alarms_json);
  if (doc.isNull()) {
    ESP_LOGW(TAG, "Stored alarms JSON in NVS was corrupt — starting with no alarms");
    return;
  }
  for (JsonObject o : doc["alarms"].as<JsonArray>()) {
    Alarm a;
    a.id = std::string(o["id"] | "");
    a.kind = std::string(o["kind"] | "");
    a.label = std::string(o["label"] | "");
    a.sensor = std::string(o["sensor"] | "");
    a.comparison = std::string(o["comparison"] | "");
    a.target = o["target"] | 0.0;
    a.duration_seconds = o["duration_seconds"] | 0;
    a.fires_at = o["fires_at"] | 0.0;
    a.created_at = o["created_at"] | 0.0;
    if (!a.id.empty() && (a.kind == "temp" || a.kind == "timer"))
      this->alarms_.push_back(a);
  }
  ESP_LOGI(TAG, "Restored %d alarm(s) from NVS", static_cast<int>(this->alarms_.size()));
}

// Call with alarms_mutex_ already held — see its declaration in the header.
void PitbossGrill::save_alarms_locked_() {
  std::string body = esphome::json::build_json([&](JsonObject root) {
    JsonArray arr = root["alarms"].to<JsonArray>();
    for (const auto &a : this->alarms_)
      fill_alarm_json(arr.add<JsonObject>(), a);
  });
  if (!this->nvs_save_string_(NVS_KEY_ALARMS, body))
    ESP_LOGW(TAG, "Failed to persist alarms to NVS — they won't survive a reboot until the next successful save");
}

void PitbossGrill::dump_config() {
  ESP_LOGCONFIG(TAG, "Pitboss Grill (Phase 7 — POST /setup + NVS persistence):");
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
  // Every caller (send_ping_/send_get_time_/send_get_state_/
  // send_mcu_command_) already set pending_reply_ to a non-NONE value right
  // before calling this — timestamp it here, once, for loop()'s stuck-reply
  // watchdog (see rpc_request_millis_'s header comment) rather than at each
  // call site individually.
  this->rpc_request_millis_ = millis();

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
  // Phase 7: grill_password_ can now be rewritten at any time by POST
  // /setup, from the httpd task — see its own header comment — so this
  // main-loop read needs state_mutex_ too, unlike everything else here
  // (timed_key()/pb_encode() are pure functions, safe to run outside it).
  std::string password;
  {
    LockGuard lock(this->state_mutex_);
    password = this->grill_password_;
  }
  auto key = timed_key(uptime);
  auto encoded = pb_encode(password, key);
  this->pending_reply_ = PendingReply::GET_STATE;
  this->write_rpc_command_(build_rpc_request(3, "PB.GetState", to_hex(encoded)));
}

// Phase 5: same auth codec as send_get_state_ above (every authenticated RPC
// encodes the grill password the same way, regardless of method), but with
// "command" alongside "psw" and PB.SendMCUCommand instead of PB.GetState.
// command_pending_hex_ is set by handle_command_()'s defer() before this
// runs — see on_get_time_reply_() for how a GetTime cycle routes here
// instead of into the periodic send_get_state_().
void PitbossGrill::send_mcu_command_(double uptime) {
  // See send_get_state_()'s comment — same Phase 7 concern.
  std::string password;
  {
    LockGuard lock(this->state_mutex_);
    password = this->grill_password_;
  }
  auto key = timed_key(uptime);
  auto encoded = pb_encode(password, key);
  this->pending_reply_ = PendingReply::MCU_COMMAND;
  this->write_rpc_command_(build_mcu_command_request(4, this->command_pending_hex_, to_hex(encoded)));
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
  // See AUTH_KEY_LATENCY_BIAS_S's comment: bias the reading forward once,
  // here, so every key derived from it (send_get_state_()/
  // send_mcu_command_() below) is biased the direction the firmware
  // actually forgives, instead of computing an unbiased key and hoping
  // this cycle's latency happens to be small.
  double biased_uptime = uptime + AUTH_KEY_LATENCY_BIAS_S;
  // pending_after_time_ is NOT reset here — a retried MCU command (see
  // on_mcu_command_reply_()) calls send_get_time_() again and needs to land
  // back here a second time. Only on_mcu_command_reply_() itself resets it,
  // once it's actually done (success or final failure).
  if (this->pending_after_time_ == PendingReply::MCU_COMMAND) {
    this->send_mcu_command_(biased_uptime);
  } else {
    this->send_get_state_(biased_uptime);
  }
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
    // uptime, so one rejection here isn't treated as fatal. Confirmed live
    // 2026-09-10: ~1 in 40 cycles, always self-healing on the very next one
    // — surfacing that as last_error_ (and so /health's/GrillDetail.razor's
    // link-problem badge) on the first occurrence is a false alarm, so it's
    // only surfaced once get_state_reject_streak_ reaches
    // error_display_threshold_ consecutive rejections with no success in
    // between. The ESP_LOGW below stays unthrottled either way — this only
    // debounces what reaches the user-facing field.
    ESP_LOGW(TAG, "PB.GetState rejected (bad password, or key-bucket skew — "
                  "next cycle re-derives the key): %s",
             json.c_str());
    if (++this->get_state_reject_streak_ >= this->error_display_threshold_.load()) {
      this->set_last_error_("PB.GetState rejected (bad password, or key-bucket skew)");
    }
    return;
  }
  this->get_state_reject_streak_ = 0;
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

// Completes the Phase 5 command flow handle_command_() started: wakes the
// httpd task blocked on command_done_sem_ with a real result, one way or
// the other. A rejected command gets exactly one retry — like
// on_get_state_reply_()'s comment above, a slow write can land in the wrong
// 10s key bucket and draw a spurious Unauthorized that isn't a bad password
// at all, and every MCU command this project sends (on/off/absolute
// setpoint) is idempotent, so retrying cannot double-apply anything. Only
// resets pending_after_time_ once truly done — see on_get_time_reply_()'s
// comment for why the retry path must not reset it early.
void PitbossGrill::on_mcu_command_reply_(const std::string &json) {
  JsonDocument doc = esphome::json::parse_json(json);
  bool rejected = doc.isNull() || !doc["error"].isNull();
  if (rejected && !this->command_retried_) {
    ESP_LOGW(TAG, "MCU command rejected, retrying once (key-bucket skew, not necessarily a bad password): %s",
             json.c_str());
    this->command_retried_ = true;
    this->send_get_time_();
    return;
  }
  this->pending_after_time_ = PendingReply::GET_STATE;
  if (rejected) {
    ESP_LOGW(TAG, "MCU command failed after retry: %s", json.c_str());
    this->command_result_error_ = "grill rejected the command (bad password, or key-bucket skew)";
  } else {
    ESP_LOGI(TAG, "MCU command accepted: %s", this->command_pending_hex_.c_str());
    this->command_result_error_.clear();
  }
  xSemaphoreGive(this->command_done_sem_);
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
    this->abandon_pending_rpc_("BLE link dropped mid-command");
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
        case PendingReply::MCU_COMMAND:
          this->on_mcu_command_reply_(reply);
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
// use (see web_server_base/web_server_idf.h). GET /health, /state, /info,
// /alarms are read-only; POST /command (Phase 5), POST /config (the
// error_display_threshold setting), and POST /alarms (Phase 6) are the
// write routes. DELETE /alarms/{id} is NOT here — see
// setup_delete_handler_()'s comment for why that one bypasses this whole
// AsyncWebHandler mechanism.

bool PitbossGrill::canHandle(AsyncWebServerRequest *request) const {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  if (request->method() == HTTP_GET)
    return url == "/health" || url == "/state" || url == "/info" || url == "/alarms";
  if (request->method() == HTTP_POST)
    return url == "/command" || url == "/config" || url == "/alarms" || url == "/setup";
  return false;
}

void PitbossGrill::handleRequest(AsyncWebServerRequest *request) {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  bool is_post = request->method() == HTTP_POST;
  if (url == "/health") {
    this->handle_health_(request);
  } else if (url == "/state") {
    this->handle_state_(request);
  } else if (url == "/info") {
    this->handle_info_(request);
  } else if (url == "/command") {
    this->handle_command_(request);
  } else if (url == "/config") {
    this->handle_config_(request);
  } else if (url == "/setup") {
    this->handle_setup_(request);
  } else if (url == "/alarms") {
    if (is_post) {
      this->handle_alarms_post_(request);
    } else {
      this->handle_alarms_get_(request);
    }
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
  bool configured;
  {
    LockGuard lock(this->state_mutex_);
    has_rssi = this->has_rssi_;
    rssi = this->last_rssi_;
    has_age = this->has_frame_millis_;
    frame_millis = this->last_frame_millis_;
    last_error = this->last_error_;
    // Phase 7: real now — false until a grill password exists, from either
    // YAML or a successful POST /setup (see grill_password_'s header
    // comment). Matches GrillRpcService.cs's own doc comment on this field.
    configured = !this->grill_password_.empty();
  }
  std::string body = esphome::json::build_json([&](JsonObject root) {
    root["configured"] = configured;
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
// SidecarInfoResponse. Unlike the sidecar, this never 404s — configured is a
// real field here (Phase 7: false until a grill password exists), not a
// missing route. "firmware" (the sidecar's own one-time Pit Boss cloud
// fetch) isn't ported yet, so it's left out entirely; GrillDetail.razor's
// `info.firmware is { } fw` check already treats a missing/null value as
// "nothing to show". accepted_setpoints_f is
// ACCEPTED_SETPOINTS_F as-is (Phase 5) — always Fahrenheit regardless of the
// grill's current display unit, matching the sidecar's own
// `boss.accepted_setpoints(fahrenheit=True)`. has_lights_/meat_probes_ are
// set once in setup() from YAML config (see __init__.py) — no
// autodetection here either, same as the sidecar's pytboss-spec lookup.
// Bug found 2026-09-10, live on the real deployment: this used to hardcode
// meat_probes to 4 here; PBV5 P2 actually has 3 (pytboss's grills.json
// spec, and docs/PLAN.md's "Decisions made" #8), so the Live Status card
// was rendering a Probe 4 the grill doesn't have. Fixed by making both
// fields real config instead of literals baked into this handler.
void PitbossGrill::handle_info_(AsyncWebServerRequest *request) {
  // board_id_ is written from parse_device(), on the BLE tracker's own task
  // — see state_mutex_'s header comment. model_ is set once in setup() from
  // compile-time config and never mutated again, so it's safe to read
  // unguarded, but there's no cost to being consistent here either.
  // grill_password_/telegram_*_ are Phase 7 additions that ARE mutated at
  // runtime (POST /setup, POST /config) so do need the lock.
  std::string board_id;
  bool configured;
  bool telegram_configured;
  {
    LockGuard lock(this->state_mutex_);
    board_id = this->board_id_;
    configured = !this->grill_password_.empty();
    telegram_configured = !this->telegram_bot_token_.empty() && !this->telegram_chat_id_.empty();
  }
  std::string body = esphome::json::build_json([&](JsonObject root) {
    root["configured"] = configured;
    if (!board_id.empty())
      root["board_id"] = board_id;
    if (!this->model_.empty())
      root["model"] = this->model_;
    JsonArray setpoints = root["accepted_setpoints_f"].to<JsonArray>();
    for (int16_t f : ACCEPTED_SETPOINTS_F)
      setpoints.add(f);
    root["has_lights"] = this->has_lights_;
    root["meat_probes"] = this->meat_probes_;
    // Not part of the sidecar's /info shape — ESP32-only settings (see
    // handle_config_()) the web app reads here and writes via POST /config.
    root["error_display_threshold"] = this->error_display_threshold_.load();
    root["telegram_configured"] = telegram_configured;
  });
  request->send(200, "application/json", body.c_str());
}

// POST /config — {error_display_threshold: N} (the number of consecutive
// PB.GetState rejections, see on_get_state_reply_(), required before one is
// surfaced as last_error_/health's/state's last_error field, added
// 2026-09-10 so a single expected, self-healing 401 — key-bucket skew, see
// docs/PROTOCOL.md — doesn't flash a false-alarm warning in
// GrillDetail.razor) and (Phase 7) {telegram_bot_token, telegram_chat_id} —
// the same runtime-settable-without-a-reflash pattern, now persisted to NVS
// rather than in-memory only, so a future web-app "notifications" flow
// (today: secrets.yaml only, see grill-firmware.yaml's comment) can enter
// these without a reflash, the same way POST /setup enters the grill
// password. Unlike handle_command_(), none of this touches BLE, so the
// in-memory update and the HTTP reply are a plain synchronous read-
// validate-write-respond with no semaphore — error_display_threshold_ being
// std::atomic and telegram_*_ needing state_mutex_ are the cross-task
// concerns there (this write, from the httpd task, races
// on_get_state_reply_()'s/notify_()'s reads on the main loop). The actual
// NVS flash write is NOT synchronous, though — see
// nvs_save_string_deferred_()'s comment: doing it inline here
// stack-overflowed the httpd task on the bench, so it's deferred to the
// main loop instead and the reply doesn't wait on it.
void PitbossGrill::handle_config_(AsyncWebServerRequest *request) {
  std::string body;
  if (!read_json_body(request, body)) {
    request->send(200, "application/json", "{\"ok\":false,\"error\":\"missing or oversized JSON body\"}");
    return;
  }
  JsonDocument doc = esphome::json::parse_json(body);
  if (doc.isNull()) {
    request->send(200, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }
  if (!doc["error_display_threshold"].isNull()) {
    int requested = doc["error_display_threshold"] | -1;
    // 1-60: 0 would surface every single rejection immediately (defeating
    // the point), and 60 is already 15 minutes of nothing-but-failures at
    // the periodic cycle's 15s cadence — well past "this needs a human".
    if (requested < 1 || requested > 60) {
      request->send(
          200, "application/json",
          "{\"ok\":false,\"error\":\"error_display_threshold must be between 1 and 60\"}");
      return;
    }
    this->error_display_threshold_.store(static_cast<uint8_t>(requested));
  }
  // Empty string is a valid, intentional value for either of these — it's
  // exactly notify_()'s "no bot configured" no-op case, so this also doubles
  // as how to turn Telegram delivery back off without a reflash.
  if (!doc["telegram_bot_token"].isNull()) {
    std::string token = doc["telegram_bot_token"] | "";
    this->nvs_save_string_deferred_(NVS_KEY_TG_TOKEN, token);
    LockGuard lock(this->state_mutex_);
    this->telegram_bot_token_ = token;
  }
  if (!doc["telegram_chat_id"].isNull()) {
    std::string chat_id = doc["telegram_chat_id"] | "";
    this->nvs_save_string_deferred_(NVS_KEY_TG_CHAT, chat_id);
    LockGuard lock(this->state_mutex_);
    this->telegram_chat_id_ = chat_id;
  }
  std::string resp = esphome::json::build_json([&](JsonObject root) {
    root["ok"] = true;
    root["error_display_threshold"] = this->error_display_threshold_.load();
  });
  request->send(200, "application/json", resp.c_str());
}

// Mirrors grill_sidecar.py's POST /command: body {action, value?, confirm?}
// (no "probe" — set_probe/probe targets are decision 6 in
// docs/ESP32_FIRMWARE_PLAN.md, not being ported), reply
// {ok, error?, action} — see GrillRpcService.cs's SendCommandAsync/
// SidecarCommandResponse for the exact shape the Blazor frontend expects.
// Only power_on/power_off/set_temp exist for this grill: light_on/light_off/
// prime_on/prime_off/set_probe are real sidecar actions, but this grill has
// no light (has_lights: false) and PBV2 declares no primer-motor slug (see
// docs/PROTOCOL.md's "command surface" note) — reported as "unsupported on
// this grill" rather than the sidecar's silent light no-op, since nothing in
// the UI can reach them for this grill anyway.
//
// power_on/power_off require confirm: true, exactly like
// grill_sidecar.py's bridge.command() — this is the safety gate
// docs/ESP32_FIRMWARE_PLAN.md's "Risks" section calls out by name
// ("keep the confirm-before-power-on UX exactly as it is today").
//
// Every reply here is HTTP 200, success or not — confirmed live
// (2026-09-10) that web_server_idf's AsyncWebServerRequest::init_response_()
// only special-cases 200/404/409 and maps anything else, 400 included, to a
// misleading 500. GrillRpcService.cs's SendCommandAsync never checks the
// status code anyway (it deserializes the body and reads "ok"/"error"
// unconditionally), so status-coding these responses correctly isn't
// possible on this ESP-IDF backend and isn't needed by the one real caller.
//
// The BLE round trip is asynchronous (GetTime -> encode -> write -> wait for
// the grill's ack, all on the main loop task/BT stack, same as every other
// RPC in this file) but this handler answers synchronously, the same way
// the sidecar's bridge.command() is awaited to completion before its caller
// gets a reply — a caller acting on {"ok": true} needs that to mean the
// grill actually took the command, not just that it was queued. That means
// blocking this httpd-task call: command_mutex_ serializes concurrent
// POST /command calls (only one physical BLE link exists to drive), then
// this defer()s the actual RPC kickoff onto the main loop — pending_reply_/
// pending_after_time_/command_pending_hex_ are main-loop-only fields, same
// as the rest of this file's RPC state, so the httpd task must never touch
// them directly — and blocks on command_done_sem_, which
// on_mcu_command_reply_() (or a disconnect/truncation abandoning the
// command — see gattc_event_handler()/on_rpc_read_()) gives once there's a
// real result. Known trade-off: ESP-IDF's httpd processes one request at a
// time (unlike the sidecar's asyncio server, which yields between awaits),
// so a command in flight also stalls this ESP32's GET /health, /state,
// /info for the same few seconds. Acceptable for a user-initiated,
// infrequent action; not something a polling loop should ever trigger.
void PitbossGrill::handle_command_(AsyncWebServerRequest *request) {
  std::string body;
  if (!read_json_body(request, body)) {
    request->send(200, "application/json", "{\"ok\":false,\"error\":\"missing or oversized JSON body\"}");
    return;
  }
  JsonDocument doc = esphome::json::parse_json(body);
  if (doc.isNull()) {
    request->send(200, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }
  std::string action = to_lower_trim(doc["action"] | "");
  bool confirm = doc["confirm"] | false;
  bool has_value = !doc["value"].isNull();
  double value = doc["value"] | 0.0;

  std::string error;
  std::string command_hex;
  if (action == "power_on") {
    if (!confirm)
      error = "'power_on' requires confirm: true";
    else
      command_hex = CMD_TURN_ON;
  } else if (action == "power_off") {
    if (!confirm)
      error = "'power_off' requires confirm: true";
    else
      command_hex = CMD_TURN_OFF;
  } else if (action == "set_temp") {
    if (!has_value) {
      error = "set_temp needs a value";
    } else {
      bool fahrenheit;
      {
        LockGuard lock(this->state_mutex_);
        fahrenheit = this->grill_state_.is_fahrenheit;
      }
      command_hex = build_set_temperature_command(value, fahrenheit);
    }
  } else if (action.empty()) {
    error = "missing action";
  } else {
    error = "unsupported on this grill: " + action;
  }

  if (error.empty() && this->state() != espbt::ClientState::ESTABLISHED)
    error = "grill not connected";

  if (!error.empty()) {
    std::string resp = esphome::json::build_json([&](JsonObject root) {
      root["ok"] = false;
      root["error"] = error;
    });
    request->send(200, "application/json", resp.c_str());
    return;
  }

  // One command at a time on the single physical BLE link.
  LockGuard cmd_lock(this->command_mutex_);
  xSemaphoreTake(this->command_done_sem_, 0);  // drain a stale signal left by a prior timeout
  this->command_result_error_.clear();
  this->command_retried_ = false;

  this->defer([this, command_hex]() {
    if (this->pending_reply_ != PendingReply::NONE) {
      // The periodic 15s GetState cycle (or, in principle, another command)
      // was already mid-flight when this one was deferred in — real enough
      // in practice (not the "vanishingly unlikely" case this comment used
      // to claim) that loop()'s RPC_REPLY_TIMEOUT_MS watchdog exists
      // specifically so this self-clears within a few seconds rather than
      // wedging forever; fail loudly here rather than stomp on whatever's
      // actually in flight.
      this->command_result_error_ = "grill busy — try again";
      xSemaphoreGive(this->command_done_sem_);
      return;
    }
    this->command_pending_hex_ = command_hex;
    this->pending_after_time_ = PendingReply::MCU_COMMAND;
    this->send_get_time_();
  });

  // 12s, not 8s: abandon_pending_rpc_()'s retry-once-on-no-reply can now
  // burn up to ~2x RPC_REPLY_TIMEOUT_MS (5s each) before this semaphore
  // would ever see it, on top of whatever the retry's own successful round
  // trip then takes — 8s wasn't enough headroom for that whole sequence to
  // land within it, which just traded a real command failure for a
  // misleading "timed out waiting for grill" even on a run that was about
  // to succeed via its retry.
  bool completed = xSemaphoreTake(this->command_done_sem_, pdMS_TO_TICKS(12000)) == pdTRUE;
  std::string result_error = completed ? this->command_result_error_ : std::string("timed out waiting for grill");

  std::string resp = esphome::json::build_json([&](JsonObject root) {
    root["ok"] = result_error.empty();
    if (!result_error.empty())
      root["error"] = result_error;
    root["action"] = action;
  });
  request->send(200, "application/json", resp.c_str());
}

// -- Phase 6: alarms --
//
// Mirrors scripts/grill_sidecar.py's GET/POST /alarms and DELETE
// /alarms/{id} — see fill_alarm_json()'s comment for the field shape, and
// setup_delete_handler_()'s comment for why DELETE is wired up completely
// differently from the other four routes in this file.

void PitbossGrill::handle_alarms_get_(AsyncWebServerRequest *request) {
  std::vector<Alarm> snapshot;
  {
    LockGuard lock(this->alarms_mutex_);
    snapshot = this->alarms_;
  }
  std::string body = esphome::json::build_json([&](JsonObject root) {
    JsonArray arr = root["alarms"].to<JsonArray>();
    for (const auto &a : snapshot)
      fill_alarm_json(arr.add<JsonObject>(), a);
  });
  request->send(200, "application/json", body.c_str());
}

// Every reply here is HTTP 200 regardless of ok/error, for the same reason
// handle_command_()'s comment gives: this ESP-IDF web server backend can't
// answer 400 correctly (it silently becomes 500 — see that comment for the
// live-confirmed detail), and GrillRpcService.cs never checks the status
// code for command/config-shaped replies anyway. AddTempAlarmAsync/
// AddTimerAlarmAsync (GrillRpcService.cs) only look at the body's `ok`.
void PitbossGrill::handle_alarms_post_(AsyncWebServerRequest *request) {
  std::string body;
  if (!read_json_body(request, body)) {
    request->send(200, "application/json", "{\"ok\":false,\"error\":\"missing or oversized JSON body\"}");
    return;
  }
  JsonDocument doc = esphome::json::parse_json(body);
  if (doc.isNull()) {
    request->send(200, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }
  std::string kind = to_lower_trim(doc["kind"] | "");
  std::string label = doc["label"] | "";

  Alarm alarm;
  std::string error;
  if (kind == "temp") {
    std::string sensor = doc["sensor"] | "";
    std::string comparison = doc["comparison"] | "";
    const char *default_label = sensor_label(sensor);
    if (default_label == nullptr) {
      error = "unknown sensor '" + sensor + "'";
    } else if (comparison != "at_or_above" && comparison != "at_or_below") {
      error = "unknown comparison '" + comparison + "'";
    } else if (doc["target"].isNull()) {
      error = "temp alarm needs a target";
    } else {
      alarm.kind = "temp";
      alarm.sensor = sensor;
      alarm.comparison = comparison;
      alarm.target = doc["target"] | 0.0;
      alarm.label = label.empty() ? default_label : label;
    }
  } else if (kind == "timer") {
    int duration = doc["duration_seconds"] | 0;
    if (duration <= 0) {
      error = "duration must be positive";
    } else {
      alarm.kind = "timer";
      alarm.duration_seconds = duration;
      alarm.label = label.empty() ? "Timer" : label;
    }
  } else {
    error = "unknown alarm kind '" + kind + "'";
  }

  // created_at/fires_at have to be real Unix seconds (see set_time()'s
  // comment) — reject rather than silently create an alarm whose timestamp
  // GrillDetail.razor's DescribeAlarm() would render as nonsense (e.g. a
  // multi-decade "remaining" countdown) if NTP hasn't synced yet. In
  // practice this only matters in the first few seconds after boot.
  if (error.empty() && (this->time_ == nullptr || !this->time_->now().is_valid())) {
    error = "grill clock not synced yet — try again in a moment";
  }

  if (!error.empty()) {
    std::string resp = esphome::json::build_json([&](JsonObject root) {
      root["ok"] = false;
      root["error"] = error;
    });
    request->send(200, "application/json", resp.c_str());
    return;
  }

  double now = static_cast<double>(this->time_->timestamp_now());
  alarm.id = generate_alarm_id();
  alarm.created_at = now;
  if (alarm.kind == "timer")
    alarm.fires_at = now + alarm.duration_seconds;

  {
    LockGuard lock(this->alarms_mutex_);
    this->alarms_.push_back(alarm);
  }
  // Phase 7: rewrite the NVS blob to match — deferred to the main loop task
  // (see nvs_save_string_deferred_()'s/save_alarms_locked_()'s comments;
  // calling it inline here, on the httpd task, stack-overflowed the device
  // on the bench). The deferred lambda takes its own fresh lock rather than
  // reusing the one above, which is already released by the time this runs.
  this->defer([this]() {
    LockGuard lock(this->alarms_mutex_);
    this->save_alarms_locked_();
  });

  std::string resp = esphome::json::build_json([&](JsonObject root) {
    root["ok"] = true;
    fill_alarm_json(root["alarm"].to<JsonObject>(), alarm);
  });
  request->send(200, "application/json", resp.c_str());
}

// DELETE /alarms/{id} — see this method's declaration in the header for why
// it exists at all instead of going through canHandle()/handleRequest():
// web_server_idf's AsyncWebServer only ever registers HTTP_GET/HTTP_POST/
// HTTP_OPTIONS with esp_http_server (AsyncWebServer::begin(), web_server_
// idf.cpp) — there is no HTTP_DELETE code path in that framework at all.
// Registering our own handler directly on the same httpd_handle_t is legal
// ESP-IDF (multiple handlers, different methods, coexist fine) but means
// operating on the raw httpd_req_t rather than AsyncWebServerRequest, whose
// constructor only AsyncWebServer itself may call.
void PitbossGrill::setup_delete_handler_() {
  httpd_handle_t server = this->web_server_base_->get_server()->get_server();
  if (server == nullptr) {
    ESP_LOGE(TAG, "Cannot register DELETE /alarms/{id} — httpd not started");
    return;
  }
  httpd_uri_t handler_delete = {};
  handler_delete.uri = "";  // matches every path, same as the GET/POST/OPTIONS
                            // handlers AsyncWebServer::begin() already registered
                            // (the server's uri_match_fn, set once at httpd_start(),
                            // treats "" as "match anything" for every method)
  handler_delete.method = HTTP_DELETE;
  handler_delete.handler = &PitbossGrill::delete_alarm_trampoline_;
  handler_delete.user_ctx = this;
  esp_err_t err = httpd_register_uri_handler(server, &handler_delete);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register DELETE handler, err=%d", err);
  }
}

esp_err_t PitbossGrill::delete_alarm_trampoline_(httpd_req_t *req) {
  auto *self = static_cast<PitbossGrill *>(req->user_ctx);
  return self->handle_alarms_delete_(req);
}

esp_err_t PitbossGrill::handle_alarms_delete_(httpd_req_t *req) {
  std::string uri(req->uri);
  size_t query_pos = uri.find('?');
  if (query_pos != std::string::npos)
    uri.resize(query_pos);

  static const std::string PREFIX = "/alarms/";
  bool removed = false;
  if (uri.compare(0, PREFIX.size(), PREFIX) == 0 && uri.size() > PREFIX.size()) {
    std::string id = uri.substr(PREFIX.size());
    LockGuard lock(this->alarms_mutex_);
    for (auto it = this->alarms_.begin(); it != this->alarms_.end(); ++it) {
      if (it->id == id) {
        this->alarms_.erase(it);
        removed = true;
        break;
      }
    }
  }
  if (removed) {
    // Phase 7 — see handle_alarms_post_()'s comment: deferred to the main
    // loop task, same stack-overflow reason (this handler runs on the same
    // httpd worker task pool as the AsyncWebServerRequest-based ones above,
    // just via a raw esp_http_server handler instead).
    this->defer([this]() {
      LockGuard lock(this->alarms_mutex_);
      this->save_alarms_locked_();
    });
  }

  std::string body = esphome::json::build_json([&](JsonObject root) { root["ok"] = removed; });
  httpd_resp_set_type(req, "application/json");
  // Unlike the AsyncWebServerRequest-based handlers above, this talks to
  // esp_http_server directly, so a real 200/404 (matching the sidecar's
  // `status=200 if removed else 404`) works here with none of
  // init_response_()'s 400->500 limitation.
  httpd_resp_set_status(req, removed ? "200 OK" : "404 Not Found");
  httpd_resp_send(req, body.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// Evaluates every alarm against the latest grill_state()/real time, fires
// (notify_()) and drops any that are due — see scripts/alarms.py's
// _check_once() for the semantics this ports. Runs on the "alarm_check"
// interval (setup()), independent of the BLE link's state, same as the
// sidecar's monitor() running "independent of bridge.configured": a timer
// alarm needs only the clock.
void PitbossGrill::check_alarms_() {
  if (this->time_ == nullptr || !this->time_->now().is_valid())
    return;  // nothing meaningful to compare fires_at/target against yet
  double now = static_cast<double>(this->time_->timestamp_now());

  GrillState state;
  {
    LockGuard lock(this->state_mutex_);
    state = this->grill_state_;
  }

  // Collected while alarms_mutex_ is held, sent after it's released — a
  // Telegram POST can take seconds (see notify_()), and nothing else here
  // needs alarms_mutex_ held that long. Same snapshot-then-act pattern
  // state_mutex_'s consumers already use.
  std::vector<std::string> fired_messages;
  bool any_fired = false;
  {
    LockGuard lock(this->alarms_mutex_);
    for (auto it = this->alarms_.begin(); it != this->alarms_.end();) {
      bool hit = false;
      std::string message;
      if (it->kind == "timer") {
        if (now >= it->fires_at) {
          hit = true;
          message = "Timer done: " + it->label + " finished.";
        }
      } else {
        int16_t value = sensor_value(state, it->sensor);
        if (value >= 0) {
          bool cmp_hit = it->comparison == "at_or_above" ? (value >= it->target) : (value <= it->target);
          if (cmp_hit) {
            hit = true;
            message = "Temperature alarm: " + it->label + " reached " + std::to_string(value) + "\xC2\xB0 (target " +
                      std::to_string(static_cast<int>(std::lround(it->target))) + "\xC2\xB0).";
          }
        }
      }
      if (hit) {
        fired_messages.push_back(message);
        it = this->alarms_.erase(it);
        any_fired = true;
      } else {
        ++it;
      }
    }
    // Phase 7 — see handle_alarms_post_()'s comment. Only on an actual
    // change: this interval runs every 5s regardless of whether anything
    // fired, and rewriting the same NVS value repeatedly buys nothing.
    if (any_fired)
      this->save_alarms_locked_();
  }
  for (const auto &message : fired_messages) {
    ESP_LOGI(TAG, "Alarm fired: %s", message.c_str());
    this->notify_(message);
  }
}

// One HTTPS POST to Telegram's Bot API — see set_telegram_bot_token()'s
// comment for the no-bot-configured no-op, and
// docs/ESP32_FIRMWARE_PLAN.md's "Telegram integration" section for why
// Telegram specifically (a placeholder, deliberately isolated to this one
// function so swapping providers later doesn't touch check_alarms_() at
// all). http_request_->post() blocks the calling task (the main loop, via
// check_alarms_()) for the round trip — acceptable here since alarm checks
// are a background 5s tick, not something latency-sensitive is waiting on.
void PitbossGrill::notify_(const std::string &message) {
  // Phase 7: both are now runtime-settable via POST /config (handle_config_
  // ()), from the httpd task — so this main-loop read needs state_mutex_
  // too, same reasoning as send_get_state_()'s grill_password_ read.
  std::string bot_token, chat_id;
  {
    LockGuard lock(this->state_mutex_);
    bot_token = this->telegram_bot_token_;
    chat_id = this->telegram_chat_id_;
  }
  if (bot_token.empty() || chat_id.empty()) {
    ESP_LOGI(TAG, "Alarm fired but no Telegram bot configured: %s", message.c_str());
    return;
  }
  if (this->http_request_ == nullptr) {
    ESP_LOGW(TAG, "Cannot notify — http_request component missing");
    return;
  }
  std::string url = "https://api.telegram.org/bot" + bot_token + "/sendMessage";
  std::string body = esphome::json::build_json([&](JsonObject root) {
    root["chat_id"] = chat_id;
    root["text"] = message;
  });
  // An explicit vector, not a brace literal passed inline: HttpRequestComponent::post()
  // has both a std::vector<Header> overload and a deprecated std::list<Header> one, and
  // a bare {{"Content-Type", "application/json"}} argument is ambiguous between them.
  std::vector<http_request::Header> headers{{"Content-Type", "application/json"}};
  auto container = this->http_request_->post(url, body, headers);
  if (container == nullptr) {
    ESP_LOGW(TAG, "Telegram notify failed: request could not start");
    return;
  }
  if (!http_request::is_success(container->status_code)) {
    ESP_LOGW(TAG, "Telegram notify failed: HTTP %d", container->status_code);
  } else {
    ESP_LOGI(TAG, "Telegram notify sent");
  }
  container->end();
}

// -- Phase 7: POST /setup (cloud password fetch) --
//
// A direct port of scripts/pitboss_cloud.py's fetch_and_save() onto the
// ESP32 itself (docs/PROTOCOL.md section 1) — one POST /login/app to trade
// account credentials for a short-lived JWT, one GET /customer-grills to
// read back the paired grill(s)' board id + RPC password, both discarded
// the instant they've done their job: the account password never leaves
// this function's local variables, and the JWT is used for exactly one
// follow-up call and never persisted (matching the sidecar's own "used once
// and dropped" contract — see GrillRpcService.cs's class comment on
// GrillRpcService: "This app never sees the password").

bool PitbossGrill::cloud_login_(const std::string &email, const std::string &password, const std::string &country,
                                std::string &token, std::string &error) {
  std::string body = esphome::json::build_json([&](JsonObject root) {
    root["email"] = email;
    root["password"] = password;
  });
  auto container = this->http_request_->post(std::string(CLOUD_BASE) + "/login/app", body, cloud_headers(country, ""));
  if (container == nullptr) {
    error = "could not reach the Pit Boss API";
    return false;
  }
  std::string resp_body;
  read_http_body(container.get(), resp_body);
  int status = container->status_code;
  container->end();

  // The API answers 404 UNIDENTIFIED_CUSTOMER for wrong credentials — really
  // is 404, not a typo for 401 (docs/PROTOCOL.md's own note on this).
  if (status == 404) {
    error = "login rejected — check the email and password (and the account country, if not US)";
    return false;
  }
  JsonDocument doc = esphome::json::parse_json(resp_body);
  if (status != 200 || doc.isNull()) {
    error = "login failed (HTTP " + std::to_string(status) + ")";
    return false;
  }
  std::string t = doc["data"]["token"] | "";
  if (t.empty()) {
    error = "login succeeded but returned no token";
    return false;
  }
  token = t;
  return true;
}

bool PitbossGrill::cloud_list_grills_(const std::string &token, const std::string &country, JsonDocument &out,
                                      std::string &error) {
  auto container = this->http_request_->get(std::string(CLOUD_BASE) + "/customer-grills", cloud_headers(country, token));
  if (container == nullptr) {
    error = "could not reach the Pit Boss API";
    return false;
  }
  std::string resp_body;
  read_http_body(container.get(), resp_body);
  int status = container->status_code;
  container->end();

  out = esphome::json::parse_json(resp_body);
  if (status != 200 || out.isNull()) {
    error = "reading the account's grills failed (HTTP " + std::to_string(status) + ")";
    return false;
  }
  return true;
}

// Mirrors scripts/grill_sidecar.py's setup(): body {email, password,
// country?, grill_id?, model?}, reply {ok, error?, grills?, board_id?,
// nickname?, model?, connected} — see GrillRpcService.cs's SetupAsync/
// SidecarSetupResponse and SetupDialog.razor for the exact contract this has
// to match, so that dialog needed zero changes to work against the ESP32.
// "grill_id" disambiguates when the account has more than one grill and
// more than one PBV2-board grill among them (pytboss_cloud.py's
// GrillChoiceNeeded) — the reply then carries `grills` (no passwords) for
// SetupDialog.razor's picker, and the caller resends with grill_id set.
// "model" is accepted (matching the sidecar's request shape) but not
// applied: this firmware's setpoints/lights/probe count are fixed by YAML
// config for the one grill it's compiled for (ACCEPTED_SETPOINTS_F,
// has_lights_, meat_probes_), so changing only the label without changing
// those would just make /info lie about what the hardware actually has —
// the reply always echoes back the compiled-in model_ instead.
//
// Every reply here is HTTP 200 regardless of ok/error, for the same reason
// handle_command_()'s comment gives (this ESP-IDF web server backend can't
// answer anything but 200/404/409 correctly via AsyncWebServerRequest, and
// GrillRpcService.cs's SetupAsync never checks the status code either way —
// it deserializes the body and reads `ok`/`grills`/`error` unconditionally).
//
// Both cloud calls are synchronous, blocking this httpd task for their
// combined round trip (typically a couple of seconds, two TLS handshakes)
// — the same trade-off handle_command_()'s comment documents for BLE
// commands: acceptable for a rare, user-initiated, one-time action, not
// something any polling loop triggers.
void PitbossGrill::handle_setup_(AsyncWebServerRequest *request) {
  std::string body;
  if (!read_json_body(request, body)) {
    request->send(200, "application/json", "{\"ok\":false,\"error\":\"missing or oversized JSON body\"}");
    return;
  }
  JsonDocument doc = esphome::json::parse_json(body);
  if (doc.isNull()) {
    request->send(200, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }

  auto send_error = [&](const std::string &msg) {
    std::string resp = esphome::json::build_json([&](JsonObject root) {
      root["ok"] = false;
      root["error"] = msg;
    });
    request->send(200, "application/json", resp.c_str());
  };

  std::string email = to_trim(std::string(doc["email"] | ""));
  std::string password = std::string(doc["password"] | "");
  if (email.empty() || password.empty()) {
    send_error("email and password are required");
    return;
  }
  std::string country = to_upper_trim(std::string(doc["country"] | "US"));
  bool has_grill_id = !doc["grill_id"].isNull();
  int grill_id = doc["grill_id"] | -1;

  std::string token, error;
  bool login_ok = this->cloud_login_(email, password, country, token, error);
  password.clear();  // the account password is used once and dropped, same as pytboss_cloud.py
  if (!login_ok) {
    send_error(error);
    return;
  }

  JsonDocument grills_doc;
  bool list_ok = this->cloud_list_grills_(token, country, grills_doc, error);
  token.clear();  // the account's JWT is never persisted either — only the grill's own RPC password is
  if (!list_ok) {
    send_error(error);
    return;
  }

  JsonArray rows = grills_doc["data"]["customer_grills"].as<JsonArray>();
  if (rows.isNull() || rows.size() == 0) {
    send_error("no grills on this account — pair the grill in the Pit Boss app first");
    return;
  }

  JsonObject chosen;
  if (has_grill_id) {
    for (JsonObject g : rows) {
      if ((g["id"] | -1) == grill_id) {
        chosen = g;
        break;
      }
    }
    if (chosen.isNull()) {
      send_error("grill id " + std::to_string(grill_id) + " is not on this account");
      return;
    }
  } else if (rows.size() == 1) {
    chosen = rows[0];
  } else {
    std::vector<JsonObject> pbv2_rows;
    for (JsonObject g : rows) {
      std::string board_id = g["board_id"] | "";
      if (board_id.compare(0, strlen(CLOUD_BOARD_PREFIX), CLOUD_BOARD_PREFIX) == 0)
        pbv2_rows.push_back(g);
    }
    if (pbv2_rows.size() != 1) {
      std::string resp = esphome::json::build_json([&](JsonObject root) {
        root["ok"] = false;
        root["error"] = "several grills on the account — choose one";
        JsonArray arr = root["grills"].to<JsonArray>();
        for (JsonObject g : rows) {
          JsonObject o = arr.add<JsonObject>();
          o["grill_id"] = g["id"];
          o["board_id"] = g["board_id"];
          o["nickname"] = g["grill_nickname"];
          std::string pw = g["password"] | "";
          o["has_password"] = !pw.empty();
        }
      });
      request->send(200, "application/json", resp.c_str());
      return;
    }
    chosen = pbv2_rows[0];
  }

  std::string board_id = chosen["board_id"] | "";
  std::string grill_pw = chosen["password"] | "";
  std::string nickname = chosen["grill_nickname"] | "";
  if (board_id.empty() || grill_pw.empty()) {
    send_error("the grill record has no board_id/password yet — finish pairing it in the Pit Boss app");
    return;
  }

  // Deferred to the main loop task — see nvs_save_string_deferred_()'s
  // comment (doing the flash write inline here stack-overflowed the httpd
  // task on the bench). Not fatal to this response either way: the fetched
  // password works for BLE auth immediately (set in-memory right below,
  // synchronously), it just won't survive a reboot if the deferred write
  // happens to fail — nvs_save_string_() logs its own warning if so.
  this->nvs_save_string_deferred_(NVS_KEY_GRILL_PW, grill_pw);
  {
    LockGuard lock(this->state_mutex_);
    this->grill_password_ = grill_pw;
  }
  grill_pw.clear();

  bool connected = this->state() == espbt::ClientState::ESTABLISHED;
  std::string resp = esphome::json::build_json([&](JsonObject root) {
    root["ok"] = true;
    root["board_id"] = board_id;
    if (!nickname.empty())
      root["nickname"] = nickname;
    root["model"] = this->model_;
    root["connected"] = connected;
  });
  request->send(200, "application/json", resp.c_str());
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
      // Without this, a disconnect mid-command left pending_reply_/
      // pending_after_time_ stuck forever: nothing else clears them, so
      // every later GetState cycle would see "a reply is still in flight"
      // and no-op permanently, and any handle_command_() blocked on
      // command_done_sem_ would just time out with the link still marked
      // busy instead of recovering once it reconnects.
      bool was_command = this->pending_after_time_ == PendingReply::MCU_COMMAND;
      this->pending_reply_ = PendingReply::NONE;
      this->pending_after_time_ = PendingReply::GET_STATE;
      if (was_command) {
        this->command_result_error_ = "grill BLE link dropped mid-command";
        xSemaphoreGive(this->command_done_sem_);
      }
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
