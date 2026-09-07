// Web Push handling shared by both service workers (service-worker.js for
// `dotnet run`, service-worker.published.js for the Docker/publish build —
// see OpenPit32.csproj's <ServiceWorker> item). Alarms fire from the sidecar
// (scripts/alarms.py) regardless of which one is active, so both import this
// file rather than duplicating the listeners.
//
// The payload is JSON written by AlarmStore._send_push: {title, body, tag}.

self.addEventListener('push', event => event.waitUntil(showAlarmNotification(event)));
self.addEventListener('notificationclick', event => event.waitUntil(focusGrillPage(event)));

async function showAlarmNotification(event) {
    let payload = { title: 'OpenPit32', body: 'An alarm was triggered.', tag: 'openpit32-alarm' };
    if (event.data) {
        try {
            payload = { ...payload, ...event.data.json() };
        } catch {
            payload = { ...payload, body: event.data.text() };
        }
    }
    await self.registration.showNotification(payload.title, {
        body: payload.body,
        tag: payload.tag,
        icon: '/icon-192.png',
        badge: '/icon-192.png',
    });
}

async function focusGrillPage(event) {
    event.notification.close();
    const windows = await clients.matchAll({ type: 'window', includeUncontrolled: true });
    for (const client of windows) {
        if ('focus' in client) return client.focus();
    }
    if (clients.openWindow) return clients.openWindow('/grill');
}
