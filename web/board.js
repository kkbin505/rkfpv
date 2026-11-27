// How the UI reaches the board. The same page runs in two places, with three
// backends:
//
//   local    served by configurator/rkfpvconf.py on the PC (it injects
//            window.FPV_TOKEN): ADB, fpvd or SSH through that server, with
//            install, IQ files, shell and the wired preview.
//   hotspot  the hosted web configurator (HTTPS, installable PWA) talking
//            straight to the board's dvrhttpd at http://10.0.0.1 while the
//            phone or PC is on the board's WiFi hotspot. Chrome lets an HTTPS
//            page reach a private address after the "local network" prompt;
//            dvrhttpd answers CORS for the hosted origin only.
//   usb      the hosted page with the board on USB: WebUSB ADB straight from
//            the browser (usb.js), so the same things as the PC configurator
//            work without Python or adb: shell, install, IQ files, recordings,
//            the wired preview. Chrome / Edge only.
//
// The hosted page switches between hotspot and usb (setTransport); `kind`
// and `caps` are live bindings, so importers always see the current backend.
//
// Everything else in the UI goes through fpv(), post() and the preview
// stream helpers below.

import * as usb from "./usb.js";
import { md5 } from "./md5.js";

const TOKEN = window.FPV_TOKEN;
export const hosted = !(TOKEN && TOKEN !== "__TOKEN__");

// what each backend can do; the UI hides the rest. savesOnPc: files land in a
// folder the PC configurator can open; saveAs: the browser asks where to save.
const CAPS = {
  local: { transports: true, install: true, iqFiles: true, shell: true, ffplay: true, savesOnPc: true, saveAs: false },
  hotspot: { transports: false, install: false, iqFiles: false, shell: false, ffplay: false, savesOnPc: false, saveAs: false },
  usb: { transports: false, install: true, iqFiles: true, shell: true, ffplay: false, savesOnPc: false, saveAs: true },
};
export let kind = hosted ? (usb.supported() ? "usb" : "hotspot") : "local";
export let caps = CAPS[kind];

// hosted page: "usb" or "hotspot"
export function setTransport(t) {
  if (!hosted) return;
  kind = t === "usb" ? "usb" : "hotspot";
  caps = CAPS[kind];
}
export const usbSupported = usb.supported;

// local backend: the connection fields in the top bar
let connFn = () => ({});
export function setConnection(fn) { connFn = fn; }

// hotspot backend: the board's address
export function boardUrl() {
  const h = (connFn().host || "10.0.0.1").replace(/^https?:\/\//, "").replace(/\/+$/, "");
  return `http://${h}`;
}

// Chrome (Local Network Access): say up front where the request goes. It
// must match the address, or Chrome fails the request: 10.0.0.1 is "local",
// 127.0.0.1 (a test board on this PC) is "loopback".
function addressSpace() {
  const h = boardUrl().slice(7).split(":")[0];
  if (h === "localhost" || /^127\./.test(h)) return { targetAddressSpace: "loopback" };
  if (/^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.|169\.254\.)/.test(h) || h.endsWith(".local")) {
    return { targetAddressSpace: "local" };
  }
  return {};
}
const CONFIRM = { "X-FPV-Confirm": "1" };

function netError(e) {
  return {
    ok: false, transport_error: true,
    error: `board not reachable at ${boardUrl()} (${e.message || e}). ` +
      "Is this device on the board's WiFi hotspot, and was the local-network permission allowed?",
  };
}

// POST to the PC configurator server (local backend only)
export async function post(path, body) {
  if (kind !== "local") return { ok: false, error: "needs the PC configurator (ADB)" };
  const r = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json", "X-Token": TOKEN },
    body: JSON.stringify({ ...connFn(), ...body }),
  });
  return r.json();
}

// ---- usb backend ------------------------------------------------------------

export function usbConnect() { return usb.connect(); }
export function usbDisconnect() { return usb.disconnect(); }
export const usbConnected = usb.connected;
export const usbExplain = usb.explain;
export const usbDeviceName = usb.deviceName;
export function onUsbGone(fn) { usb.onDisconnected(fn); }

