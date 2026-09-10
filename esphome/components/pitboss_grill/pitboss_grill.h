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
// debug-log pushes carry the same two frames, decoded the same way. Phase 4
// exposes that state over the same three read-only REST routes today's
// scripts/grill_sidecar.py serves (/health, /state, /info) — registered on
// ESPHome's own shared httpd (web_server_base), not a second HTTP server —
// so nginx's proxy_pass can point straight at this ESP32 with zero Blazor
// frontend changes. Phase 5 adds POST /command (turn-on/turn-off/
// set-temperature) behind the same confirm semantics grill_sidecar.py's
// bridge.command() enforces today — see handle_command_()'s comment for the
// full flow. All five phases are bench-verified against real hardware.
// POST /config (added 2026-09-10, not a numbered phase) makes
// error_display_threshold_ — how many consecutive PB.GetState rejections
// before one is shown as an error — a runtime setting instead of a
// hardcoded constant; see its own comment below. Phase 6 adds
// GET/POST /alarms + DELETE /alarms/{id} (temp-target/timer alarms, same
// semantics as scripts/alarms.py) and a Telegram notify() on fire — see
// check_alarms_()/notify_()'s comments. Phase 7 adds POST /setup — a direct
// port of scripts/pitboss_cloud.py's one-time Pit Boss cloud login onto the
// ESP32 itself (see handle_setup_()/cloud_login_()/cloud_list_grills_()) —
// and NVS persistence for the grill password it fetches, the alarms list,
// and the Telegram bot token/chat id (POST /config), so none of that is
// lost on a reboot/reflash anymore. See load_persisted_state_()/
// nvs_save_string_()'s comments for the storage layout (docs/
// ESP32_FIRMWARE_PLAN.md's "Decisions made" #7).

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/http_request/http_request.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <esp_gattc_api.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <string>
#include <vector>

namespace esphome {
namespace pitboss_grill {

namespace espbt = esphome::esp32_ble_tracker;
using esphome::esp32_ble::ESPBTUUID;
using namespace esp32_ble_client;

// AsyncWebHandler/AsyncWebServerRequest come from web_server_idf.h's global
// `using namespace esphome::web_server_idf` (see that header) — unqualified
// here to match how every other web_server_base consumer (web_server,
// prometheus, captive_portal) spells them.
class PitbossGrill : public BLEClientBase, public AsyncWebHandler {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // BLEClientBase defaults to setup_priority::BLUETOOTH (350) — fine for
  // Phases 1-3 (BLE client registration itself happens outside setup(), via
  // a raw register_client() call esp32_ble_tracker's codegen emits, so
  // component setup ORDER never mattered there). Phase 4's
  // web_server_base_->init() does need the network stack up first, though:
  // running it at BLUETOOTH priority (i.e. before wifi's own setup()) OTA'd
  // fine but crash-looped the instant the very first real HTTP GET came in.
  // Same fix/reasoning as esphome/components/prometheus's PrometheusHandler.
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  bool parse_device(const espbt::ESPBTDevice &device) override;
  bool gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  /// Advertised-name prefix to match — the grill's BLE address rotates, so
  /// (like the sidecar's esphome_ble.find_grill() today) this matches by
  /// name instead of a fixed MAC. Defaults to this project's tested board.
  void set_name_prefix(const std::string &prefix) { this->name_prefix_ = prefix; }

  /// The per-grill RPC password. Phase 2 needed this as a compile-time
  /// secret to bench-test the auth codec at all; Phase 7 replaces that with
  /// a real fetch-and-persist flow (POST /setup -> NVS, see
  /// load_persisted_state_()) run from the ESP32 itself, so this YAML value
  /// is now only the *initial* fallback used until the first successful
  /// /setup — NVS wins once it has one. May be left "" in YAML entirely.
  void set_grill_password(const std::string &password) { this->grill_password_ = password; }

  /// The shared ESPHome httpd instance (see esphome/grill-firmware.yaml's
  /// `web_server_base:`/`web_server:` block) — REST routes below are
  /// registered on it in setup(), the same pattern web_server/prometheus/
  /// captive_portal use, rather than standing up a second HTTP server.
  void set_web_server_base(web_server_base::WebServerBase *base) { this->web_server_base_ = base; }

  /// The friendly model name this firmware is built for (today's sidecar's
  /// DEFAULT_MODEL) — reported as-is by /info; no per-grill autodetection
  /// happens here, same as the sidecar today.
  void set_model(const std::string &model) { this->model_ = model; }

