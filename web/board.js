// How the UI reaches the board. The same page runs in two places:
//
//   local    served by configurator/rkfpvconf.py on the PC (it injects
//            window.FPV_TOKEN): ADB, fpvd or SSH through that server, with
//            install, IQ files, shell and the wired preview.
//   hotspot  the hosted web configurator (HTTPS, installable PWA) talking
//            straight to the board's dvrhttpd at http://10.0.0.1 while the
//            phone or PC is on the board's WiFi hotspot. Chrome lets an HTTPS
//            page reach a private address after the "local network" prompt;
//            dvrhttpd answers CORS for the hosted origin only.
//
// Everything else in the UI goes through fpv(), post() and the preview
// stream helpers below.

const TOKEN = window.FPV_TOKEN;
export const kind = TOKEN && TOKEN !== "__TOKEN__" ? "local" : "hotspot";

// what each backend can do; the UI hides the rest
export const caps = kind === "local"
  ? { transports: true, install: true, iqFiles: true, shell: true, ffplay: true, savesOnPc: true }
  : { transports: false, install: false, iqFiles: false, shell: false, ffplay: false, savesOnPc: false };

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

// one `fpv ... --json` on the board
export async function fpv(...args) {
  if (kind === "local") return post("/api/fpv", { args });
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

// start the video on the board; resolves to {ok, error}
export async function previewStart() {
  if (kind === "local") {
    const r = await post("/api/preview/start", {});
    if (r.ok) localPort = r.local_port;
    return r;
  }
  return fpv("preview", "on");
}

export async function previewStop() {
  if (kind === "local") {
    const port = localPort;
    localPort = 0;
    return port ? post("/api/preview/stop", { port }) : { ok: true };
  }
  return fpv("preview", "off");
}

export function previewFetch(signal) {
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
  return kind === "local" || location.protocol !== "https:" || "userAgentData" in navigator;
}
