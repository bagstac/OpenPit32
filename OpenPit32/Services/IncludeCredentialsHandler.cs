using Microsoft.AspNetCore.Components.WebAssembly.Http;

namespace OpenPit32.Services;

/// <summary>
/// Explicitly includes browser credentials — cookies among them — on every
/// GrillRpcService request, rather than relying on the WASM HttpClient's
/// default fetch options.
///
/// Needed once nginx sits in front of the app requiring a login (see
/// docker/nginx.conf.template, scripts/login_service.py's /login): the
/// browser's own top-level navigation to "/" always carries the session
/// cookie, but the WASM runtime's background HttpClient calls apparently do
/// not reuse it by default, surfacing as every /api/ call coming back 401
/// even though the page itself loaded fine. First diagnosed this against
/// Basic Auth before the login moved to a real form + cookie (password
/// managers don't reliably fill Basic Auth's browser-native popup); the fix
/// applies the same way to either. Harmless (a no-op) against the ESP32
/// directly in local dev, which has no cookies/auth of its own to send.
/// </summary>
public class IncludeCredentialsHandler : DelegatingHandler
{
    protected override Task<HttpResponseMessage> SendAsync(
        HttpRequestMessage request, CancellationToken cancellationToken)
    {
        request.SetBrowserRequestCredentials(BrowserRequestCredentials.Include);
        return base.SendAsync(request, cancellationToken);
    }
}
