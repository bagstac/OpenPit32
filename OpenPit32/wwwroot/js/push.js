// JS interop for the grill page's Alarms card (OpenPit32/Pages/GrillDetail.razor).
// Loaded as an ES module via IJSRuntime.InvokeAsync<IJSObjectReference>("import", ...).
//
// Push itself is delivered and shown entirely by the service worker's own
// 'push' listener (wwwroot/push-worker.js) — this module only handles the
// one-time setup: asking for Notification permission and registering a
// PushSubscription with the browser, keyed to the sidecar's VAPID public key
// (GET /push/vapid-public-key) so the sidecar (scripts/alarms.py) is the only
// party that can address these subscriptions.

export function isSupported() {
    return 'serviceWorker' in navigator && 'PushManager' in window
        && typeof Notification !== 'undefined';
}

export function permission() {
    return typeof Notification !== 'undefined' ? Notification.permission : 'unsupported';
}

export async function requestPermission() {
    if (typeof Notification === 'undefined') return 'unsupported';
    return await Notification.requestPermission();
}

// The browser's applicationServerKey wants raw bytes, not the base64url text
// the sidecar hands back — same conversion every Web Push tutorial does.
function urlBase64ToUint8Array(base64Url) {
    const padding = '='.repeat((4 - (base64Url.length % 4)) % 4);
    const base64 = (base64Url + padding).replace(/-/g, '+').replace(/_/g, '/');
    const raw = atob(base64);
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
    return bytes;
}

export async function subscribe(vapidPublicKey) {
    const registration = await navigator.serviceWorker.ready;
    let sub = await registration.pushManager.getSubscription();
    if (!sub) {
        sub = await registration.pushManager.subscribe({
            userVisibleOnly: true,
            applicationServerKey: urlBase64ToUint8Array(vapidPublicKey),
        });
    }
    return sub.toJSON();
}

export async function getExistingSubscription() {
    if (!('serviceWorker' in navigator)) return null;
    const registration = await navigator.serviceWorker.getRegistration();
    if (!registration) return null;
    const sub = await registration.pushManager.getSubscription();
    return sub ? sub.toJSON() : null;
}

export async function unsubscribe() {
    if (!('serviceWorker' in navigator)) return null;
    const registration = await navigator.serviceWorker.getRegistration();
    if (!registration) return null;
    const sub = await registration.pushManager.getSubscription();
    if (!sub) return null;
    const endpoint = sub.endpoint;
    await sub.unsubscribe();
    return endpoint;
}