function usbError(e) {
  return { ok: false, transport_error: true, error: usb.explain(e) || `USB: ${e?.message || e}` };
}

// same quoting as Python's shlex.quote
const quote = (a) => (/^[A-Za-z0-9_@%+=:,./-]+$/.test(a) ? a : `'${a.replace(/'/g, "'\\''")}'`);

// `fpv ... --json` over a shell, parsed like run_fpv in rkfpvconf.py: the JSON
// is the last line, anything before it is stray output
async function usbFpv(args) {
  let r;
  try {
    r = await usb.shell("fpv " + args.map((a) => quote(String(a))).join(" ") + " --json");
  } catch (e) {
    return usbError(e);
  }
  const text = r.out.trim();
  if (/not found/.test(text) && !text.startsWith("{")) {
    return { ok: false, error: "fpv is not installed on the board", missing_fpv: true };
  }
  for (const line of text.split("\n").reverse()) {
    const l = line.trim();
    if (l.startsWith("{")) {
      try { return JSON.parse(l); } catch { break; }
    }
  }
  return { ok: false, error: (text || `exit ${r.rc}`).slice(0, 2000) };
}

// one `fpv ... --json` on the board
export async function fpv(...args) {
  if (kind === "local") return post("/api/fpv", { args });
  if (kind === "usb") return usbFpv(args);
  try {
    const r = await fetch(`${boardUrl()}/api/fpv`, {
      method: "POST", headers: { ...CONFIRM, "Content-Type": "text/plain" },
      body: args.map(String).join(" "), cache: "no-store", ...addressSpace(),
    });
    return await r.json();
  } catch (e) {
    return netError(e);
  }
}

// `fpv` typed in the CLI tab: a shell line locally, `fpv …` only on the hotspot
export async function cli(line) {
  if (kind === "local") return post("/api/cli", { line });
  if (kind === "usb") {
    if (!line.trim()) return { ok: true, output: "" };
    try {
      const r = await usb.shell(line);
      return { ok: true, rc: r.rc, output: r.out.replace(/\n$/, "") };
    } catch (e) {
      return usbError(e);
    }
  }
  const words = line.trim().split(/\s+/);
  if (words[0] !== "fpv" || words.length < 2) {
    return { ok: true, output: "over the hotspot only `fpv <command>` works, e.g. fpv get" };
  }
  const r = await fpv(...words.slice(1).filter((w) => w !== "--json"));
  if (r.ok && "text" in r) return { ok: true, output: r.text.replace(/\n$/, "") };
  return { ok: true, output: JSON.stringify(r, null, 2) };
}

export async function presets() {
  if (kind === "local") {
    return fetch("/api/presets", { headers: { "X-Token": TOKEN } }).then((x) => x.json());
  }
  // the hosted site ships configurator/presets/*.json as one file (see the Pages workflow)
  try {
    const r = await fetch("presets.json", { cache: "no-cache" });
    return r.ok ? r.json() : { ok: true, presets: [] };
  } catch (e) {
    return { ok: true, presets: [] };
  }
}

// ---- live preview ------------------------------------------------------
// The stream is the same on both: u32 big-endian length + H.265 Annex-B
// access unit, starting at an IDR.

let localPort = 0;
let boardPort = 0;          // usb: rkfpv's preview port on the board

// start the video on the board; resolves to {ok, error}
export async function previewStart() {
  if (kind === "local") {
    const r = await post("/api/preview/start", {});
    if (r.ok) localPort = r.local_port;
    return r;
  }
  const r = await fpv("preview", "on");
  if (kind === "usb" && r.ok) boardPort = +r.port || 5602;
  return r;
}

export async function previewStop() {
  if (kind === "local") {
    const port = localPort;
    localPort = 0;
    return port ? post("/api/preview/stop", { port }) : { ok: true };
  }
  return fpv("preview", "off");
}

