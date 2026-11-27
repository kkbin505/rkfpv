// Offline copy of the hosted configurator: on the board's hotspot there is
// no internet, so the app files come from this cache. Requests to the board
// (http://10.0.0.1/...) are never cached or touched.
//
// The deploy build (configurator/build_site.py) puts the commit in the cache name, so every deploy
// gets a new cache and old ones are dropped.
const CACHE = "rkfpv-__VERSION__";
// The deploy workflow replaces the list with every file of the site (the
// vendored ADB modules too); this one is what a plain copy of web/ needs.
const FILES = [/*FILES*/
  "./", "index.html", "style.css", "app.js", "board.js", "preview.js", "usb.js", "md5.js",
  "manifest.webmanifest", "presets.json",
  "icons/icon.svg", "icons/icon-192.png", "icons/icon-512.png", "icons/icon-maskable-512.png",
/*END*/];

self.addEventListener("install", (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(FILES)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", (e) => {
  e.waitUntil(caches.keys()
    .then((keys) => Promise.all(keys.filter((k) => k.startsWith("rkfpv-") && k !== CACHE).map((k) => caches.delete(k))))
    .then(() => self.clients.claim()));
});

// network first (a new deploy shows up when online), cache when offline
self.addEventListener("fetch", (e) => {
  const url = new URL(e.request.url);
  if (e.request.method !== "GET" || url.origin !== self.location.origin) return;
  e.respondWith(
    fetch(e.request)
      .then((r) => {
        if (r.ok) {
          const copy = r.clone();
          caches.open(CACHE).then((c) => c.put(e.request, copy));
        }
        return r;
      })
      .catch(() => caches.match(e.request, { ignoreSearch: true })
        .then((r) => r || caches.match("index.html"))),
  );
});
