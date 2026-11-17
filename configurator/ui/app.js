// rkfpv configurator UI. Every board action is an `fpv ... --json` call
// through fpvconf.py (POST /api/fpv).
"use strict";

const $ = (s) => document.querySelector(s);
const $$ = (s) => Array.from(document.querySelectorAll(s));

const state = {
  connected: false,
  schema: [],        // from `fpv schema`
  values: {},        // last values read from the board
  edits: {},         // key -> pending value
  tab: "status",
  log: "rkfpv",
  pollTimer: null,
  txpowerKey: "",    // txpower_<card> of the adapter in use (from status)
};

const APPLY_TAG = {
  venc: ["live", "live"], radio: ["live", "live"], fec: ["live", "live"], txpower: ["live", "live"],
  video: ["restart", "restart"], link: ["link", "link"], boot: ["restart", "next boot"],
};

// ---------------------------------------------------------------- transport

function conn() {
  return {
    transport: $("#transport").value,
    device: $("#device").value,
    host: $("#host").value.trim(),
  };
}

async function post(path, body) {
  const r = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json", "X-Token": window.FPV_TOKEN },
    body: JSON.stringify({ ...conn(), ...body }),
  });
  return r.json();
}

const fpv = (...args) => post("/api/fpv", { args });

// ---------------------------------------------------------------- ui helpers

let toastTimer;
function toast(msg, kind = "", actions = []) {
  const t = $("#toast");
  t.className = kind;
  t.textContent = msg;
  for (const [label, fn] of actions) {
    const b = document.createElement("button");
    b.textContent = label;
    b.className = "primary";
    b.style.marginRight = "6px";
    b.onclick = () => { t.hidden = true; fn(); };
    t.appendChild(document.createElement("br"));
    t.appendChild(b);
  }
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), actions.length ? 15000 : 5000);
}

function setBadge(mode) {
  const b = $("#modeBadge");
  b.className = "badge " + (mode || "off");
  b.textContent = mode ? { fpv: "FPV running", debug: "debug mode", idle: "idle" }[mode] || mode : "disconnected";
}

function showTab(tab) {
  if (!state.connected && tab !== "status") return;
  state.tab = tab;
  $$("#tabs a").forEach((a) => a.classList.toggle("active", a.dataset.tab === tab));
  $("#notConnected").hidden = state.connected;
  $$(".tab").forEach((s) => (s.hidden = true));
  if (!state.connected) return;
  if (tab === "status") { $("#tab-status").hidden = false; refreshStatus(); }
  else if (tab === "logs") { $("#tab-logs").hidden = false; loadLog(); }
  else if (tab === "cli") { $("#tab-cli").hidden = false; $("#cliIn").focus(); }
  else { $("#tab-form").hidden = false; renderForm(tab); }
}

// ---------------------------------------------------------------- connect

async function loadDevices() {
  const r = await fetch("/api/devices", { headers: { "X-Token": window.FPV_TOKEN } }).then((x) => x.json());
  const sel = $("#device");
  const prev = sel.value;
  sel.innerHTML = "";
  if (!r.ok) {
    sel.innerHTML = `<option value="">${r.error}</option>`;
    return;
  }
  if (!r.devices.length) sel.innerHTML = `<option value="">no ADB device</option>`;
  for (const d of r.devices) {
    const o = document.createElement("option");
    o.value = d.serial;
    o.textContent = `${d.serial}${d.model ? " · " + d.model : ""}${d.state !== "device" ? " (" + d.state + ")" : ""}`;
    sel.appendChild(o);
  }
  if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
}

async function connect() {
  if (state.connected) return disconnect();
  $("#connect").disabled = true;
  $("#connect").textContent = "Connecting…";
  try {
    const r = await fpv("schema");
    if (!r.ok) {
      if (r.missing_fpv) {
        toast("fpv is not on the board yet.", "err", [["Install it now", async () => {
          const i = await post("/api/install", {});
          i.ok ? (toast("Installed: " + i.installed.join(", "), "ok"), connect()) : toast(i.error, "err");
        }]]);
      } else toast(r.error, "err");
      return;
    }
    state.schema = r.params;
    state.values = Object.fromEntries(r.params.map((p) => [p.key, p.value]));
    state.edits = {};
    state.connected = true;
    $("#connect").textContent = "Disconnect";
    $("#tabs").classList.remove("disabled");
    showTab(state.tab);
    if (state.tab !== "status") refreshStatus();   // mode badge
    state.pollTimer = setInterval(() => state.tab === "status" && refreshStatus(), 2000);
  } catch (e) {
    toast("Configurator server not reachable: " + e, "err");
  } finally {
    $("#connect").disabled = false;
    if (!state.connected) $("#connect").textContent = "Connect";
  }
}

