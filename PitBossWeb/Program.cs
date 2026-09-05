using Microsoft.AspNetCore.Components.Web;
using Microsoft.AspNetCore.Components.WebAssembly.Hosting;
using PitBossWeb;
using PitBossWeb.Services;

var builder = WebAssemblyHostBuilder.CreateDefault(args);
builder.RootComponents.Add<App>("#app");
builder.RootComponents.Add<HeadOutlet>("head::after");

// The only backend is the local sidecar (scripts/grill_sidecar.py), which
// holds the Bluetooth session and the grill password.
builder.Services.AddHttpClient<GrillRpcService>(client =>
    client.BaseAddress = new Uri("http://127.0.0.1:8091"));

await builder.Build().RunAsync();
