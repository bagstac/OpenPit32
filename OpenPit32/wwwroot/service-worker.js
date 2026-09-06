// In development, always fetch from the network and do not enable offline
// support. Caching here would make development more confusing (changes
// wouldn't be reflected on the first load after each rebuild). The real
// worker for published/Docker builds is service-worker.published.js.
self.addEventListener('fetch', () => { });
