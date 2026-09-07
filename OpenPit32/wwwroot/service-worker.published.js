// Caches the app shell (the compiled Blazor runtime, static assets) listed
// in service-worker-assets.js — generated at publish time by the Blazor
// SDK from ServiceWorkerAssetsManifest in OpenPit32.csproj — so the app
// still opens offline. Grill data itself is never precached or served
// stale: /api/* is proxied by nginx to a separate container, not a static
// wwwroot file, so it's never part of the assets manifest onFetch below
// checks against, and always falls through to a real network request.
self.importScripts('./service-worker-assets.js');
// Push notifications (grill alarms) — shared with the dev worker, see its comment.
self.importScripts('./push-worker.js');

self.addEventListener('install', event => event.waitUntil(onInstall(event)));
self.addEventListener('activate', event => event.waitUntil(onActivate(event)));
self.addEventListener('fetch', event => event.respondWith(onFetch(event)));

const cacheNamePrefix = 'offline-cache-';
const cacheName = `${cacheNamePrefix}${self.assetsManifest.version}`;
const offlineAssetsInclude = [/\.dll$/, /\.pdb$/, /\.wasm/, /\.html/, /\.js$/, /\.json$/, /\.css$/, /\.woff$/, /\.png$/, /\.jpe?g$/, /\.gif$/, /\.ico$/, /\.blat$/, /\.dat$/, /\.svg$/];
const offlineAssetsExclude = [/^service-worker\.js$/];

async function onInstall() {
    console.info('Service worker: install');

    const assetsRequests = self.assetsManifest.assets
        .filter(asset => offlineAssetsInclude.some(pattern => pattern.test(asset.url)))
        .filter(asset => !offlineAssetsExclude.some(pattern => pattern.test(asset.url)))
        .map(asset => new Request(asset.url, { integrity: asset.hash, cache: 'no-cache' }));
    await caches.open(cacheName).then(cache => cache.addAll(assetsRequests));
}

async function onActivate() {
    console.info('Service worker: activate');

    const cacheKeys = await caches.keys();
    await Promise.all(cacheKeys
        .filter(key => key.startsWith(cacheNamePrefix) && key !== cacheName)
        .map(key => caches.delete(key)));
}

async function onFetch(event) {
    let cachedResponse = null;
    if (event.request.method === 'GET') {
        // A client-side route (e.g. /grill) isn't a real file, so serve the
        // app shell for any navigation and let the Blazor router take over —
        // matches nginx's own try_files fallback for the online case.
        const shouldServeIndexHtml = event.request.mode === 'navigate';
        const request = shouldServeIndexHtml ? 'index.html' : event.request;
        const cache = await caches.open(cacheName);
        cachedResponse = await cache.match(request);
    }

    return cachedResponse || fetch(event.request);
}
