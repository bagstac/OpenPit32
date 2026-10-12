using System.Net.Http.Json;
using System.Text.Json;

namespace OpenPit32.Services;

/// <summary>Decoded grill state, served by GET /state — the ESP32 directly
/// as of Phase 4 (docs/ESP32_FIRMWARE_PLAN.md); type names here keep the
/// "Sidecar" prefix from when scripts/grill_sidecar.py served this same
/// shape, since every Razor page already references them.</summary>
public class SidecarState
{
    public bool moduleIsOn { get; set; }
    public double? grillTemp { get; set; }
    public double? grillSetTemp { get; set; }
    public double? smokerActTemp { get; set; }
    public double? p1Temp { get; set; }
    public double? p2Temp { get; set; }
    public double? p3Temp { get; set; }
    public double? p4Temp { get; set; }
    public bool fanState { get; set; }
    public bool hotState { get; set; }
    public bool motorState { get; set; }
    public bool lightState { get; set; }
    public bool primeState { get; set; }
    public bool isFahrenheit { get; set; }
    public bool noPellets { get; set; }
    public bool err1 { get; set; }
    public bool err2 { get; set; }
    public bool err3 { get; set; }
    public bool highTempErr { get; set; }
    public bool fanErr { get; set; }
    public bool hotErr { get; set; }
    public bool motorErr { get; set; }
    public bool erL { get; set; }
}

public class SidecarStateResponse
{
    public SidecarState? state { get; set; }
    public double? state_age_seconds { get; set; }
    public string? last_error { get; set; }
}

/// <summary>GET /health — link status. `configured` is false until the grill
/// password has been fetched (see <see cref="GrillRpcService.SetupAsync"/>).</summary>
public class SidecarHealthResponse
{
    public bool configured { get; set; }
    public bool connected { get; set; }
    public bool proxy_connected { get; set; }
    public string? proxy_host { get; set; }
    /// <summary>BLE leg: the ESP32's RSSI for the grill (dBm).</summary>
    public int? rssi { get; set; }
    /// <summary>WiFi leg: the ESP32's own WiFi RSSI (dBm).</summary>
    public int? proxy_wifi_rssi { get; set; }
    public long? proxy_uptime_seconds { get; set; }
    public double? state_age_seconds { get; set; }
    public string? last_error { get; set; }
}

public class SidecarInfoResponse
{
    public bool configured { get; set; }
    public string? board_id { get; set; }
    public string? model { get; set; }
    public JsonElement? firmware { get; set; }
    public List<int>? accepted_setpoints_f { get; set; }
    public bool has_lights { get; set; }
    public int meat_probes { get; set; }
    /// <summary>ESP32-only (POST /config) — consecutive PB.GetState rejections
    /// required before the grill link shows a warning. Null when talking to
    /// the sidecar, which has no such setting.</summary>
    public int? error_display_threshold { get; set; }
}

/// <summary>POST /config reply — see SetErrorDisplayThresholdAsync.</summary>
public class SidecarConfigResponse
{
    public bool ok { get; set; }
    public string? error { get; set; }
    public int? error_display_threshold { get; set; }
}

public class SidecarCommandResponse
{
    public bool ok { get; set; }
    public string? error { get; set; }
    public string? action { get; set; }
}

/// <summary>A grill on the Pit Boss account (POST /setup 409 reply) — no password.</summary>
public class SidecarGrillChoice
{
    public int? grill_id { get; set; }
    public string? board_id { get; set; }
    public string? nickname { get; set; }
    public bool has_password { get; set; }
}

public class SidecarSetupResponse
{
    public bool ok { get; set; }
    public string? error { get; set; }
    public List<SidecarGrillChoice>? grills { get; set; }
    public string? board_id { get; set; }
    public string? nickname { get; set; }
    public string? model { get; set; }
    public bool connected { get; set; }
}

