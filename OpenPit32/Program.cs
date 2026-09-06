using Microsoft.AspNetCore.Components.Web;
using Microsoft.AspNetCore.Components.WebAssembly.Hosting;
using OpenPit32;
using OpenPit32.Services;

var builder = WebAssemblyHostBuilder.CreateDefault(args);
builder.RootComponents.Add<App>("#app");
builder.RootComponents.Add<HeadOutlet>("head::after");

// The only backend is the sidecar (scripts/grill_sidecar.py), which holds
// the Bluetooth session and the grill password. It always runs on the same
// host as this page — 127.0.0.1 for local dev, a LAN host once deployed
// (docker-compose.yml runs both containers on one host) — just a fixed
// different port, so the sidecar address is derived from the page's own
// host rather than hardcoded. Cross-origin because the ports differ; the
// sidecar's CORS allowlist (GRILL_SIDECAR_ORIGINS) must include this origin.
var pageHost = new Uri(builder.HostEnvironment.BaseAddress).Host;
builder.Services.AddHttpClient<GrillRpcService>(client =>
    client.BaseAddress = new Uri($"http://{pageHost}:8091"));

await builder.Build().RunAsync();