  /// This grill's fixed hardware capabilities, reported as-is by /info —
  /// same numbers the sidecar got from pytboss's grills.json spec for this
  /// model (no autodetection here either). Defaults below match "PBV5 P2"
  /// specifically; a different model must set both explicitly in YAML.
  /// Getting meat_probes wrong doesn't just misreport a number: it's the
  /// probe count GrillDetail.razor's AvailableSensors() loops over to
  /// decide how many probe cards to render, so a real hardware bug here
  /// simply prints a "Probe N" the physical grill doesn't have.
  void set_has_lights(bool has_lights) { this->has_lights_ = has_lights; }
  void set_meat_probes(uint8_t meat_probes) { this->meat_probes_ = meat_probes; }

  /// Real wall-clock time (grill-firmware.yaml's `time:` block, any
  /// platform) — a timer alarm's fires_at has to be a real Unix timestamp
  /// GrillDetail.razor's DescribeAlarm() can subtract DateTimeOffset.UtcNow
  /// from, not just millis()-since-boot. See check_alarms_()/handle_alarms_
  /// post_()'s comments.
  void set_time(time::RealTimeClock *clock) { this->time_ = clock; }

  /// Shared ESPHome HTTP client (grill-firmware.yaml auto-loads this with
  /// defaults, same as web_server_base) — notify_()'s one POST to Telegram's
  /// Bot API, per docs/ESP32_FIRMWARE_PLAN.md's "Telegram integration"
  /// section.
  void set_http_request(http_request::HttpRequestComponent *client) { this->http_request_ = client; }

  /// Telegram Bot API credentials (see grill-firmware.yaml's comment on
  /// obtaining them) — both default to "" (notify_() then just logs instead
  /// of sending), so this compiles and runs with no bot configured at all.
  void set_telegram_bot_token(const std::string &token) { this->telegram_bot_token_ = token; }
  void set_telegram_chat_id(const std::string &chat_id) { this->telegram_chat_id_ = chat_id; }

  // -- AsyncWebHandler (web_server_base's shared httpd) --
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

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

  // Phase 6: one alarm, as created via POST /alarms — field names/shapes
  // deliberately mirror scripts/alarms.py's AlarmStore entries exactly (see
  // OpenPit32/Services/GrillRpcService.cs's AlarmDto) so nginx can point
  // /api/alarms at this ESP32 with zero frontend changes, same as every
  // other route here. "temp" alarms use sensor/comparison/target and leave
  // duration_seconds/fires_at unset; "timer" alarms are the reverse. Both
  // kinds are real Unix-epoch seconds in created_at/fires_at (via time_),
  // not millis()-since-boot — see set_time()'s comment.
  struct Alarm {
    std::string id;
    std::string kind;  // "temp" | "timer"
    std::string label;
    std::string sensor;       // temp only: "grillTemp"/"smokerActTemp"/"p1Temp".."p4Temp"
    std::string comparison;   // temp only: "at_or_above" | "at_or_below"
    double target{0};         // temp only
    int duration_seconds{0};  // timer only
    double fires_at{0};       // timer only
    double created_at{0};
  };

 protected:
  void resolve_characteristics_();
  void register_for_notifications_();
  void send_ping_();
  void send_get_state_cycle_();
  void send_get_time_();
  void send_get_state_(double uptime);
  void send_mcu_command_(double uptime);
  void on_get_time_reply_(const std::string &json);
  void on_get_state_reply_(const std::string &json);
  void on_mcu_command_reply_(const std::string &json);
  void on_rpc_notify_(const uint8_t *data, uint16_t len);
  void on_rpc_read_(const uint8_t *data, uint16_t len);
  void on_debug_log_(const uint8_t *data, uint16_t len);
  void request_next_reply_chunk_();
  void write_rpc_command_(const std::string &json);
  void write_next_rpc_chunk_();
  void on_rpc_write_complete_(uint16_t handle, esp_gatt_status_t status);
  void parse_status_frame_(const std::string &hex);
  void parse_temperature_frame_(const std::string &hex);

