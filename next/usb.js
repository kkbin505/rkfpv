// WebUSB ADB: the hosted page talks to the board's adbd straight from the
// browser (Chrome / Edge, desktop and Android), no adb or Python on the PC.
// Tango ADB (MIT) is vendored in ./vendor, see vendor/VERSIONS.
//
// The board's adbd (buildroot / luckfox) is an old one: no `exec:` and no
// shell protocol v2, so every command goes through `shell:<cmd>`, the
// service desktop `adb shell` uses on such daemons. stdout and stderr arrive
// mixed on one stream; the exit code is echoed as the last line.
import { Adb, AdbDaemonTransport } from "@yume-chan/adb";
import { AdbDaemonWebUsbDeviceManager } from "@yume-chan/adb-daemon-webusb";
import AdbWebCredentialStore from "@yume-chan/adb-credential-web";
import { t } from "./i18n.js";

const manager = AdbDaemonWebUsbDeviceManager.BROWSER;
const credentials = new AdbWebCredentialStore("rkfpv");

let adb = null;
let serial = "";
let queue = Promise.resolve();          // one shell command at a time
let onGone = () => {};

export const supported = () => !!manager && window.isSecureContext;
export const connected = () => !!adb;
export const deviceName = () => serial;
export function onDisconnected(fn) { onGone = fn; }

// Help text for the errors people actually hit (from the prototype page).
export function explain(e) {
  const name = e?.name || "";
  const text = String(e?.message || e);
  if (e?.constructor?.name === "DeviceBusyError" || /already in use/i.test(text) || name === "NetworkError") {
    return t("usb.busy");
  }
  if (name === "SecurityError" || /access denied/i.test(text)) {
    const ua = navigator.userAgent;
    if (/Windows/.test(ua)) {
      return t("usb.windows");
    }
    if (/Linux/.test(ua) && !/Android/.test(ua)) {
      return t("usb.linux") + "\n" +
        "echo 'SUBSYSTEM==\"usb\", ATTR{idVendor}==\"2207\", TAG+=\"uaccess\"' | " +
        "sudo tee /etc/udev/rules.d/70-rkfpv.rules && sudo udevadm control --reload-rules";
    }
    return t("usb.noPermission");
  }
  return null;
}

// Opens the browser's device chooser. Must run from a click.
export async function connect() {
  if (!supported()) throw new Error(t("usb.unsupported"));
  const device = await manager.requestDevice();
  if (!device) return false;                         // chooser cancelled
  const connection = await device.connect();
  const transport = await AdbDaemonTransport.authenticate({
    serial: device.serial, connection, credentialStore: credentials,
  });
  adb = new Adb(transport);
  serial = device.serial;
  queue = Promise.resolve();
  const mine = adb;
  adb.disconnected.then(() => {
    if (adb !== mine) return;                        // we closed it ourselves
    adb = null;
    onGone();
  });
  return true;
}

// Lets go of the device so adb or another tab can use it.
export async function disconnect() {
  const a = adb;
  adb = null;
  if (a) { try { await a.close(); } catch { /* already gone */ } }
}

function need() {
  if (!adb) throw new Error(t("usb.notConnected"));
  return adb;
}

async function readAll(readable) {
  const reader = readable.getReader();
  const dec = new TextDecoder();
  let text = "";
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    text += dec.decode(value, { stream: true });
  }
  return text + dec.decode();
}

// Runs one shell command line: {rc, out} with stdout and stderr together.
export function shell(cmd) {
  const run = async () => {
    const socket = await need().createSocket(`shell:${cmd}\necho __RC=$?`);
    let out = (await readAll(socket.readable)).replace(/\r\n/g, "\n");
    let rc = -1;
    const m = out.match(/__RC=(\d+)\s*$/);
    if (m) { rc = +m[1]; out = out.slice(0, m.index); }
    return { rc, out };
  };
  const p = queue.then(run, run);
  queue = p.catch(() => {});
  return p;
}

// Writes bytes to a file on the board (adb sync, like `adb push`).
export async function push(path, bytes, mode = 0o644) {
  const sync = await need().sync();
  try {
    await sync.write({
      filename: path,
      file: new ReadableStream({ start(c) { c.enqueue(bytes); c.close(); } }),
      permission: mode,
    });
  } finally {
    await sync.dispose();
  }
}

// Streams a file from the board (adb sync, like `adb pull`) into `sink`, a
// WritableStream; or returns the whole file as a Uint8Array when no sink.
export async function pull(path, sink, onBytes = () => {}) {
  const sync = await need().sync();
  try {
    let n = 0;
    const count = new TransformStream({ transform(chunk, c) { n += chunk.length; onBytes(n); c.enqueue(chunk); } });
    const src = sync.read(path).pipeThrough(count);
    if (sink) { await src.pipeTo(sink); return n; }
    const parts = [];
    const reader = src.getReader();
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      parts.push(value);
    }
    const out = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
  } finally {
    await sync.dispose();
  }
}

// A TCP connection to a port on the board (what `adb forward` does), as a
// ReadableStream. adbd refuses the open while nothing listens there.
export async function openTcp(port, signal) {
  const socket = await need().createSocket(`tcp:${port}`);
  if (signal) {
    if (signal.aborted) { await socket.close(); throw new DOMException("aborted", "AbortError"); }
    signal.addEventListener("abort", () => socket.close().catch(() => {}), { once: true });
  }
  return socket.readable;
}
