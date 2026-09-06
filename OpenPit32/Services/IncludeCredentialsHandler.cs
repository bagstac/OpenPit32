using Microsoft.AspNetCore.Components.WebAssembly.Http;

namespace OpenPit32.Services;

/// <summary>
/// Explicitly includes browser credentials — cookies, and per the Fetch
/// spec's definition of "credentials", a cached HTTP Basic Auth header —
/// on every sidecar request, rather than relying on the WASM HttpClient's
/// default fetch options.
///
/// Needed once nginx sits in front of the app requiring Basic Auth (see
/// docker/nginx.conf): the browser's own top-level navigation to "/"
/// always carries the login the user typed, but the WASM runtime's
/// background HttpClient calls apparently do not reuse it by default,
/// surfacing as every /api/ call coming back 401 even though the page
/// itself loaded fine.
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
