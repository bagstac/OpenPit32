# Code review findings — 2026-10-20

Full-repo review (ESP32 firmware, Blazor client, host-side Python/nginx),
run by three independent reviewers with the results cross-checked against the
code. Claims marked **[verified]** were re-read in the source and confirmed
by the orchestrator; everything else is reviewer-reported with file:line so it
can be checked in seconds.

Reviewers were told to ignore style and documented trade-offs, and to flag
comments that claim safety the code doesn't have. Nothing was modified.

Priority order for fixes is at the bottom.

---

## P0 — safety / breaks the app's core invariants

### P0-1. A failed command can re-send itself later — including power-on
- **Where:** `esphome/components/pitboss_grill/pitboss_grill.cpp:969`
  (`on_get_time_reply_()`), interacting with the `get_state` interval at
  `:514` and `send_get_state_cycle_()` at `:918`. **[verified]**
- **Issue:** `on_get_time_reply_()`'s three early-return error paths (reply
  not valid JSON / reply carries an `error` field / `result.time` missing)
  return without resetting `pending_after_time_` or clearing
  `command_pending_hex_`. The comment says only `on_mcu_command_reply_()`
  resets it — but if that reply never arrives, the next 15 s periodic cycle
  runs `send_get_state_cycle_()` → `send_get_time_()` →
  `on_get_time_reply_()`, sees `pending_after_time_ == MCU_COMMAND`, and
  **re-sends the stale `command_pending_hex_`** (e.g. `FE0101FF` power-on)
  minutes after the blocked `POST /command` already timed out at 12 s and
  told the user it failed. `on_mcu_command_reply_()`'s rejection-retry path
  (`:1067`) reaches the same dead end via its own `send_get_time_()`. Only a
  BLE disconnect resets the state otherwise.
- **Why it matters:** an uncommanded ignition on a pellet smoker after a
  reported failure — defeats the confirm-before-power-on gate.
- **Fix:** in each early-return path, when `pending_after_time_ ==
  MCU_COMMAND`: set `pending_after_time_ = GET_STATE`, clear
  `command_pending_hex_`, set `command_result_error_`, and
  `xSemaphoreGive(command_done_sem_)` — the same final handling
  `abandon_pending_rpc_()` performs. Factor into a small helper shared with
  `abandon_pending_rpc_()` so no future path can skip it.

### P0-2. Gate detection misfires on nginx error pages → login bounce loop
- **Where:** `OpenPit32/Services/SessionExpiryHandler.cs:56`
  (`PromptForLoginIfSessionExpired`). **[verified]**
- **Issue:** the handler navigates to `/login` on **any non-JSON response**
  without checking `response.IsSuccessStatusCode`. The followed login gate
  always arrives as HTTP 200 HTML — but so do nginx's own 502/504 error
  pages, which occur whenever the grill's ESP32 is off or unreachable (a
  normal state for a grill). With a valid session, every poll
  force-navigates to `/login`; reviewer reports the login page renders the
  form even for authenticated users (no redirect-back), so the app bounces
  `/grill → /login → /grill` in an endless loop and never shows the real
  "grill unreachable" condition. Sibling issue in
  `service-worker.published.js`'s navigate branch: any non-ok/redirected
  navigation is answered with the "session expired" synthetic page.
- **Why it matters:** regression of the exact 2026-10-12 bug class this
  handler was written to fix — the app blaming everything except the actual
  failure.
- **Fix:** only treat the response as the gate when it is **both** non-JSON
  **and** success-statused: `if (response.IsSuccessStatusCode &&
  !IsJson(response)) { …navigate… }`. Optionally sniff the body for a
  login-page marker for defense in depth.

### P0-3. Any LAN device can control the grill
- **Where:** `docker/nginx.conf.template:131` (proxies add no credentials) +
  the firmware's httpd (no auth). **[verified]**
