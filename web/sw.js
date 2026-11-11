// Service worker: makes the console installable and usable offline.
// Same-origin GETs: network first (so an online visit always gets the latest page), falling back
// to the cache after NET_TIMEOUT_MS or on error. Everything else (boards on 192.168.7x.1, the
// firmware URL, ...) is not touched and goes straight to the network.
const VERSION = 'dev';  // replaced with the commit SHA by .github/workflows/pages.yml
const CACHE = 'kb2040-' + VERSION;
const NET_TIMEOUT_MS = 4000;
const SHELL = [
  './',
  'manifest.webmanifest',
  'icon-192.png',
  'icon-512.png',
  'icon-maskable-512.png',
  'apple-touch-icon.png',
  'vendor/web-serial-polyfill.js',
];
// Published firmware (CI build): cached for offline flashing, but optional, so the worker still
// installs where it is missing (python3 serve.py, forks without the CI build).
const OPTIONAL = ['firmware/firmware.json', 'firmware/firmware.bin'];

self.addEventListener('install', (e) => {
  e.waitUntil((async () => {
    const c = await caches.open(CACHE);
    await c.addAll(SHELL);
    await Promise.allSettled(OPTIONAL.map((u) => c.add(new Request(u, { cache: 'no-store' }))));
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (e) => {
  e.waitUntil((async () => {
    for (const k of await caches.keys()) if (k.startsWith('kb2040-') && k !== CACHE) await caches.delete(k);
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', (e) => {
  const req = e.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== self.location.origin) return;
  e.respondWith(networkFirst(req));
});

async function networkFirst(req) {
  const cache = await caches.open(CACHE);
  try {
    const res = await Promise.race([
      fetch(req),
      new Promise((_, reject) => setTimeout(() => reject(new Error('timeout')), NET_TIMEOUT_MS)),
    ]);
    if (res.ok) cache.put(req.mode === 'navigate' ? './' : req, res.clone());
    return res;
  } catch (err) {
    const hit = await cache.match(req.mode === 'navigate' ? './' : req, { ignoreSearch: true });
    if (hit) return hit;
    throw err;
  }
}
