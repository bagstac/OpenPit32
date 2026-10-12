// Caches the app shell (the compiled Blazor runtime, static assets) listed
// in service-worker-assets.js — generated at publish time by the Blazor
// SDK from ServiceWorkerAssetsManifest in OpenPit32.csproj — so the app
// still opens offline. Grill data itself is never precached or served
// stale: /api/* is proxied by nginx to a separate container, not a static
// wwwroot file, so it's never part of the assets manifest onFetch below
// checks against, and always falls through to a real network request.
self.importScripts('./service-worker-assets.js');

self.addEventListener('install', event => event.waitUntil(onInstall(event)));
self.addEventListener('activate', event => event.waitUntil(onActivate(event)));
self.addEventListener('fetch', event => event.respondWith(onFetch(event)));

const cacheNamePrefix = 'offline-cache-';
const cacheName = `${cacheNamePrefix}${self.assetsManifest.version}`;
const offlineAssetsInclude = [/\.dll$/, /\.pdb$/, /\.wasm/, /\.html/, /\.js$/, /\.json$/, /\.css$/, /\.woff$/, /\.png$/, /\.jpe?g$/, /\.gif$/, /\.ico$/, /\.blat$/, /\.dat$/, /\.svg$/];
const offlineAssetsExclude = [/^service-worker\.js$/];

// A navigation the login gate bounced cannot be answered with the bounced
// response itself (see onFetch), so hand back a page that walks the browser
// to the real /login. Both mechanisms on purpose: the meta refresh needs no
// JavaScript at all, the script catches a browser that ignores it, and the
// link is there for anyone reading it.
function loginRedirectPage(path) {
    // Only the shape of the requested URL is reused, and anything that could
    // break out of an HTML attribute or a script literal is dropped: this is
    // a URL the caller chose, not something to trust.
    const requested = (path || '/').replace(/[<>"'`\\]/g, '');
    const next = requested.charAt(0) === '/' ? requested : '/';
    const target = '/login?next=' + next;
    return new Response(
        '<!doctype html><html lang="en"><head><meta charset="utf-8">' +
        '<meta http-equiv="refresh" content="0;url=' + target + '">' +
        '<title>OpenPit32 — Log in</title></head><body>' +
        '<p>Your OpenPit32 session has expired. ' +
        '<a href="' + target + '">Log in</a>.</p>' +
        '<script>location.replace(' + JSON.stringify(target) + ');</script>' +
        '</body></html>',
        { status: 200, headers: { 'Content-Type': 'text/html; charset=utf-8' } });
}

async function onInstall() {
    console.info('Service worker: install');

    const assetsRequests = self.assetsManifest.assets
        .filter(asset => offlineAssetsInclude.some(pattern => pattern.test(asset.url)))
        .filter(asset => !offlineAssetsExclude.some(pattern => pattern.test(asset.url)))
        .map(asset => new Request(asset.url, { integrity: asset.hash, cache: 'no-cache' }));
    await caches.open(cacheName).then(cache => cache.addAll(assetsRequests));

    // Take over as soon as this version finishes installing rather than
    // waiting for every open tab to close first (the default) — Blazor
    // fingerprints framework files per publish, so a browser stuck on an old
    // worker after a redeploy ends up fetching files that no longer exist
    // and the app fails to load until someone manually clears site storage.
    self.skipWaiting();
}

async function onActivate() {
    console.info('Service worker: activate');

    const cacheKeys = await caches.keys();
    await Promise.all(cacheKeys
        .filter(key => key.startsWith(cacheNamePrefix) && key !== cacheName)
        .map(key => caches.delete(key)));

    // Pairs with skipWaiting() above: control already-open tabs immediately
    // instead of only new navigations.
    await self.clients.claim();
}

async function onFetch(event) {
    // /login and /logout are real server routes (scripts/login_service.py,
    // proxied by nginx) — not Blazor client-side routes. Serving the cached
    // app shell for these (as every other navigation gets below) means the
    // request never reaches the auth service at all: clicking "Log out"
    // would just re-render the cached SPA instead of ever clearing the
    // session cookie server-side.
    const url = new URL(event.request.url);
    if (url.pathname === '/login' || url.pathname === '/logout') {
        return fetch(event.request);
    }

    // A navigation is a client-side route (e.g. /grill) that isn't a real
    // file, so the app shell stands in for it — matches nginx's own
    // try_files fallback. NETWORK FIRST, with the cached shell only as the
    // offline fallback. The shell is served by a login gate, and an
    // unauthenticated navigation comes back as a redirect to /login (fetch
    // follows it transparently), so serving the cached shell from the cache
    // here would boot a logged-out app whose every /api/* call is redirected
    // too — which is exactly how a lost session used to present itself as an
    // unreachable grill instead of a login form (2026-10-12), with no way
    // back: every navigation re-rendered the same cached shell, so the login
    // page could never be reached from the installed PWA.
    if (event.request.method === 'GET' && event.request.mode === 'navigate') {
        const cache = await caches.open(cacheName);
        try {
            // cache: 'no-cache' is load-bearing: the app shell carries no
            // Cache-Control (nginx:alpine sends none for it), so a plain
            // fetch() can be answered from the browser's own HTTP cache
            // without ever reaching the server — hiding the redirect this
            // whole branch exists to surface.
            const response = await fetch(event.request, { cache: 'no-cache' });
            if (response.ok && !response.redirected) {
                await cache.put('index.html', response.clone());
                return response;
            }
            // Anything else means the login gate bounced this navigation:
            // either the login page (if the redirect was followed) or an
            // opaque redirect. Neither can be handed back as the response to
            // a navigation — the browser rejects a followed redirect's target
            // with ERR_FAILED, measured 2026-10-12 — so answer with a
            // synthetic page that sends the browser to the real /login,
            // which reaches the auth service because /login is excluded from
            // the cache above.
            return loginRedirectPage(url.pathname + url.search);
        } catch (error) {
            const cached = await cache.match('index.html');
            if (cached) return cached;
            return new Response('Offline, and no cached copy of the app shell.', {
                status: 503,
                headers: { 'Content-Type': 'text/plain' }
            });
        }
    }

    let cachedResponse = null;
    if (event.request.method === 'GET') {
        const cache = await caches.open(cacheName);
        cachedResponse = await cache.match(event.request);
    }

    return cachedResponse || fetch(event.request);
}
