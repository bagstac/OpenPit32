using Microsoft.AspNetCore.Components.Web;
using Microsoft.AspNetCore.Components.WebAssembly.Hosting;
using OpenPit32;
using OpenPit32.Services;

var builder = WebAssemblyHostBuilder.CreateDefault(args);
builder.RootComponents.Add<App>("#app");
builder.RootComponents.Add<HeadOutlet>("head::after");

// The only backend is the sidecar (scripts/grill_sidecar.py), which holds
// the Bluetooth session and the grill password. Where it's reachable
// depends on how this page is served:
//   - Docker (docker/web.Dockerfile writes wwwroot/appsettings.json with
//     "SidecarBaseUrl": "/api/"): nginx reverse-proxies /api/ to the sidecar
//     container on the same origin — needed so a single hostname (and a
//     single Cloudflare Tunnel / Basic Auth challenge) covers both, and
//     avoids CORS entirely.
//   - Local dev (`dotnet run`, no appsettings.json): falls back to the
//     sidecar's fixed port on the page's own host, e.g. http://localhost:8091
//     — cross-origin, so the sidecar's CORS allowlist (GRILL_SIDECAR_ORIGINS)
//     must include this origin.
var configuredBase = builder.Configuration["SidecarBaseUrl"];
var sidecarBase = string.IsNullOrWhiteSpace(configuredBase)
    ? new Uri($"http://{new Uri(builder.HostEnvironment.BaseAddress).Host}:8091")
    : new Uri(new Uri(builder.HostEnvironment.BaseAddress), configuredBase);
builder.Services.AddTransient<IncludeCredentialsHandler>();
builder.Services.AddHttpClient<GrillRpcService>(client => client.BaseAddress = sidecarBase)
    .AddHttpMessageHandler<IncludeCredentialsHandler>();

await builder.Build().RunAsync();