  // REST handlers — see docs/ESP32_FIRMWARE_PLAN.md's rollout plan item 4.
  // Field names/shapes deliberately mirror scripts/grill_sidecar.py's
  // /health, /state, /info exactly (see OpenPit32/Services/GrillRpcService.cs
  // for the Blazor-side DTOs this has to match) so nginx can point at this
  // ESP32 instead of the sidecar with zero frontend changes.
  void handle_health_(AsyncWebServerRequest *request);
  void handle_state_(AsyncWebServerRequest *request);
  void handle_info_(AsyncWebServerRequest *request);
  // Phase 5: POST /command — turn-on/turn-off/set-temperature. See the .cpp
  // for the full flow (it blocks the calling httpd task on a semaphore until
  // the BLE round trip actually completes, mirroring grill_sidecar.py's
  // bridge.command(), which the caller awaits to completion the same way).
  void handle_command_(AsyncWebServerRequest *request);
  // POST /config — error_display_threshold, and (Phase 7) the Telegram bot
  // token/chat id, each persisted to NVS as they're set (see
  // nvs_save_string_()) the same runtime-settable-without-a-reflash pattern
  // error_display_threshold established. Doesn't touch BLE at all, so unlike
  // handle_command_() this answers synchronously with no defer()/semaphore.
  void handle_config_(AsyncWebServerRequest *request);

  // Phase 7: POST /setup — a direct port of scripts/pitboss_cloud.py's
  // fetch_and_save() onto the ESP32 itself (docs/PROTOCOL.md section 1: one
  // POST /login/app, one GET /customer-grills). See the .cpp for the full
  // flow and GrillRpcService.cs's SetupAsync/SidecarSetupResponse for the
  // exact request/reply shape SetupDialog.razor expects — this mirrors it
  // so that dialog needed zero changes. cloud_login_()/cloud_list_grills_()
  // are its two HTTPS calls, split out so handle_setup_() itself reads as
  // the same choose-a-grill logic pitboss_cloud.py's fetch_and_save() has.
  void handle_setup_(AsyncWebServerRequest *request);
  bool cloud_login_(const std::string &email, const std::string &password, const std::string &country,
                    std::string &token, std::string &error);
  bool cloud_list_grills_(const std::string &token, const std::string &country, JsonDocument &out,
                          std::string &error);

  // Phase 6: GET/POST /alarms — see alarms_'s comment on locking.
  void handle_alarms_get_(AsyncWebServerRequest *request);
  void handle_alarms_post_(AsyncWebServerRequest *request);
  // DELETE /alarms/{id} is NOT one of these: web_server_idf's AsyncWebServer
  // only ever registers HTTP_GET/HTTP_POST/HTTP_OPTIONS handlers with the
  // underlying esp_http_server (see AsyncWebServer::begin() in
  // web_server_idf.cpp) — there is no code path for any other method,
  // regardless of what canHandle()/handleRequest() do. Matching the
  // sidecar's real `DELETE /alarms/{id}` (rather than inventing a
  // POST-based delete just for this platform, which is a permanent frontend
  // divergence) means registering our own HTTP_DELETE handler directly on
  // the raw httpd_handle_t in setup() — see setup_delete_handler_()/
  // handle_alarms_delete_() in the .cpp. That bypasses AsyncWebServerRequest
  // entirely (its constructor is private to AsyncWebServer), so this one
  // route talks to esp_http_server's C API directly rather than through the
  // AsyncWebHandler abstraction the rest of this file uses.
  void setup_delete_handler_();
  static esp_err_t delete_alarm_trampoline_(httpd_req_t *req);
  esp_err_t handle_alarms_delete_(httpd_req_t *req);

  // Runs every 5s (setup()'s "alarm_check" interval, matching
  // scripts/alarms.py's CHECK_SECONDS) — evaluates every alarm against
  // grill_state()/time_->timestamp_now(), fires (notify_()) and drops any
  // that are due. A fired alarm is removed, never re-armed or repeated,
  // same as the sidecar today.
  void check_alarms_();
  // One HTTPS POST to Telegram's Bot API — see set_telegram_bot_token()'s
  // comment on the no-bot-configured case. Deliberately this project's only
  // caller of http_request_, kept in its own function per
  // docs/ESP32_FIRMWARE_PLAN.md's "Telegram integration" section, so
  // swapping to ntfy.sh/Pushover later touches only this one body.
  void notify_(const std::string &message);

  // Phase 4 is the first thing that reads grill_state_/last_error_/board_id_/
  // last_rssi_ from outside the task that writes them — the REST handlers
  // above run on esp_http_server's own httpd task, while writers span the
  // BT stack's task (gattc_event_handler()/on_debug_log_() run there
  // directly, not deferred — see on_rpc_read_()'s comment on why *that*
  // defer exists, which is stack depth, not thread-safety) and the main
  // loop task (deferred GetState replies). Three FreeRTOS tasks touching
  // non-trivial (heap-backed std::string) fields with no synchronization is
  // a real crash risk, not a theoretical one — confirmed on the bench
  // 2026-09-09: the very first real HTTP request after adding /health
  // crash-looped the device. set_last_error_()/clear_last_error_() and the
  // LockGuards in parse_device()/parse_status_frame_()/
  // parse_temperature_frame_()/handle_*_() are the fix.
  void set_last_error_(const std::string &message);
  void clear_last_error_();