- **Issue:** nginx forwards `/api/*` to the ESP32 with no credentials, and
  the firmware's HTTP server (web_server_base auto-loaded with empty config)
  exposes `/command`, `/config`, `/alarms`, `/setup` unauthenticated. The
  Phase-5 "confirm-required safety gate" is a `confirm: true` body field the
  caller sets itself. Anything on the home LAN (guest Wi-Fi, a compromised
  IoT box) can `POST {"action":"power_on","confirm":true}` to
  `http://192.168.1.114/command` with no login and no token.
- **Scope:** NOT reachable via the public tunnel — every nginx `/api/*`
  location is `auth_request`-gated (verified). This is LAN-only exposure,
  but the LAN is half the deployment's surface and the physical effect
  raises the stakes.
- **Fix:** enable auth on ESPHome's `web_server_base` and have nginx inject
  the credential when proxying (`proxy_set_header Authorization "Basic …"`),
  or firewall the ESP32 to accept connections only from 192.168.1.131.

---

## P1 — real bugs, lower stakes

### Firmware

- **BLE task writes "main-loop-only" RPC state** — `pitboss_grill.cpp:2543`:
  `gattc_event_handler()` calls `send_ping_()` directly on the BLE stack
  task, writing `pending_reply_`, `rpc_write_json_`, `rpc_write_offset_`,
  `rpc_write_in_progress_` — fields the header declares main-loop-only.
  `on_rpc_read_()` (`:1242`) likewise clears `pending_reply_` from the BLE
  task while the main loop's watchdog reads/writes the same state. On this
  dual-core ESP32 the narrow window allows two concurrent
  `write_rpc_command_()` calls racing on a `std::string` (heap-corrupting
  UB), or a reply completing exactly as the 5 s watchdog abandons, clobbering
  the new request's kind and disarming the watchdog mid-flight (turning a
  dropped reply into a full 12 s `/command` timeout). *Fix: defer the ping;
  move the `pending_reply_` reset into the deferred dispatch; or guard the
  RPC-state fields with a dedicated mutex.* (Never observed to crash in
  weeks of running — narrow windows — but the comments claim an ownership
  discipline the code doesn't have.)

- **`/setup` retry can consume the previous attempt's result** —
  `pitboss_grill.cpp:2364`: no in-flight or generation guard around the new
  `setup_done_sem_` handoff (added 2026-10-20). A `run_cloud_setup_()` from a
  prior request that timed out at 25 s can still be queued/executing; when it
  finishes it gives the semaphore and the new handler consumes the OLD run's
  `setup_result_body_` — persisting the previous attempt's grill password
  even if credentials were corrected between attempts, and leaving a stale
  token for the request after that. *Fix: `uint32 setup_generation_`
  incremented by the handler and captured by value into the lambda;
  `run_cloud_setup_()` stores/gives only if the generation still matches (or
  refuse new calls while `setup_in_progress_`).*

- **Largest JSON parse in the firmware still runs on the httpd task** —
  `pitboss_grill.cpp:2392`: `handle_setup_()` re-parses the up-to-4 KB
  `/customer-grills` reply, iterates it, and builds the response JSON on the
  ~4 KB-stack httpd task — the task this project has crash-looped three
  times. `run_cloud_setup_()` moved the TLS off but left this behind. *Fix:
  do grill selection and response building inside `run_cloud_setup_()` on
  the main loop; `send()` the prebuilt string.*

- **Half-established link never recovers** — `pitboss_grill.cpp:799`: if
  `esp_ble_gattc_register_for_notify()` fails for either characteristic (or
  `resolve_characteristics_()` finds one missing, `:775`), the code only
  logs: `notifies_expected_` stays 2, state never becomes ESTABLISHED, and
  the open connection is never torn down or retried — no GetState cycles,
  every command answers "grill not connected", potentially for hours until
  the grill drops the link itself. *Fix: actively disconnect on failure so
  auto-reconnect retries the whole sequence; only count successfully
  initiated registrations toward `notifies_expected_`.*

- **`command_result_error_` written from three tasks unlocked** —
  `pitboss_grill.cpp:1720`: the httpd task's `clear()` races writes from the
  main loop (`on_mcu_command_reply_` `:1073/1076`,
  `abandon_pending_rpc_` `:586`) and the BLE task (disconnect `:2518`) when a
  timed-out command's retry is still completing. The header comment ("only
  ever written by the main loop") is violated. *Fix: move the clear into the
  deferred lambda on the main loop, or guard with a mutex.*

### Blazor client

- **Polls have no reentrancy guard** — `GrillDetail.razor:309` (same pattern
  `Home.razor:98`, `Settings.razor:110`, `GrillEvents.razor:81`,
  `BridgeHealth.razor:96`): a 10 s `System.Threading.Timer` fires
  `_ => _ = InvokeAsync(PollSidecar)` regardless of whether the previous poll
  (up to 4 sequential HTTP calls, 100 s default timeout) finished. While the
  backend hangs, polls pile up against the contended ESP32 radio, interleaved
  completions can clobber fresh state with stale responses, and discarded
  tasks swallow exceptions. *Fix: `Interlocked` in-flight flag, or replace
  the Timer with a `PeriodicTimer` consumed by an awaited loop.*

- **Unhandled exceptions in alarm handlers** — `GrillDetail.razor:387`
  (`RemoveAlarm` has no try/catch; `AddAlarm` uses try/finally without
  catch, `:361-374`): a routine network blip while adding/deleting an alarm
  escapes the event handler and hits Blazor's fatal "unhandled error" UI,
  killing the page until reload. *Fix: try/catch → existing `alarmError`
  banner, matching `Send()`'s pattern.*

### Host side

- **Failed-login logging is dead code** — `scripts/login_service.py:80`:
  `logging.basicConfig(level=logging.WARNING)` filters out the
  `_LOGGER.info("login failed …")` record at `:293`, so a brute-force run
  against the public `POST /login` (~86k guesses/day at the 1 s
  `LOGIN_FAIL_DELAY`) leaves zero operator-visible signal. *Fix: level INFO;
  the only INFO record is the failed-login line, so volume stays trivial.*

- **No rate limiting / lockout on `POST /login`** —
  `scripts/login_service.py:87`: reachable from the internet via the
  Cloudflare tunnel; only a flat 1 s sleep per failure. *Fix: Cloudflare
  rate-limit rule on `POST /login`, or per-IP failure tracking with backoff
  keyed off `X-Forwarded-For`.*

- **Event-log `source_ip` is meaningless** —
  `docker/nginx.conf.template:132`: no `X-Forwarded-For`/`X-Real-IP` on the
  ESP32 proxy blocks, so the event log's `source_ip` — the point of the
  2026-10-05 "who is resetting the setpoint" investigation — always records
  the nginx container's bridge IP. *Fix: `proxy_set_header X-Forwarded-For
  $proxy_add_x_forwarded_for;` and have the firmware prefer the XFF value.*

- **`index.html` has no `Cache-Control`** — `nginx.conf.template:115`: the
  no-cache location covers the stable-named files but not `index.html`
  itself, so browsers apply heuristic caching (≈10 % of time-since-modified)
  — the same stale-asset failure class as the two documented 2026-09-12
  incidents; after weeks live, a redeploy can leave a browser serving an old
  shell referencing deleted hashed files. *Fix: add `index.html` to the
  no-cache location.*

- **`AUTH_USERS` passwords containing a comma are silently truncated** —
  `scripts/login_service.py:148`: the parse splits on `,` and takes the
  first `:`; `alice:pa,ssword` yields the accepted password `pa` — weaker
  than the user believes, with no error. *Fix: reject entries containing a
  comma at startup, or switch to a proper list format.*

- **Plaintext on the LAN leg** — `docker/README.md:70` documents it: the
  login password POST, the 30-day session cookie, and the ESP32's Bearer
  `EVENT_LOG_TOKEN` to `:8091/log-event` all transit plain HTTP on the LAN;
  the tunnel's TLS protects only the internet leg. A Wi-Fi sniffer gets the
  login password once and owns the account on the public hostname too.
  `.env` also holds `AUTH_PASSWORD`/`AUTH_USERS` in cleartext (default 0644
  after the documented `cp`+nano flow, plus readable via `docker inspect`).
  *Fix: internal-CA TLS on the LAN origin or an explicit trusted-LAN
  decision + Wi-Fi hardening; `chmod 600 .env`; consider bcrypt hashes in
  `AUTH_USERS`.*

- **Auth container runs as root** — `docker/auth.Dockerfile:31`: no `USER`
  directive, while it's the network-exposed service parsing unauthenticated
  input and holding the session-signing secret and events DB. *Fix: add a
  non-root user (port 8091 is unprivileged, no caps needed).*

---

## P2 — worth doing, not urgent

- **`nginx.conf.template:220`** — the `location ~ \.wasm$` regex block has no
  `auth_request` (regex beats the gated prefix location), silently breaking
  the "everything behind the login" invariant. Low impact today (the .wasm
  files are public .NET runtime binaries; app code is in gated .dll files)
  but any future wwwroot .wasm would be served unauthenticated via the
  tunnel. *Fix: add the auth_request/redirect to that block.*
- **`nginx.conf.template:45`** — no security headers at all
  (`X-Content-Type-Options`, `X-Frame-Options`, `Referrer-Policy`;
  SameSite=Lax already blunts clickjacking). Cheap defense-in-depth.
- **`scripts/login_service.py:176`** — `expires.isdigit()` accepts non-ASCII
  Unicode digits for which `int()` raises, turning a malformed cookie into a
  500 on `/auth-check` instead of a clean 401. *Fix: `isascii() and
  isdigit()` or try/except.*
- **`scripts/login_service.py:280`** — `POST /login` has no CSRF token or
  Origin check (login-CSRF). Negligible on this deployment (sessions carry
  no per-user identity; SameSite=Lax), so hygiene only.
- **No `.dockerignore`** — every build ships the full repo as context:
  `.env`, `esphome/secrets.yaml`, `.git`, two venvs. Not COPYed into any
  layer today, but streamed to the daemon each build and one broadened COPY
  away from being baked into an image. *Fix: add one.*
- **`.gitignore`** — `scripts/grill_events.db` (default DB path when running
  the login service outside Docker) is not ignored; a casual `git add .`
  would commit source IPs. *Fix: add the line.*
- **`pitboss_grill.cpp:1218`** — `on_rpc_notify_()` trusts the announced
  4-byte reply length with no cap; a corrupted announcement can grow
  `rpc_reply_buffer_` unbounded (and if it arrives with
  `pending_reply_==NONE` the 5 s watchdog is disarmed). *Fix: reject
  `reply_len == 0 || reply_len > 2048`.*
- **`pitboss_grill.cpp:2019`** — `save_alarms_locked_()` (JSON build + NVS
  commit, may erase a page) is called while holding `alarms_mutex_`,
  blocking the httpd `/alarms` handlers for the whole write — violating the
  header's own "never hold a lock across a JSON build or blocking call"
  rule. No deadlock (no lock cycle), just the documented-pattern violation.
  *Fix: snapshot under lock, serialize+write outside.*
- **`pitboss_grill.cpp:478`** — `xSemaphoreCreateBinary()` results unchecked;
  a low-memory boot makes every later take/give fault. *Fix: check and skip
  REST registration on failure.*
- **`pitboss_grill.cpp:2371`** — the new code's comment overstates
  confinement: the account password sits in a heap-allocated deferred lambda
  until the main loop runs it (multiple live copies possible in the
  timeout+retry scenario). Never logged/persisted/echoed — exposure is
  minimal — but the comment promises more than is true. *Fix: correct the
  comment; `std::move` into the lambda; clear the captured copy as soon as
  `cloud_login_()` returns.*
- **`esphome/grill-firmware.yaml:112`** — production runs at DEBUG, emitting
  an ESP_LOGI per pushed FE0C frame plus the 10 s status line; the project's
  own incidents blame log volume on the shared 2.4 GHz radio. *Fix: INFO in
  production, DEBUG for bench sessions.*
- **`GrillEvents.razor:90`** — poll swallows every exception, so a failed
  fetch is indistinguishable from an empty log ("No events recorded yet").
  *Fix: separate error state.*
- **`GrillDetail.razor:544`** — `r.error` is nullable; a `{"ok":false}`
  reply with no error string renders zero feedback for a rejected command.
  *Fix: `r.error ?? "Command failed (no error reported)."`*
- **`GrillDetail.razor:344`** — one try/catch wraps all four poll requests;
  after the transient-failure threshold the catch nulls `health` even when
  only the alarms or state route is failing — the page claims the bridge is
  down while `/health` answered fine. *Fix: per-endpoint failure scoping.*
- **`GrillDetail.razor:426`** — timer-alarm countdown formats `hh:mm:ss`
  (wraps at 24 h) with no input cap, and computes against the client's clock
  vs the ESP32-set `fires_at`. *Fix: cap minutes at 1440 or format days;
  consider surfacing clock skew.*
- **`service-worker.published.js:56`** — `skipWaiting()` + immediate
  deletion of every other cache can 404 a tab that is mid-load of the
  previous version. Documented trade-off (a stale worker is worse), but the
  window is closable: keep the immediately-previous generation's cache for
  one activation cycle.

---

## Verified solid (no action)

- **Auth codec** (`pitboss_grill.cpp:161-198`): byte-for-byte equivalent to
  the vendored pytboss 2026.8.6 codec, including the overflow-safe modular
  arithmetic; the `AUTH_KEY_LATENCY_BIAS_S` forward-bias fix is correctly
  reasoned.
- **FE0B/FE0C decode**: every offset, the 960→−1 probe sentinel, the
  ftoc skip-on-sentinel, and length checks match pytboss's PBV2 entry;
  `build_set_temperature_command` provably round-trips onto
  `ACCEPTED_SETPOINTS_F`.
- **The `defer()`+semaphore handoff pattern** everywhere else: deferred
  lambdas capture by value, semaphores drained before reuse, `setup_result_*`
  mutex-guarded on both sides; no deadlock in the new `/setup` split.
- **Cookie signing/verification**: HMAC-SHA256 with
  `hmac.compare_digest`, server-side expiry on every request, fail-closed
  `auth_check` when no users configured.
- **No tunnel path reaches the ESP32 unauthenticated**: every `/api/*`
  location is `auth_request`-gated; unknown `/api/*` falls through to the
  gated SPA location; `/login`,`/logout` proxy to the auth service only.
- **Login flow open-redirect/XSS handling** correct (`_safe_next` rejects
  absolute/protocol-relative; escaping present).
- **`cleanup_grill_events.py`**: fully parameterized SQL, DST-correct hour
  gate, idempotent across fall-back, failures logged not silent.
- **SetupDialog credentials**: Pit Boss password never in a URL/query/log;
  cleared on success and Close; correct input types/autocomplete.
- **Service worker gate/redirect/cache logic** matches its documented
  design (network-first navigations with `cache:'no-cache'`, `response.redirected`
  never cached as shell, `/login`/`/logout` bypass cache).
- **`DOCKER_DEPLOY` gating** consistent; local dev has no gate and no
  `/login`. Culture parsing safe (int-only input, doubles only in display).

---

## Recommended order of attack

1. **P0-3** — LAN auth on the ESP32 (it's a fire; smallest change, biggest
   risk reduction).
2. **P0-1** — stale-command re-send helper (safety; firmware commit + flash
   + on-device verification with a harmless `set_temp`).
3. **P0-2** — one-line `IsSuccessStatusCode` fix + service-worker sibling
   (restores the app's most important diagnostic).
4. **`/setup` generation guard + move the grill-list JSON off the httpd
   task** — code added 2026-10-20, fix while fresh.
5. The P1 firmware concurrency items (defer the ping, `command_result_error_`
   locking, notify-failure recovery) as one firmware-hardening commit.
6. Client P1s (poll guard, alarm try/catch) as one app commit.
7. Host-side P1s (login logging + rate limit, XFF, index.html no-cache,
   AUTH_USERS validation) as one deployment commit.
8. P2 items opportunistically.