export async function previewFetch(signal) {
  if (kind === "usb") {
    // straight to rkfpv's port through adbd: no `adb forward` needed. The
    // open fails while rkfpv is still starting; preview.js retries.
    return { ok: true, body: await usb.openTcp(boardPort || 5602, signal) };
  }
  if (kind === "local") {
    return fetch(`/api/preview/stream?port=${localPort}`, { headers: { "X-Token": TOKEN }, signal });
  }
  return fetch(`${boardUrl()}/live`, { signal, cache: "no-store", ...addressSpace() });
}

export function previewFfplay() {
  return post("/api/preview/ffplay", { port: localPort });
}

// ---- recordings ---------------------------------------------------------

export function dvrFileUrl(name, download) {
  return `${boardUrl()}/dvr/${encodeURIComponent(name)}${download ? "?dl=1" : ""}`;
}

// Is this a browser that can reach the board from an HTTPS page at all?
// Only Chromium has Local Network Access; userAgentData is Chromium-only.
export function hotspotSupported() {
  if (kind === "usb") return usb.supported();
  return kind === "local" || location.protocol !== "https:" || "userAgentData" in navigator;
}

// ---- files: install, IQ, recordings ---------------------------------------
// Locally these go through rkfpvconf.py; over USB the browser does the same
// steps itself. Results have the same shape either way.

// what install() copies; the Pages workflow publishes these under board-files/
const BOARD_FILES = [["fpv", "/usr/bin/fpv", 0o755], ["S99fpv", "/etc/init.d/S99fpv", 0o755],
  ["wfb.sh", "/usr/bin/wfb.sh", 0o755]];
const DEFAULT_FILES = ["rkfpv.conf", "wfb.yaml"];   // only if the image did not bring them

async function boardFile(name) {
  const r = await fetch(`board-files/${name}`, { cache: "no-cache" });
  if (!r.ok) throw new Error(`board-files/${name}: HTTP ${r.status} (this site has no copy of the board scripts)`);
  // LF only: the board's shell cannot run a script with CR in it
  return new TextEncoder().encode((await r.text()).replace(/\r\n/g, "\n"));
}

export async function install() {
  if (kind !== "usb") return post("/api/install", {});
  try {
    const installed = [];
    for (const [name, remote, mode] of BOARD_FILES) {
      await usb.push(remote, await boardFile(name), mode);
      await usb.shell(`chmod ${mode.toString(8)} ${remote}`);
      installed.push(remote);
    }
    await usb.shell("mkdir -p /etc/fpv/defaults");
    for (const name of DEFAULT_FILES) {
      const r = await usb.shell(`[ -f /etc/fpv/defaults/${name} ] && echo yes`);
      if (!r.out.includes("yes")) await usb.push(`/etc/fpv/defaults/${name}`, await boardFile(name));
    }
    await usb.shell("sync");
    return { ok: true, installed };
  } catch (e) {
    return { ok: false, error: usb.explain(e) || String(e?.message || e) };
  }
}

const IQ_STOCK_DIR = "/oem/usr/share/iqfiles";
const IQ_CUSTOM_DIR = "/userdata/iq";
const IQ_NAME = /^[A-Za-z0-9_.-]+\.json$/;
const IQ_KEYS = ["sensor_calib", "module_calib", "main_scene"];   // what rkaiq needs at top level
const IQ_MAX = 8 * 1024 * 1024;

// {ok, name, bytes: Uint8Array}
export async function iqPull(name, where) {
  if (kind !== "usb") {
    const r = await post("/api/iq/pull", { name, where });
    if (r.ok) r.bytes = Uint8Array.from(atob(r.data), (c) => c.charCodeAt(0));
    return r;
  }
  if (!IQ_NAME.test(name) || !["stock", "custom"].includes(where)) return { ok: false, error: `bad IQ file: ${name}` };
  try {
    const bytes = await usb.pull(`${where === "stock" ? IQ_STOCK_DIR : IQ_CUSTOM_DIR}/${name}`);
    return { ok: true, name, size: bytes.length, bytes };
  } catch (e) {
    return { ok: false, error: usb.explain(e) || String(e?.message || e) };
  }
}