/// <summary>An alarm as stored by the grill's ESP32 (pitboss_grill.cpp,
/// NVS-persisted) — either a "temp" alarm (sensor/comparison/target set) or
/// a "timer" alarm (duration_seconds/fires_at set). Dropped from the list
/// server-side once it fires; delivered via Telegram, not browser push.</summary>
public class AlarmDto
{
    public string id { get; set; } = "";
    public string kind { get; set; } = ""; // "temp" | "timer"
    public string label { get; set; } = "";
    public string? sensor { get; set; }
    public string? comparison { get; set; } // "at_or_above" | "at_or_below"
    public double? target { get; set; }
    public int? duration_seconds { get; set; }
    /// <summary>Timer alarms only: unix seconds this fires at.</summary>
    public double? fires_at { get; set; }
    public double created_at { get; set; }
}

public class AlarmsResponse
{
    public List<AlarmDto> alarms { get; set; } = new();
}

public class AlarmResponse
{
    public bool ok { get; set; }
    public string? error { get; set; }
    public AlarmDto? alarm { get; set; }
}

/// <summary>One row from GET /api/events (scripts/login_service.py's
/// POST /log-event, written by the ESP32 — see docker/README.md's "Grill
/// event log" section). <c>event</c> is "command" | "connect" |
/// "disconnect" | "setpoint_jump"; which other fields are set depends on
/// which one it is — e.g. <c>action</c>/<c>value</c>/<c>source_ip</c> for a
/// command, <c>old_value</c>/<c>new_value</c> (inside <c>detail</c>) for a
/// setpoint jump. <c>received_at</c> is this service's own wall-clock (always
/// reliable); <c>ts</c> is the ESP32's, 0/null if it hadn't synced NTP yet.</summary>
public class GrillEventDto
{
    public long received_at { get; set; }
    public double? ts { get; set; }
    /// <summary>"event" is a C# keyword — <c>@event</c> escapes it while
    /// still matching the JSON field name exactly, same as every other
    /// property on this DTO. <see cref="EventType"/> is a plain alias for
    /// Razor markup, where nesting a second <c>@</c> inside an expression
    /// is needless risk for no benefit.</summary>
    public string @event { get; set; } = "";
    public string EventType => @event;
    public string? action { get; set; }
    public double? value { get; set; }
    public double? old_value { get; set; }
    public bool? ok { get; set; }
    public string? error { get; set; }
    public string? source_ip { get; set; }
    public JsonElement? detail { get; set; }
}

public class GrillEventsResponse
{
    public List<GrillEventDto> events { get; set; } = new();
}

/// <summary>
/// Thin client for the grill bridge — as of Phase 8 (docs/ESP32_FIRMWARE_PLAN.md),
/// the grill's own ESP32 for every grill/alarm route, and a small login-only
/// process for /login, /logout, /auth-check (both same-origin behind nginx,
/// hence one shared HttpClient/base address — see docker/nginx.conf.template).
/// This app never sees the grill password; the only cloud interaction is
/// <see cref="SetupAsync"/>, a one-time fetch of that password from the
/// user's Pit Boss account, sent straight through to the ESP32's own
/// POST /setup, which persists it to its own flash.
/// </summary>
public class GrillRpcService
{
    private readonly HttpClient _http;

    public GrillRpcService(HttpClient http) => _http = http;

    /// <summary>Absolute URL of the raw /health JSON, for a link in the UI.</summary>
    public string HealthUrl => new Uri(_http.BaseAddress!, "health").ToString();

    public Task<SidecarStateResponse?> GetStateAsync() =>
        _http.GetFromJsonAsync<SidecarStateResponse>("state");

    public Task<SidecarHealthResponse?> GetHealthAsync() =>
        _http.GetFromJsonAsync<SidecarHealthResponse>("health");

    /// <summary>Null while the grill isn't configured yet (it answers 404).</summary>
    public async Task<SidecarInfoResponse?> GetInfoAsync()
    {
        var resp = await _http.GetAsync("info");
        if (!resp.IsSuccessStatusCode) return null;
        return await resp.Content.ReadFromJsonAsync<SidecarInfoResponse>();
    }

    public async Task<SidecarCommandResponse> SendCommandAsync(
        string action, double? value = null, int? probe = null, bool confirm = false)
    {
        var body = new Dictionary<string, object?> { ["action"] = action };
        if (value is not null) body["value"] = value;
        if (probe is not null) body["probe"] = probe;
        if (confirm) body["confirm"] = true;

        var resp = await _http.PostAsJsonAsync("command", body);
        return await resp.Content.ReadFromJsonAsync<SidecarCommandResponse>()
               ?? new SidecarCommandResponse { ok = false, error = "Empty reply from the grill" };
    }

