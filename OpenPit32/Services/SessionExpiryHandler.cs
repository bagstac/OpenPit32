using Microsoft.AspNetCore.Components;

namespace OpenPit32.Services;

/// <summary>
/// Turns a lost session into a login prompt instead of a phantom "can't
/// reach the ESP32" error — the failure mode found on 2026-10-12, when the
/// phone's 30-day session cookie ran out and every page showed the grill as
/// unreachable for hours while the ESP32 answered fine.
///
/// The mechanism is nginx's login gate (docker/nginx.conf.template): an
/// unauthenticated /api/* call is answered with a 302 to /login, and fetch()
/// follows redirects transparently, so what reaches
/// <see cref="GrillRpcService"/> is <b>HTTP 200 carrying the login page's
/// HTML</b> rather than grill JSON. IsSuccessStatusCode is true, the JSON
/// parse throws, and the page renders its connectivity error — which blames
/// the one component that was actually healthy.
///
/// The body is the only reliable tell. RequestMessage.RequestUri does NOT
/// work here: in Blazor WebAssembly the redirect is followed by the browser
/// itself, so .NET never sees a 302 and the URI keeps naming the /api/
/// route (measured 2026-10-12 — a URI-based check silently never fires).
/// Every grill route answers JSON, so in a build that has a login gate in
/// front of it, a <b>successful</b> non-JSON reply IS that gate — a failed
/// one is nginx or the ESP32 saying so, and is left to the page's error
/// rendering.
///
/// Navigating with <c>forceLoad</c> deliberately leaves the SPA (a
/// client-side route would just re-render the app) and /login is the one
/// navigation the service worker never serves from its cache
/// (service-worker.published.js), so the browser really does reach the auth
/// service.
///
/// Compiled out of the local-dev build (DOCKER_DEPLOY undefined), which
/// talks to the ESP32 directly: there is no login gate to expire and no
/// /login to navigate to, so a non-JSON reply there means something else.
/// Deliberately not rate-limit-guarded: the pages poll on a timer, so
/// several in-flight requests can notice the same expiry at once, and
/// identical navigations to the same URL are harmless.
/// </summary>
public class SessionExpiryHandler : DelegatingHandler
{
    private readonly NavigationManager _navigation;

    public SessionExpiryHandler(NavigationManager navigation) => _navigation = navigation;

    protected override async Task<HttpResponseMessage> SendAsync(
        HttpRequestMessage request, CancellationToken cancellationToken)
    {
        var response = await base.SendAsync(request, cancellationToken);
        PromptForLoginIfSessionExpired(response);
        return response;
    }

    private void PromptForLoginIfSessionExpired(HttpResponseMessage response)
    {
#if DOCKER_DEPLOY
        // Only a *successful* non-JSON reply is the gate. nginx's own failures
        // are text/html too — 502 when the ESP32 is off the network, 504 on an
        // upstream timeout — and so are the ESP32's own rejections (431 when
        // the request header set overflows its 1 KB httpd buffer). Treating
        // "not JSON" alone as the gate turned every one of those into a login
        // prompt: 2026-11-01, three POST /api/command 431s each bounced the
        // phone to /login mid-cook, and 2026-11-02 a powered-off grill made
        // the app loop login → 502 → login. Those deserve the connectivity
        // error instead, which is what the calling page renders.
        if (!response.IsSuccessStatusCode)
        {
            return;
        }

        if (IsJson(response))
        {
            return;
        }

        // Already on the login page: navigating again would be a redirect
        // loop rather than a prompt.
        var here = _navigation.ToBaseRelativePath(_navigation.Uri);
        if (here.Equals("login", StringComparison.OrdinalIgnoreCase) ||
            here.StartsWith("login?", StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        // next= so logging in returns to where the expiry interrupted, not
        // always to Home. Left unescaped on purpose: the page's own
        // _safe_next() only accepts a value starting with "/", which
        // percent-encoding the first "/" would break.
        _navigation.NavigateTo($"/login?next=/{here}", forceLoad: true);
#endif
    }

    private static bool IsJson(HttpResponseMessage response)
    {
        var mediaType = response.Content.Headers.ContentType?.MediaType;
        return mediaType is not null &&
               mediaType.Contains("json", StringComparison.OrdinalIgnoreCase);
    }
}