function disconnect() {
  state.connected = false;
  clearInterval(state.pollTimer);
  $("#connect").textContent = "Connect";
  $("#tabs").classList.add("disabled");
  $("#saveBar").hidden = true;
  setBadge(null);
  showTab("status");
}

// ---------------------------------------------------------------- status

function fmtUptime(s) {
  s = +s;
  if (!s) return "?";
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  return h ? `${h} h ${m} min` : `${m} min ${s % 60} s`;
}

function kv(str) {
  return Object.fromEntries((str || "").trim().split(/\s+/).filter(Boolean).map((x) => x.split("=")));
}

async function refreshStatus() {
  let s;
  try { s = await fpv("status"); } catch { return; }
  if (!s.ok) {
    if (s.transport_error) { toast(s.error, "err"); disconnect(); }
    return;
  }
  setBadge(s.mode);
  if ((s.txpower_key || "") !== state.txpowerKey) {
    state.txpowerKey = s.txpower_key || "";
    if (state.tab === "Radio") renderForm("Radio");   // move the "in use" tag
  }
  $("#stMode").textContent = { fpv: "FPV", debug: "Debug", idle: "Idle" }[s.mode] || s.mode;
  $("#stModeSub").textContent = `USB role: ${s.usb_role || "?"}\nboot override: ${s.override}`;

  if (s.rkfpv_pid) {
    $("#stStream").textContent = s.fps ? `${s.fps} fps · ${(s.kbps / 1000).toFixed(2)} Mbps` : "starting…";
    $("#stStreamSub").textContent = `rkfpv pid ${s.rkfpv_pid}\n${s.last_stats || ""}`;
  } else {
    $("#stStream").textContent = "off";
    $("#stStreamSub").textContent = s.last_stats ? "last run:\n" + s.last_stats : "rkfpv not running";
  }

  if (s.wfb_tx_pid) {
    const r = kv(s.radio), f = kv(s.fec);
    $("#stRadio").textContent = `MCS ${r.mcs_index ?? "?"} · FEC ${f.k ?? "?"}/${f.n ?? "?"}`;
    $("#stRadioSub").textContent = `${s.driver || "?"} · ${r.bandwidth ?? "?"} MHz · STBC ${r.stbc ?? "?"} · LDPC ${r.ldpc ?? "?"}` +
      (s.txpower_key ? `\nTX power ${s.txpower} (${s.txpower_key.replace("txpower_", "")})` +
        (s.txpower_iw_dbm ? `, driver reports ${s.txpower_iw_dbm} dBm` : "") : "");
  } else {
    $("#stRadio").textContent = "off";
    $("#stRadioSub").textContent = s.driver ? `driver ${s.driver} loaded` : "no WiFi adapter driver loaded";
  }

  $("#stBoard").textContent = s.temp_c ? `${s.temp_c} °C` : "–";
  const mem = s.mem_avail_kb ? `${(s.mem_avail_kb / 1024).toFixed(1)} MB free` : "";
  $("#stBoardSub").textContent = `${mem}\nup ${fmtUptime(s.uptime_s)}`;

  $$("#bootMode button").forEach((b) => b.classList.toggle("on", b.dataset.mode === s.override));
}

async function setBootMode(mode) {
  if (mode === "fpv" && !confirm(
    "Always FPV: the board starts the video link at every boot, even with a PC attached.\n\n" +
    "ADB will NOT come up any more. To undo you need SSH over the wfb link or the serial console " +
    "('fpv mode auto').\n\nContinue?")) return;
  const r = await fpv("mode", mode);
  r.ok ? toast(`Boot mode: ${r.mode} (next boot)${r.note ? "\n" + r.note : ""}`, "ok") : toast(r.error, "err");
  refreshStatus();
}

// ---------------------------------------------------------------- settings forms