    /// <summary>
    /// ESP32-only setting (POST /config): how many consecutive PB.GetState
    /// rejections are required before GrillDetail.razor shows a link-problem
    /// warning, instead of flashing one on the first, usually self-healing,
    /// occurrence.
    /// </summary>
    public async Task<SidecarConfigResponse> SetErrorDisplayThresholdAsync(int threshold)
    {
        var resp = await _http.PostAsJsonAsync("config", new { error_display_threshold = threshold });
        return await resp.Content.ReadFromJsonAsync<SidecarConfigResponse>()
               ?? new SidecarConfigResponse { ok = false, error = "Empty reply from grill bridge" };
    }

    /// <summary>
    /// Ask the ESP32 to fetch the grill password from the Pit Boss account
    /// (POST /setup). A 409-shaped reply (still HTTP 200 on this backend —
    /// see pitboss_grill.cpp's handle_setup_()) carries <c>grills</c> to
    /// choose from; resend with grillId. No "model" to send: the ESP32
    /// always reports the model it was compiled/configured for regardless
    /// of what's sent (see that handler's comment) — this project only ever
    /// targets the one grill it's flashed for.
    /// </summary>
    public async Task<SidecarSetupResponse> SetupAsync(
        string email, string password, string country, int? grillId)
    {
        var body = new Dictionary<string, object?>
        {
            ["email"] = email,
            ["password"] = password,
            ["country"] = country,
        };
        if (grillId is not null) body["grill_id"] = grillId;

        var resp = await _http.PostAsJsonAsync("setup", body);
        return await resp.Content.ReadFromJsonAsync<SidecarSetupResponse>()
               ?? new SidecarSetupResponse { ok = false, error = "Empty reply from the grill" };
    }

    // ---- Alarms (ESP32-native as of Phase 6/7; Telegram-delivered, no
    // browser push/VAPID subscription involved at all) ----

    public Task<AlarmsResponse?> GetAlarmsAsync() =>
        _http.GetFromJsonAsync<AlarmsResponse>("alarms");

    public async Task<AlarmResponse> AddTempAlarmAsync(
        string sensor, string comparison, double target, string? label)
    {
        var body = new Dictionary<string, object?>
        {
            ["kind"] = "temp",
            ["sensor"] = sensor,
            ["comparison"] = comparison,
            ["target"] = target,
        };
        if (!string.IsNullOrWhiteSpace(label)) body["label"] = label;
        return await PostAlarmAsync(body);
    }

    public async Task<AlarmResponse> AddTimerAlarmAsync(int durationSeconds, string? label)
    {
        var body = new Dictionary<string, object?>
        {
            ["kind"] = "timer",
            ["duration_seconds"] = durationSeconds,
        };
        if (!string.IsNullOrWhiteSpace(label)) body["label"] = label;
        return await PostAlarmAsync(body);
    }

    private async Task<AlarmResponse> PostAlarmAsync(Dictionary<string, object?> body)
    {
        var resp = await _http.PostAsJsonAsync("alarms", body);
        return await resp.Content.ReadFromJsonAsync<AlarmResponse>()
               ?? new AlarmResponse { ok = false, error = "Empty reply from the grill" };
    }

    public async Task<bool> DeleteAlarmAsync(string id) =>
        (await _http.DeleteAsync($"alarms/{Uri.EscapeDataString(id)}")).IsSuccessStatusCode;

    /// <summary>GET /api/events (docker/README.md's "Grill event log"
    /// section) — the auth service, not the ESP32, same reasoning as
    /// HealthUrl's comment on this being one shared base address. Empty
    /// list, not null/throw, if the feature isn't configured yet (the
    /// backend answers {"events": []} in that case).</summary>
    public async Task<List<GrillEventDto>> GetEventsAsync(int limit = 200)
    {
        var resp = await _http.GetFromJsonAsync<GrillEventsResponse>($"events?limit={limit}");
        return resp?.events ?? new();
    }
}