// Check an edited IQ JSON, push it next to the custom file and let
// `fpv iq install` verify the md5 and swap it in (iq_push in rkfpvconf.py).
// Nothing on the board changes unless every check passes.
export async function iqPush(name, bytes) {
  if (kind !== "usb") {
    let bin = "";
    for (let i = 0; i < bytes.length; i += 0x8000) bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return post("/api/iq/push", { name, data: btoa(bin) });
  }
  if (!IQ_NAME.test(name)) return { ok: false, error: `not an IQ file name: ${name}` };
  if (bytes.length < 1000 || bytes.length > IQ_MAX) {
    return { ok: false, error: `${name}: ${bytes.length} bytes is not a plausible IQ file` };
  }
  let doc;
  try {
    doc = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes).replace(/^\uFEFF/, ""));
  } catch (e) {
    return { ok: false, error: `${name} is not valid JSON: ${e.message}` };
  }
  const missing = IQ_KEYS.filter((k) => !doc || typeof doc !== "object" || !(k in doc));
  if (missing.length) return { ok: false, error: `${name} is missing ${missing.join(", ")}: not an rkaiq IQ file` };
  const st = await fpv("iq", "list");
  if (!st.ok) return st;
  const stock = (st.stock || []).map((f) => f.name);
  if (!stock.includes(name)) {
    return { ok: false, error: `${name} does not match a stock IQ file name; rkaiq picks the file by name ` +
      `(stock: ${stock.join(", ")})` };
  }
  try {
    await usb.shell(`mkdir -p ${IQ_CUSTOM_DIR}`);
    await usb.push(`${IQ_CUSTOM_DIR}/${name}.new`, bytes);      // byte for byte: CRLF etc. kept
  } catch (e) {
    return usbError(e);
  }
  return fpv("iq", "install", name, md5(bytes));
}

const DVR_BOARD_DIR = "/mnt/dvr";

// Where "Download all" saves over USB: one folder picked once (Chrome, Edge
// on the desktop), else every file is its own browser download.
export async function pickDvrFolder() {
  if (kind !== "usb" || !window.showDirectoryPicker) return null;
  return window.showDirectoryPicker({ id: "rkfpv-dvr", mode: "readwrite" });
}

// Copies one recording to this device. local: rkfpvconf.py saves it in its
// folder. usb: streamed into a file the user picks (or into `folder`), so a
// big recording never sits in memory; browsers without the file picker
// (Android) get an in-memory download instead.
export async function dvrSave(name, { folder = null, onBytes = () => {} } = {}) {
  if (kind !== "usb") return post("/api/dvr/pull", { name });
  if (!/^[A-Za-z0-9_.-]+$/.test(name)) return { ok: false, error: `not a recording name: ${name}` };
  const t0 = performance.now();
  try {
    let handle = null;
    if (folder) handle = await folder.getFileHandle(name, { create: true });
    else if (window.showSaveFilePicker) {
      try {
        handle = await window.showSaveFilePicker({ id: "rkfpv-dvr", suggestedName: name });
      } catch (e) {
        if (e.name === "AbortError") return { ok: false, cancelled: true, error: "cancelled" };
        throw e;
      }
    }
    let size;
    if (handle) {
      size = await usb.pull(`${DVR_BOARD_DIR}/${name}`, await handle.createWritable(), onBytes);
    } else {
      const bytes = await usb.pull(`${DVR_BOARD_DIR}/${name}`, null, onBytes);
      size = bytes.length;
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob([bytes]));
      a.download = name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    }
    const secs = Math.max((performance.now() - t0) / 1000, 0.001);
    return { ok: true, size, seconds: +secs.toFixed(1), mbps: +(size / secs / 1e6).toFixed(1),
      path: handle ? (folder ? `${folder.name}/${name}` : name) : "browser downloads" };
  } catch (e) {
    return { ok: false, error: usb.explain(e) || String(e?.message || e) };
  }
}