  // Phase 7: raw ESP-IDF NVS (not ESPHome's own typed ESPPreferences API,
  // which is built for small fixed-size structs, not the variable-length
  // strings/JSON this needs) under one namespace — see the .cpp for the key
  // names. NVS itself is already initialized by the time any Component::
  // setup() runs (esphome::esp32::ESP32Preferences calls nvs_flash_init()
  // from app_main(), before the logger or any component — see that
  // component's preferences.cpp), so nvs_open() is always safe to call here
  // with no init step of our own.
  bool nvs_load_string_(const char *key, std::string &out) const;
  bool nvs_save_string_(const char *key, const std::string &value);
  // Bench-confirmed 2026-09-10: calling nvs_save_string_() (or anything that
  // JSON-builds first, like save_alarms_locked_()) directly from a REST
  // handler stack-overflowed and crash-looped the real device
  // (vApplicationStackOverflowHook) — the ESP-IDF httpd task's small
  // (~4KB) default stack, already used by JSON building, has no headroom
  // left for NVS's own flash access (worse on a key's first-ever write,
  // which may need to allocate/erase a page). Every REST handler that
  // persists a plain string now goes through this instead of calling
  // nvs_save_string_() directly — same fix in spirit as on_rpc_read_()'s
  // defer() of JSON parsing off the BLE callback's own small stack, applied
  // to the httpd task instead of the BT stack's task.
  void nvs_save_string_deferred_(const char *key, const std::string &value);
  // Called once from setup(), before anything else can touch the fields it
  // sets: grill_password_/telegram_bot_token_/telegram_chat_id_ are
  // overridden by whatever's in NVS (a prior POST /setup or POST /config),
  // falling back to the YAML-configured value if NVS has nothing yet;
  // alarms_ is restored from its own NVS blob the same way scripts/
  // alarms.py's AlarmStore reloads alarms.json on restart.
  void load_persisted_state_();
  // Rewrites the whole "alarms" NVS key from the current alarms_ — call with
  // alarms_mutex_ already held, same as every other alarms_ mutation in this
  // file. Mirrors decision #7 in docs/ESP32_FIRMWARE_PLAN.md: "a direct port
  // of what scripts/alarms.py already does" — rewritten on every add/
  // remove/fire, not diffed. Builds JSON + writes NVS, so — per
  // nvs_save_string_deferred_()'s comment — only ever call this from the
  // main loop task (check_alarms_() already is one; the httpd-task handlers
  // must this->defer() a fresh LockGuard + call, not call it inline).
  void save_alarms_locked_();

  Mutex state_mutex_;

  std::string name_prefix_{"PBV2-"};
  // Set once at boot (YAML default, then possibly overridden by NVS — see
  // load_persisted_state_()), but Phase 7's POST /setup can rewrite it at
  // any later time from the httpd task, while send_get_state_()/
  // send_mcu_command_() read it every cycle from the main loop — every
  // access outside setup()/load_persisted_state_() itself must go through
  // state_mutex_, unlike model_ below (still truly write-once).
  std::string grill_password_;
  std::string model_;
  bool has_lights_{false};
  uint8_t meat_probes_{3};
  web_server_base::WebServerBase *web_server_base_{nullptr};

  // How many consecutive PB.GetState rejections (see get_state_reject_
  // streak_) are required before one is surfaced as last_error_ — the "5
  // consecutive cycles" delay requested 2026-09-10, made runtime-adjustable
  // (POST /config, see handle_config_()) rather than a YAML compile-time
  // constant so the web app can tune it without a reflash. atomic because
  // handle_config_() (httpd task) writes it and on_get_state_reply_() (main
  // loop) reads it — a plain uint8_t would be a real, if narrow, data race.
  // In-memory only: resets to the default on every boot/reflash, same as
  // everything else here that isn't yet in the Phase 7 NVS plan.
  std::atomic<uint8_t> error_display_threshold_{5};

  // The grill's full advertised name (e.g. "PBV2-9451DC46B934"), captured in
  // parse_device() — reported by /info as board_id, same as the sidecar's.
  std::string board_id_;

