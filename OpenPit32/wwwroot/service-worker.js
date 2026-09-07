// In development, always fetch from the network and do not enable offline
// support. Caching here would make development more confusing (changes
// wouldn't be reflected on the first load after each rebuild). The real
// worker for published/Docker builds is service-worker.published.js.
self.addEventListener('fetch', () => { });

// Same reasoning as service-worker.published.js's onInstall/onActivate:
// take over immediately instead of waiting for every tab to close.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));

// Push notifications (grill alarms) work the same in dev as in production —
// only the offline asset cache above is dev-specific — so both workers share
// this handling. See push-worker.js.
self.importScripts('./push-worker.js');
