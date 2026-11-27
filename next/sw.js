// Offline copy of the hosted configurator: on the board's hotspot there is
// no internet, so the app files come from this cache. Requests to the board
// (http://10.0.0.1/...) are never cached or touched.
//
// The deploy build (configurator/build_site.py) puts the commit in the cache name, so every deploy
// gets a new cache and old ones are dropped.
const CACHE = "rkfpv-69a2e31e7c42";
// The deploy workflow replaces the list with every file of the site (the
// vendored ADB modules too); this one is what a plain copy of web/ needs.
const FILES = [
  "./",
  "app.js",
  "board.js",
  "index.html",
  "manifest.webmanifest",
  "md5.js",
  "presets.json",
  "preview.js",
  "style.css",
  "usb.js",
  "icons/icon-192.png",
  "icons/icon-512.png",
  "icons/icon-maskable-512.png",
  "icons/icon.svg",
  "vendor/VERSIONS",
  "vendor/@yume-chan/adb-credential-web/esm/index.js",
  "vendor/@yume-chan/adb-daemon-webusb/esm/device.js",
  "vendor/@yume-chan/adb-daemon-webusb/esm/error.js",
  "vendor/@yume-chan/adb-daemon-webusb/esm/index.js",
  "vendor/@yume-chan/adb-daemon-webusb/esm/manager.js",
  "vendor/@yume-chan/adb-daemon-webusb/esm/observer.js",
  "vendor/@yume-chan/adb-daemon-webusb/esm/utils.js",
  "vendor/@yume-chan/adb/esm/adb.js",
  "vendor/@yume-chan/adb/esm/banner.js",
  "vendor/@yume-chan/adb/esm/device-observer.js",
  "vendor/@yume-chan/adb/esm/features.js",
  "vendor/@yume-chan/adb/esm/index.js",
  "vendor/@yume-chan/adb/esm/commands/base.js",
  "vendor/@yume-chan/adb/esm/commands/framebuffer.js",
  "vendor/@yume-chan/adb/esm/commands/index.js",
  "vendor/@yume-chan/adb/esm/commands/power.js",
  "vendor/@yume-chan/adb/esm/commands/reverse.js",
  "vendor/@yume-chan/adb/esm/commands/tcpip.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/index.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/pty.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/service.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/utils.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/none/index.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/none/process.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/none/pty.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/none/service.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/none/spawner.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/shell/index.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/shell/process.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/shell/pty.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/shell/service.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/shell/shared.js",
  "vendor/@yume-chan/adb/esm/commands/subprocess/shell/spawner.js",
  "vendor/@yume-chan/adb/esm/commands/sync/index.js",
  "vendor/@yume-chan/adb/esm/commands/sync/list.js",
  "vendor/@yume-chan/adb/esm/commands/sync/pull.js",
  "vendor/@yume-chan/adb/esm/commands/sync/push.js",
  "vendor/@yume-chan/adb/esm/commands/sync/request.js",
  "vendor/@yume-chan/adb/esm/commands/sync/response.js",
  "vendor/@yume-chan/adb/esm/commands/sync/socket.js",
  "vendor/@yume-chan/adb/esm/commands/sync/stat.js",
  "vendor/@yume-chan/adb/esm/commands/sync/sync.js",
  "vendor/@yume-chan/adb/esm/daemon/auth.js",
  "vendor/@yume-chan/adb/esm/daemon/crypto.js",
  "vendor/@yume-chan/adb/esm/daemon/device.js",
  "vendor/@yume-chan/adb/esm/daemon/dispatcher.js",
  "vendor/@yume-chan/adb/esm/daemon/index.js",
  "vendor/@yume-chan/adb/esm/daemon/packet.js",
  "vendor/@yume-chan/adb/esm/daemon/socket.js",
  "vendor/@yume-chan/adb/esm/daemon/transport.js",
  "vendor/@yume-chan/adb/esm/server/client.js",
  "vendor/@yume-chan/adb/esm/server/index.js",
  "vendor/@yume-chan/adb/esm/server/observer.js",
  "vendor/@yume-chan/adb/esm/server/stream.js",
  "vendor/@yume-chan/adb/esm/server/transport.js",
  "vendor/@yume-chan/adb/esm/server/commands/index.js",
  "vendor/@yume-chan/adb/esm/server/commands/m-dns.js",
  "vendor/@yume-chan/adb/esm/server/commands/wireless.js",
  "vendor/@yume-chan/adb/esm/utils/array-buffer.js",
  "vendor/@yume-chan/adb/esm/utils/auto-reset-event.js",
  "vendor/@yume-chan/adb/esm/utils/base64.js",
  "vendor/@yume-chan/adb/esm/utils/hex.js",
  "vendor/@yume-chan/adb/esm/utils/index.js",
  "vendor/@yume-chan/adb/esm/utils/no-op.js",
  "vendor/@yume-chan/adb/esm/utils/ref.js",
  "vendor/@yume-chan/adb/esm/utils/sequence-equal.js",
  "vendor/@yume-chan/async/esm/async-operation-manager.js",
  "vendor/@yume-chan/async/esm/delay.js",
  "vendor/@yume-chan/async/esm/index.js",
  "vendor/@yume-chan/async/esm/maybe-promise.js",
  "vendor/@yume-chan/async/esm/no-op.js",
  "vendor/@yume-chan/async/esm/promise-resolver.js",
  "vendor/@yume-chan/async/esm/suppress-unhandled-rejection.js",
  "vendor/@yume-chan/event/esm/disposable.js",
  "vendor/@yume-chan/event/esm/event-emitter.js",
  "vendor/@yume-chan/event/esm/event.js",
  "vendor/@yume-chan/event/esm/index.js",
  "vendor/@yume-chan/event/esm/sticky-event-emitter.js",
  "vendor/@yume-chan/event/esm/utils.js",
  "vendor/@yume-chan/no-data-view/esm/index.js",
  "vendor/@yume-chan/no-data-view/esm/int16.js",
  "vendor/@yume-chan/no-data-view/esm/int32.js",
  "vendor/@yume-chan/no-data-view/esm/int64.js",
  "vendor/@yume-chan/no-data-view/esm/int8.js",
  "vendor/@yume-chan/no-data-view/esm/uint16.js",
  "vendor/@yume-chan/no-data-view/esm/uint32.js",
  "vendor/@yume-chan/no-data-view/esm/uint64.js",
  "vendor/@yume-chan/stream-extra/esm/buffered-transform.js",
  "vendor/@yume-chan/stream-extra/esm/buffered.js",
  "vendor/@yume-chan/stream-extra/esm/concat.js",
  "vendor/@yume-chan/stream-extra/esm/consumable.js",
  "vendor/@yume-chan/stream-extra/esm/distribution.js",
  "vendor/@yume-chan/stream-extra/esm/duplex.js",
  "vendor/@yume-chan/stream-extra/esm/encoding.js",
  "vendor/@yume-chan/stream-extra/esm/index.js",
  "vendor/@yume-chan/stream-extra/esm/inspect.js",
  "vendor/@yume-chan/stream-extra/esm/maybe-consumable.js",
  "vendor/@yume-chan/stream-extra/esm/pipe-from.js",
  "vendor/@yume-chan/stream-extra/esm/push-readable.js",
  "vendor/@yume-chan/stream-extra/esm/split-string.js",
  "vendor/@yume-chan/stream-extra/esm/stream.js",
  "vendor/@yume-chan/stream-extra/esm/struct-deserialize.js",
  "vendor/@yume-chan/stream-extra/esm/struct-serialize.js",
  "vendor/@yume-chan/stream-extra/esm/task-queue.js",
  "vendor/@yume-chan/stream-extra/esm/task.js",
  "vendor/@yume-chan/stream-extra/esm/try-close.js",
  "vendor/@yume-chan/stream-extra/esm/types.js",
  "vendor/@yume-chan/stream-extra/esm/wrap-readable.js",
  "vendor/@yume-chan/stream-extra/esm/wrap-writable.js",
  "vendor/@yume-chan/stream-extra/esm/consumable/index.js",
  "vendor/@yume-chan/stream-extra/esm/consumable/readable.js",
  "vendor/@yume-chan/stream-extra/esm/consumable/wrap-byte-readable.js",
  "vendor/@yume-chan/stream-extra/esm/consumable/wrap-writable.js",
  "vendor/@yume-chan/stream-extra/esm/consumable/writable.js",
  "vendor/@yume-chan/stream-extra/esm/maybe-consumable/index.js",
  "vendor/@yume-chan/stream-extra/esm/maybe-consumable/utils.js",
  "vendor/@yume-chan/stream-extra/esm/maybe-consumable/wrap-writable.js",
  "vendor/@yume-chan/stream-extra/esm/maybe-consumable/writable.js",
  "vendor/@yume-chan/struct/esm/bipedal.js",
  "vendor/@yume-chan/struct/esm/buffer.js",
  "vendor/@yume-chan/struct/esm/concat.js",
  "vendor/@yume-chan/struct/esm/extend.js",
  "vendor/@yume-chan/struct/esm/index.js",
  "vendor/@yume-chan/struct/esm/number.js",
  "vendor/@yume-chan/struct/esm/readable.js",
  "vendor/@yume-chan/struct/esm/string.js",
  "vendor/@yume-chan/struct/esm/struct.js",
  "vendor/@yume-chan/struct/esm/types.js",
  "vendor/@yume-chan/struct/esm/utils.js",
  "vendor/@yume-chan/struct/esm/field/factory.js",
  "vendor/@yume-chan/struct/esm/field/index.js",
  "vendor/@yume-chan/struct/esm/field/serialize.js",
  "vendor/@yume-chan/struct/esm/field/types.js",
];

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