function control(p, value) {
  const wrap = document.createElement("div");
  const onChange = (v) => {
    if (String(v) === String(state.values[p.key])) delete state.edits[p.key];
    else state.edits[p.key] = String(v);
    wrap.closest(".param").classList.toggle("changed", p.key in state.edits);
    updateSaveBar();
  };

  if (p.type === "bool") {
    wrap.innerHTML = `<label class="switch"><input type="checkbox"><span></span></label>`;
    const cb = wrap.querySelector("input");
    cb.checked = value === "1";
    cb.onchange = () => onChange(cb.checked ? "1" : "0");
  } else if (p.type === "enum") {
    const sel = document.createElement("select");
    for (const c of p.choices) sel.add(new Option(c, c));
    sel.value = value;
    sel.onchange = () => onChange(sel.value);
    wrap.appendChild(sel);
  } else {
    const num = document.createElement("input");
    num.type = "number";
    num.min = p.min;
    num.max = p.max;
    const validate = () => {
      const v = num.value;
      const bad = v === "" || !/^\d+$/.test(v) || +v < p.min || +v > p.max;
      num.classList.toggle("invalid", bad);
      return !bad;
    };
    if (p.default === "auto") {
      wrap.className = "auto";
      wrap.innerHTML = `<label class="switch" title="auto"><input type="checkbox"><span></span></label><small>auto</small>`;
      const cb = wrap.querySelector("input");
      wrap.appendChild(num);
      cb.checked = value === "auto";
      num.disabled = cb.checked;
      num.value = cb.checked ? "" : value;
      cb.onchange = () => {
        num.disabled = cb.checked;
        if (cb.checked) { num.classList.remove("invalid"); onChange("auto"); }
        else { if (!num.value) num.value = p.min; validate() && onChange(num.value); }
      };
      num.oninput = () => validate() ? onChange(num.value) : (state.edits[p.key] = null, updateSaveBar());
    } else {
      num.value = value;
      num.oninput = () => validate() ? onChange(num.value) : (state.edits[p.key] = null, updateSaveBar());
      wrap.appendChild(num);
    }
  }
  return wrap;
}

function renderForm(group) {
  const body = $("#formBody");
  body.innerHTML = "";
  const params = state.schema.filter((p) => p.group === group);
  const box = document.createElement("div");
  box.className = "box";
  box.innerHTML = `<h3>${group}</h3>`;
  for (const p of params) {
    const row = document.createElement("div");
    row.className = "param" + (p.key in state.edits ? " changed" : "");
    const [cls, label] = APPLY_TAG[p.apply] || ["restart", p.apply];
    const range = p.type === "int" ? ` · ${p.min}–${p.max}` : "";
    const inUse = p.key === state.txpowerKey ? `<span class="tag inuse">in use</span>` : "";
    row.innerHTML = `<div class="name">${p.key}<span class="tag ${cls}">${label}</span>${inUse}</div>`;
    const value = p.key in state.edits && state.edits[p.key] !== null ? state.edits[p.key] : state.values[p.key];
    row.appendChild(control(p, value));
    const help = document.createElement("div");
    help.className = "help";
    help.textContent = `${p.help} (default ${p.default}${range})`;
    row.appendChild(help);
    box.appendChild(row);
  }
  body.appendChild(box);
  if (group === "Radio") {
    const note = document.createElement("div");
    note.className = "box note";
    note.innerHTML = "<h3>TX power</h3>" +
      "Each card keeps its own level (OpenIPC scale 1–63, about 0.5 dB per step), so swapping cards " +
      "never over-drives the new one. <b>8812AU</b>: raw power index. <b>8812CU / 8822CU / 8812EU</b>: " +
      "level ÷ 2 = dBm. <i>auto</i> = the card's default (20 / 20 / 40).<br>" +
      "Never transmit without an antenna. High levels, especially on the 8812EU, need a solid 5 V supply " +
      "and cooling: a brown-out reboots the board mid-flight.";
    body.appendChild(note);
  }
  if (group === "Radio" || group === "Telemetry") {
    const note = document.createElement("p");
    note.className = "muted";
    note.innerHTML = "Settings tagged <span class='tag link'>link</span> must be changed on the ground station too, " +
      "otherwise the link is lost.";
    body.appendChild(note);
  }
}

function updateSaveBar() {
  const keys = Object.keys(state.edits);
  const invalid = keys.filter((k) => state.edits[k] === null);
  $("#saveBar").hidden = keys.length === 0;
  $("#save").disabled = invalid.length > 0;
  $("#saveInfo").textContent = invalid.length
    ? `fix ${invalid.join(", ")}`
    : `${keys.length} unsaved change${keys.length > 1 ? "s" : ""}: ` + keys.map((k) => `${k}=${state.edits[k]}`).join("  ");
}

