using Microsoft.AspNetCore.Components.Web;
using Microsoft.AspNetCore.Components.WebAssembly.Hosting;
using OpenPit32;
using OpenPit32.Services;

var builder = WebAssemblyHostBuilder.CreateDefault(args);
builder.RootComponents.Add<App>("#app");
builder.RootComponents.Add<HeadOutlet>("head::after");

// The backend is split as of Phase 8 (docs/ESP32_FIRMWARE_PLAN.md): the
// grill's own ESP32 (esphome/grill-firmware.yaml) for every grill/alarm
// route GrillRpcService calls, and a small login-only service
// (scripts/login_service.py) for /login, /logout, /auth-check — neither of
// which GrillRpcService ever calls itself; the browser navigates to those
// directly, and only behind nginx (see below). Where the ESP32 is reachable
// depends on how this page is served:
//   - Docker (docker/web.Dockerfile passes -p:DefineConstants=DOCKER_DEPLOY
//     to `dotnet publish`): nginx reverse-proxies /api/ on the same
//     origin, fanning each route out to the ESP32 or the login service
//     itself (docker/nginx.conf.template) — needed so a single hostname
//     (and a single Cloudflare Tunnel) covers both, and avoids CORS
//     entirely.
//   - Local dev (`dotnet run`, no such constant): no nginx, so no login
//     gate either (that's Docker/nginx-specific — see docker/README.md) —
//     GrillRpcService just talks to the grill's ESP32 directly. Set
//     GRILL_HOST below to your own grill's LAN IP.
//
// Deliberately a compile-time switch, not something read at startup (a
// fetched wwwroot/appsettings.json, or WebAssemblyHostBuilder's
// environment name): behind nginx's login gate, the page's own top-level
// navigation and the core framework files (referenced directly from
// index.html in .NET 10 — there's no separate blazor.boot.json fetch
// anymore) carry the browser's cached login fine, but a run-time check
// still depends on trusting some other WASM-runtime-internal fetch or
// environment-detection path to behave the same way, and one such attempt
// already didn't. Baking the choice in at publish time has no such risk.
#if DOCKER_DEPLOY
var apiBase = new Uri(new Uri(builder.HostEnvironment.BaseAddress), "/api/");
#else
const string GRILL_HOST = "192.168.1.114"; // <- your grill's ESP32 LAN IP
var apiBase = new Uri($"http://{GRILL_HOST}/");
#endif
builder.Services.AddTransient<IncludeCredentialsHandler>();
builder.Services.AddHttpClient<GrillRpcService>(client => client.BaseAddress = apiBase)
    .AddHttpMessageHandler<IncludeCredentialsHandler>();

await builder.Build().RunAsync();
