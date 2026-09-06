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
//   - Docker (docker/web.Dockerfile passes -p:DefineConstants=DOCKER_DEPLOY
//     to `dotnet publish`): nginx reverse-proxies /api/ to the sidecar
//     container on the same origin — needed so a single hostname (and a
//     single Cloudflare Tunnel / Basic Auth challenge) covers both, and
//     avoids CORS entirely.
//   - Local dev (`dotnet run`, no such constant): falls back to the
//     sidecar's fixed port on the page's own host, e.g.
//     http://localhost:8091 — cross-origin, so the sidecar's CORS
//     allowlist (GRILL_SIDECAR_ORIGINS) must include this origin.
//
// Deliberately a compile-time switch, not something read at startup (a
// fetched wwwroot/appsettings.json, or WebAssemblyHostBuilder's
// environment name): behind nginx's Basic Auth, the page's own top-level
// navigation and the core framework files (referenced directly from
// index.html in .NET 10 — there's no separate blazor.boot.json fetch
// anymore) carry the browser's cached login fine, but a run-time check
// still depends on trusting some other WASM-runtime-internal fetch or
// environment-detection path to behave the same way, and one such attempt
// already didn't. Baking the choice in at publish time has no such risk.
#if DOCKER_DEPLOY
var sidecarBase = new Uri(new Uri(builder.HostEnvironment.BaseAddress), "/api/");
#else
var sidecarBase = new Uri($"http://{new Uri(builder.HostEnvironment.BaseAddress).Host}:8091");
#endif
builder.Services.AddTransient<IncludeCredentialsHandler>();
builder.Services.AddHttpClient<GrillRpcService>(client => client.BaseAddress = sidecarBase)
    .AddHttpMessageHandler<IncludeCredentialsHandler>();

await builder.Build().RunAsync();