async function save() {
  const args = ["set"];
  for (const [k, v] of Object.entries(state.edits)) args.push(k, v);
  $("#save").disabled = true;
  const r = await fpv(...args);
  $("#save").disabled = false;
  if (!r.ok) return toast(r.error, "err");

  for (const res of r.results) state.values[res.key] = res.value;
  state.schema.forEach((p) => (p.value = state.values[p.key]));
  state.edits = {};
  updateSaveBar();
  if (!["status", "logs", "cli"].includes(state.tab)) renderForm(state.tab);

  const lines = r.results.map((x) => `${x.key} = ${x.value}  → ${x.result}`);
  if (r.warning) lines.push("", "⚠ " + r.warning);
  if (!r.fpv_running) lines.push("", "Saved. FPV is off (debug mode); used at the next FPV boot.");
  const needs = new Set(r.results.filter((x) => x.result === "restart-needed").map((x) => x.apply));
  const actions = [];
  if (needs.size) {
    const what = needs.has("video") && needs.has("link") ? "all" : needs.has("link") ? "link" : "video";
    actions.push([`Restart ${what}`, async () => {
      const rr = await fpv("restart", what);
      rr.ok ? toast(rr.restarted ? `Restarted ${what}` : rr.message, "ok") : toast(rr.error, "err");
    }]);
  }
  toast(lines.join("\n"), "ok", actions);
}

function revert() {
  state.edits = {};
  updateSaveBar();
  if (!["status", "logs", "cli"].includes(state.tab)) renderForm(state.tab);
}

// ---------------------------------------------------------------- logs + cli

async function loadLog() {
  $("#logText").textContent = "loading…";
  const r = await fpv("log", state.log, "300");
  $("#logText").textContent = r.ok ? r.text || "(empty)" : r.error;
  $("#logText").scrollTop = 1e9;
}

const cliHistory = [];
let histPos = 0;
async function cliSend(e) {
  e.preventDefault();
  const line = $("#cliIn").value.trim();
  if (!line) return;
  cliHistory.push(line);
  histPos = cliHistory.length;
  $("#cliIn").value = "";
  const out = $("#cliOut");
  out.textContent += `# ${line}\n`;
  const r = await post("/api/cli", { line });
  out.textContent += (r.ok ? r.output : "error: " + r.error) + "\n";
  out.scrollTop = 1e9;
  // a `fpv set` typed here changes values the forms show
  if (/^fpv\s+(set|reset)\b/.test(line)) {
    const s = await fpv("schema");
    if (s.ok) {
      state.schema = s.params;
      state.values = Object.fromEntries(s.params.map((p) => [p.key, p.value]));
    }
  }
}

// ---------------------------------------------------------------- wiring

$("#transport").onchange = () => {
  const ssh = $("#transport").value === "ssh";
  $("#host").hidden = !ssh;
  $("#device").hidden = ssh;
  $("#refresh").hidden = ssh;
};
$("#refresh").onclick = loadDevices;
$("#connect").onclick = connect;
$$("#tabs a").forEach((a) => (a.onclick = () => showTab(a.dataset.tab)));
$$("#bootMode button").forEach((b) => (b.onclick = () => setBootMode(b.dataset.mode)));
$("#install").onclick = async () => {
  const r = await post("/api/install", {});
  r.ok ? toast("Installed: " + r.installed.join(", "), "ok") : toast(r.error, "err");
};
$$("#logSel button").forEach((b) => (b.onclick = () => {
  state.log = b.dataset.log;
  $$("#logSel button").forEach((x) => x.classList.toggle("on", x === b));
  loadLog();
}));
$("#logReload").onclick = loadLog;
$("#cliForm").onsubmit = cliSend;
$("#cliIn").onkeydown = (e) => {
  if (e.key === "ArrowUp" && histPos > 0) { $("#cliIn").value = cliHistory[--histPos]; e.preventDefault(); }
  if (e.key === "ArrowDown") { histPos = Math.min(cliHistory.length, histPos + 1); $("#cliIn").value = cliHistory[histPos] || ""; }
};
$("#save").onclick = save;
$("#revert").onclick = revert;
window.onbeforeunload = () => (Object.keys(state.edits).length ? "unsaved changes" : undefined);

$("#tabs").classList.add("disabled");
showTab("status");
// ?connect[&tab=Video]: connect to the first device on load (bookmarkable)
const qs = new URLSearchParams(location.search);
loadDevices().then(() => {
  if (!qs.has("connect")) return;
  if (qs.get("tab")) state.tab = qs.get("tab");
  connect();
});
