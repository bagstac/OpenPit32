using System.Net.Http.Json;
using System.Text.Json;

namespace OpenPit32.Services;

/// <summary>Decoded grill state served by scripts/grill_sidecar.py (/state).</summary>
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

public class SidecarModelsResponse
{
    public string? control_board { get; set; }
    public List<string> models { get; set; } = new();
    public string? @default { get; set; }
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

/// <summary>An alarm as stored by scripts/alarms.py — either a "temp" alarm
/// (sensor/comparison/target set) or a "timer" alarm (duration_seconds/fires_at
/// set). Dropped from the list server-side once it fires.</summary>
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

public class VapidKeyResponse
{
    public string publicKey { get; set; } = "";
}

/// <summary>
/// Thin client for the local grill sidecar (http://127.0.0.1:8091), which
/// holds the Bluetooth session and the grill password. This app never sees
/// the password; the only cloud interaction is <see cref="SetupAsync"/>, a
/// one-time fetch of that password from the user's Pit Boss account, and the
/// account credentials are passed straight through to the sidecar.
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

    /// <summary>Null while the sidecar is not set up (it answers 404).</summary>
    public async Task<SidecarInfoResponse?> GetInfoAsync()
    {
        var resp = await _http.GetAsync("info");
        if (!resp.IsSuccessStatusCode) return null;
        return await resp.Content.ReadFromJsonAsync<SidecarInfoResponse>();
    }

    public Task<SidecarModelsResponse?> GetModelsAsync() =>
        _http.GetFromJsonAsync<SidecarModelsResponse>("models");

    public async Task<SidecarCommandResponse> SendCommandAsync(
        string action, double? value = null, int? probe = null, bool confirm = false)
    {
        var body = new Dictionary<string, object?> { ["action"] = action };
        if (value is not null) body["value"] = value;
        if (probe is not null) body["probe"] = probe;
        if (confirm) body["confirm"] = true;

        var resp = await _http.PostAsJsonAsync("command", body);
        return await resp.Content.ReadFromJsonAsync<SidecarCommandResponse>()
               ?? new SidecarCommandResponse { ok = false, error = "Empty reply from sidecar" };
    }

    /// <summary>
    /// ESP32-only setting (POST /config): how many consecutive PB.GetState
    /// rejections are required before GrillDetail.razor shows a link-problem
    /// warning, instead of flashing one on the first, usually self-healing,
    /// occurrence. No-op against the sidecar (no such route there).
    /// </summary>
    public async Task<SidecarConfigResponse> SetErrorDisplayThresholdAsync(int threshold)
    {
        var resp = await _http.PostAsJsonAsync("config", new { error_display_threshold = threshold });
        return await resp.Content.ReadFromJsonAsync<SidecarConfigResponse>()
               ?? new SidecarConfigResponse { ok = false, error = "Empty reply from grill bridge" };
    }

    /// <summary>
    /// Ask the sidecar to fetch the grill password from the Pit Boss account.
    /// A 409 reply carries <c>grills</c> to choose from; resend with grillId.
    /// </summary>
    public async Task<SidecarSetupResponse> SetupAsync(
        string email, string password, string country, string? model, int? grillId)
    {
        var body = new Dictionary<string, object?>
        {
            ["email"] = email,
            ["password"] = password,
            ["country"] = country,
        };
        if (!string.IsNullOrWhiteSpace(model)) body["model"] = model;
        if (grillId is not null) body["grill_id"] = grillId;

        var resp = await _http.PostAsJsonAsync("setup", body);
        return await resp.Content.ReadFromJsonAsync<SidecarSetupResponse>()
               ?? new SidecarSetupResponse { ok = false, error = "Empty reply from sidecar" };
    }

    // ---- Alarms & push (scripts/alarms.py) ----

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
               ?? new AlarmResponse { ok = false, error = "Empty reply from sidecar" };
    }

    public async Task<bool> DeleteAlarmAsync(string id) =>
        (await _http.DeleteAsync($"alarms/{Uri.EscapeDataString(id)}")).IsSuccessStatusCode;

    /// <summary>Null if the sidecar is unreachable (no key yet to subscribe with).</summary>
    public async Task<string?> GetVapidPublicKeyAsync()
    {
        var resp = await _http.GetFromJsonAsync<VapidKeyResponse>("push/vapid-public-key");
        return resp?.publicKey;
    }

    /// <summary>Registers a browser's PushSubscription.toJSON() with the sidecar.</summary>
    public async Task<bool> SubscribePushAsync(JsonElement subscription) =>
        (await _http.PostAsJsonAsync("push/subscribe", subscription)).IsSuccessStatusCode;

    public async Task<bool> UnsubscribePushAsync(string endpoint) =>
        (await _http.PostAsJsonAsync("push/unsubscribe", new { endpoint })).IsSuccessStatusCode;
}