  // Grill BLE advertisement RSSI — only updated by parse_device(), which
  // (like the rest of BLE advertising) goes quiet once connected; matches
  // the sidecar's own "rssi" field and its same staleness caveat (see
  // /health's dump in docs/ESP32_FIRMWARE_PLAN.md's Phase 1 notes).
  int8_t last_rssi_{0};
  bool has_rssi_{false};

  // millis() timestamp of the last successfully decoded status/temperature
  // frame (either source) — /health and /state report age off this, mirror
  // of bridge.state_at in grill_sidecar.py.
  uint32_t last_frame_millis_{0};
  bool has_frame_millis_{false};

  // Mirrors bridge.last_error in grill_sidecar.py: the most recent
  // RPC/decode failure, cleared on the next success. Empty means "no error
  // outstanding", not "never started".
  std::string last_error_;

  // Consecutive PB.GetState rejections (see on_get_state_reply_()) since the
  // last success — main-loop-only, same as pending_reply_, since it's only
  // ever touched from there. A single spurious 401 is expected, documented
  // behavior (docs/PROTOCOL.md: a slow write can land in the wrong 10s auth
  // key bucket) that self-heals on the very next 15s cycle, so surfacing it
  // as last_error_ on the first occurrence just flashes a false alarm in the
  // UI — set_last_error_() is only called once this streak reaches
  // error_display_threshold_. Does NOT gate the ESP_LOGW in
  // on_get_state_reply_() itself, which stays unthrottled for diagnostics.
  uint8_t get_state_reject_streak_{0};

  // Only one RPC request is ever in flight at a time (see write_rpc_command_
  // and the reply-reassembly fields below) — this says which one, so
  // on_rpc_read_'s completion handler knows how to interpret the reply
  // without needing to track/match request ids of our own.
  enum class PendingReply { NONE, PING, GET_TIME, GET_STATE, MCU_COMMAND };
  PendingReply pending_reply_{PendingReply::NONE};

  // GET_TIME is a shared first step (every authenticated call needs a fresh
  // uptime to derive its key) — this says what on_get_time_reply_() should
  // do once it lands: continue the periodic GetState cycle (the default), or
  // send the MCU command handle_command_() queued in command_pending_hex_.
  // Main-loop-only, like pending_reply_ itself — see handle_command_()'s
  // comment for why the httpd task never touches this directly.
  PendingReply pending_after_time_{PendingReply::GET_STATE};

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

  // Phase 5 (POST /command) — see handle_command_() in the .cpp for the full
  // flow. command_mutex_ serializes concurrent POST /command requests (only
  // one physical BLE link exists to drive); command_done_sem_ is how the
  // httpd task, blocked waiting for a command's real result, is woken by the
  // main-loop task once on_mcu_command_reply_() (or a timeout) has one.
  // command_pending_hex_/command_result_error_/command_retried_ are only
  // ever written by the main loop and only ever read by the httpd task after
  // the semaphore wakes it — that handoff is the synchronization, so neither
  // needs state_mutex_.
  Mutex command_mutex_;
  SemaphoreHandle_t command_done_sem_{nullptr};
  std::string command_pending_hex_;
  std::string command_result_error_;
  bool command_retried_{false};

  GrillState grill_state_;

  // Phase 6 (alarms) — alarms_mutex_ guards the vector itself: written from
  // the httpd task (handle_alarms_post_()/handle_alarms_delete_()) and read
  // from both the httpd task (handle_alarms_get_()) and the main loop
  // (check_alarms_(), on the "alarm_check" interval). Snapshot-under-lock,
  // release, THEN act (build JSON / call notify_()) is the same pattern
  // state_mutex_'s consumers already use, for the same reason: never hold a
  // lock across a JSON build or a blocking network call.
  Mutex alarms_mutex_;
  std::vector<Alarm> alarms_;

  time::RealTimeClock *time_{nullptr};
  http_request::HttpRequestComponent *http_request_{nullptr};
  // Phase 7: both now runtime-settable via POST /config and persisted to
  // NVS (see handle_config_()/load_persisted_state_()) — the YAML value is
  // only the initial default, same relationship grill_password_ now has
  // with POST /setup. Read from the main loop (notify_()) and written from
  // the httpd task (handle_config_()), so — like grill_password_ — every
  // access outside setup()/load_persisted_state_() goes through
  // state_mutex_.
  std::string telegram_bot_token_;
  std::string telegram_chat_id_;
};

}  // namespace pitboss_grill
}  // namespace esphome

#endif
