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

const NON_FORM_TABS = ["status", "image", "dvr", "presets", "logs", "cli"];
const IMAGE_GROUPS = [["Picture", "#imgPicture"], ["Exposure", "#imgExposure"], ["Noise & detail", "#imgNoise"]];

const APPLY_TAG = {
  venc: ["live", "live"], radio: ["live", "live"], fec: ["live", "live"], txpower: ["live", "live"],
  video: ["restart", "restart"], link: ["link", "link"], boot: ["restart", "next boot"],
  hotspot: ["restart", "next hotspot"],
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
  if (tab === "status") { $("#tab-status").hidden = false; refreshStatus(); renderForm("Hotspot", $("#hsSettings")); loadHotspot(); }
  else if (tab === "logs") { $("#tab-logs").hidden = false; loadLog(); }
  else if (tab === "cli") { $("#tab-cli").hidden = false; $("#cliIn").focus(); }
  else if (tab === "presets") { $("#tab-presets").hidden = false; loadPresets(); }
  else if (tab === "image") { $("#tab-image").hidden = false; renderImage(); loadIq(); }
  else if (tab === "dvr") { $("#tab-dvr").hidden = false; renderForm("DVR", $("#dvrSettings")); refreshDvr(); loadDvrFiles(); }
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
    state.pollTimer = setInterval(() => {
      if (state.tab === "status") { refreshStatus(); loadHotspot(); }
      if (state.tab === "dvr") refreshDvr();
    }, 2000);
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
  } else if (p.type === "str") {
    const inp = document.createElement("input");
    inp.type = "text";
    inp.maxLength = p.max;
    inp.spellcheck = false;
    inp.autocomplete = "off";
    inp.value = value;
    inp.oninput = () => {
      const ok = validValue(p, inp.value);
      inp.classList.toggle("invalid", !ok);
      ok ? onChange(inp.value) : (state.edits[p.key] = null, updateSaveBar());
    };
    wrap.appendChild(inp);
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
    if (p.specials && p.specials.length) {
      // words like auto / iq / off, or a number
      wrap.className = "auto";
      const sel = document.createElement("select");
      for (const w of p.specials) sel.add(new Option(w, w));
      sel.add(new Option("value", ""));
      wrap.append(sel, num);
      const special = p.specials.includes(value);
      sel.value = special ? value : "";
      num.disabled = special;
      num.value = special ? "" : value;
      sel.onchange = () => {
        num.disabled = sel.value !== "";
        if (sel.value) { num.classList.remove("invalid"); onChange(sel.value); }
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

function renderForm(group, body = $("#formBody")) {
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
    help.textContent = p.default === "" ? p.help : `${p.help} (default ${p.default}${range})`;
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

// one `fpv set` for a {key: value} object (form Save, presets, restore);
// fpv validates every pair before it writes anything
async function applyValues(values) {
  const args = ["set"];
  for (const [k, v] of Object.entries(values)) args.push(k, v);
  if (args.length === 1) return null;
  const r = await fpv(...args);
  if (!r.ok) { toast(r.error, "err"); return r; }

  for (const res of r.results) state.values[res.key] = res.value;
  state.schema.forEach((p) => (p.value = state.values[p.key]));

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
  return r;
}

async function save() {
  $("#save").disabled = true;
  const r = await applyValues(state.edits);
  $("#save").disabled = false;
  if (!r || !r.ok) return;
  state.edits = {};
  updateSaveBar();
  rerenderForms();
}

// redraw the settings form(s) of the current tab after values changed
function rerenderForms() {
  if (!NON_FORM_TABS.includes(state.tab)) renderForm(state.tab);
  else if (state.tab === "dvr") renderForm("DVR", $("#dvrSettings"));
  else if (state.tab === "image") renderImage();
  else if (state.tab === "status") renderForm("Hotspot", $("#hsSettings"));
}

function revert() {
  state.edits = {};
  updateSaveBar();
  rerenderForms();
}

// ---------------------------------------------------------------- hotspot

async function loadHotspot() {
  const r = await fpv("hotspot", "status");
  const box = $("#hsState");
  box.innerHTML = "";
  if (!r.ok) { box.textContent = r.error; return; }
  const b = document.createElement("span");
  b.className = "recbadge" + (r.mode === "hotspot" ? " rec" : "");
  b.textContent = r.mode === "hotspot" ? "● hotspot on" : r.mode === "switching" ? "switching…" : "hotspot off";
  const t = document.createElement("span");
  t.className = "sub";
  t.textContent = `switch ${r.switch} · network ${r.ssid || "(created on first use)"} · password ${r.pass || "-"}` +
    ` · channel ${r.channel}` + (r.mode === "hotspot" ? ` · ${r.clients} client(s) · http://${r.address}/` : "");
  box.append(b, t);
}

// ---------------------------------------------------------------- image + IQ

function renderImage() {
  for (const [g, sel] of IMAGE_GROUPS) renderForm(g, $(sel));
}

const IQ_STOCK = "/oem/usr/share/iqfiles", IQ_CUSTOM = "/userdata/iq";
let iqInfo = null;

async function loadIq() {
  const [r, h] = await Promise.all([fpv("iq", "status"), fpv("iq", "history")]);
  if (!r.ok) { $("#iqState").textContent = r.error; return; }
  iqInfo = r;
  const custom = r.configured === IQ_CUSTOM;
  $("#iqState").innerHTML = "";
  const b = document.createElement("span");
  b.className = "recbadge" + (r.fallback ? " warn" : "");
  b.textContent = r.fallback ? "FALLBACK: stock in use" : custom ? "custom IQ" : "stock IQ";
  const t = document.createElement("span");
  t.className = "sub";
  t.textContent = (r.fallback ? ` ${r.note}. Fix or roll back the custom file, then restart video.` : "") +
    (r.sensor ? `  sensor ${r.sensor}` : "") + (+r.tries ? `  · failed starts: ${r.tries}` : "");
  $("#iqState").append(b, t);
  $("#iqUseStock").disabled = !custom;
  $("#iqUseCustom").disabled = custom || !r.custom.length;

  const adb = $("#transport").value === "adb";
  const tb = $("#iqFiles");
  tb.innerHTML = "";
  const rows = [...r.custom.map((f) => ({ ...f, where: "custom" })), ...r.stock.map((f) => ({ ...f, where: "stock" }))];
  for (const f of rows) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td></td><td class="mono"></td><td></td><td></td>`;
    const active = (f.where === "custom") === custom;
    tr.children[0].textContent = f.where + (active ? " ●" : "");
    tr.children[1].textContent = f.name;
    tr.children[2].textContent = fmtMB(f.size);
    const dl = document.createElement("button");
    dl.textContent = "Download";
    dl.disabled = !adb;
    dl.onclick = () => iqDownload(f.name, f.where);
    tr.children[3].append(dl);
    tb.appendChild(tr);
  }
  $("#iqHint").textContent = adb ? "" : "Download and upload need the ADB (USB) connection.";
  $("#iqUpload").disabled = !adb;

  const hb = $("#iqHistory");
  hb.innerHTML = "";
  const hist = h.ok ? h.history.sort((a, b2) => b2.version - a.version) : [];
  $("#iqHistBox").hidden = !hist.length;
  for (const v of hist) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td class="mono"></td><td></td><td></td><td></td>`;
    tr.children[0].textContent = v.name;
    tr.children[1].textContent = "version " + v.version;
    tr.children[2].textContent = fmtMB(v.size);
    const rb = document.createElement("button");
    rb.textContent = "Roll back";
    rb.onclick = async () => {
      if (!confirm(`Replace the current custom ${v.name} with version ${v.version}? (The current one is kept in history.)`)) return;
      const x = await fpv("iq", "rollback", v.name, String(v.version));
      x.ok ? (toast(`Restored ${v.name} version ${v.version}. Restart video to use it.`, "ok",
                    [["Restart video", restartVideo]]), loadIq()) : toast(x.error, "err");
    };
    tr.children[3].append(rb);
    hb.appendChild(tr);
  }
}

async function restartVideo() {
  const r = await fpv("restart", "video");
  r.ok ? toast(r.restarted ? "Video restarted (about 2 s without picture)" : r.message, "ok") : toast(r.error, "err");
  setTimeout(loadIq, 4000);
}

async function useIq(dir) {
  const r = await applyValues({ iq_dir: dir });
  if (r && r.ok) loadIq();
}

async function iqDownload(name, where) {
  const r = await post("/api/iq/pull", { name, where });
  if (!r.ok) return toast(r.error, "err");
  const bytes = Uint8Array.from(atob(r.data), (c) => c.charCodeAt(0));
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([bytes], { type: "application/json" }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}

async function iqUpload(file) {
  const buf = new Uint8Array(await file.arrayBuffer());
  let bin = "";
  for (let i = 0; i < buf.length; i += 0x8000) bin += String.fromCharCode.apply(null, buf.subarray(i, i + 0x8000));
  toast(`Checking and uploading ${file.name}…`);
  const r = await post("/api/iq/push", { name: file.name, data: btoa(bin) });
  if (!r.ok) return toast(r.error, "err");
  const custom = iqInfo && iqInfo.configured === IQ_CUSTOM;
  toast(`${file.name} installed in ${IQ_CUSTOM} (md5 verified; the previous version is in history).` +
        (custom ? " Restart video to use it." : " Switch to the custom IQ to use it."), "ok",
        custom ? [["Restart video", restartVideo]] : [["Use custom IQ", () => useIq(IQ_CUSTOM)]]);
  loadIq();
}

// ---------------------------------------------------------------- DVR

const fmtMB = (b) => (b / 1048576).toFixed(b < 10485760 ? 1 : 0) + " MB";
let dvrState = "";

async function refreshDvr() {
  const r = await fpv("dvr", "status");
  if (!r.ok) return;
  dvrState = r.state;
  const badge = $("#dvrBadge"), btn = $("#dvrToggle");
  const label = { recording: "● REC", waiting: "● waiting for keyframe", stopped: "stopped",
                  full: "card full", error: "error", "no-card": "no DVR partition", off: "off" }[r.state] || r.state;
  badge.textContent = label;
  badge.className = "recbadge" + (r.state === "recording" || r.state === "waiting" ? " rec"
    : ["full", "error", "no-card"].includes(r.state) ? " warn" : "");
  const rec = r.state === "recording" || r.state === "waiting";
  btn.textContent = rec ? "Stop recording" : "Start recording";
  btn.disabled = !r.fpv_running || r.state === "no-card";
  $("#dvrFile").textContent = rec && r.file !== "-" ? r.file : "";
  $("#dvrSub").textContent = !r.fpv_running
    ? "FPV is off (debug mode): recording happens in FPV mode. Start/Stop needs fpvd over the air (issue #1) or the serial console."
    : rec ? `${fmtMB(+r.bytes)} written${+r.drops ? ` · ${r.drops} frames dropped (card too slow?)` : ""}` +
      (r.mem_avail_kb ? ` · RAM free ${(r.mem_avail_kb / 1024).toFixed(1)} MB` : "")
    : r.error ? `error: ${r.error.replace(/_/g, " ")}` : "";
  const total = +r.total_mb, free = +r.free_mb;
  $("#dvrBar").style.width = total ? `${Math.round(100 * (total - free) / total)}%` : "0";
  $("#dvrSpace").textContent = total ? `${(free / 1024).toFixed(1)} GB free of ${(total / 1024).toFixed(1)} GB` : "";
}

async function toggleDvr() {
  const start = !(dvrState === "recording" || dvrState === "waiting");
  const r = await fpv("dvr", start ? "start" : "stop");
  if (!r.ok) return toast(r.error, "err");
  toast(start ? "Recording starts at the next keyframe" : "Recording stopped", "ok");
  setTimeout(() => { refreshDvr(); loadDvrFiles(); }, 1200);
}

let dvrFiles = [];
async function loadDvrFiles() {
  const tb = $("#dvrFiles");
  const r = await fpv("dvr", "list");
  tb.innerHTML = "";
  if (!r.ok) { $("#dvrHint").textContent = r.error; dvrFiles = []; return; }
  dvrFiles = r.files;
  const adb = ["adb", "hotspot"].includes($("#transport").value);
  $("#dvrHint").textContent = !r.files.length ? "No recordings on the card."
    : adb ? `Files are saved on this PC in the folder 'Open folder' shows.`
    : "Downloading needs the ADB (USB) or WiFi hotspot connection.";
  for (const f of r.files) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td class="mono"></td><td></td><td></td>`;
    tr.children[0].textContent = f.name + (f.recording ? "  ● recording" : "");
    tr.children[1].textContent = fmtMB(f.size);
    const dl = document.createElement("button");
    dl.textContent = "Download";
    dl.disabled = !adb || f.recording;
    dl.onclick = () => pull(f.name, dl);
    const del = document.createElement("button");
    del.textContent = "Delete";
    del.className = "ghost danger";
    del.disabled = f.recording;
    del.onclick = async () => {
      if (!confirm(`Delete ${f.name} from the card?`)) return;
      const d = await fpv("dvr", "delete", f.name);
      d.ok ? loadDvrFiles() : toast(d.error, "err");
    };
    tr.children[2].append(dl, del);
    tb.appendChild(tr);
  }
}

async function pull(name, btn) {
  const old = btn ? btn.textContent : "";
  if (btn) { btn.disabled = true; btn.textContent = "downloading…"; }
  const r = await post("/api/dvr/pull", { name });
  if (btn) { btn.disabled = false; btn.textContent = old; }
  if (!r.ok) { toast(r.error, "err"); return false; }
  toast(`${name}: ${fmtMB(r.size)} in ${r.seconds} s (${r.mbps} MB/s)\n${r.path}`, "ok",
        [["Open folder", () => post("/api/dvr/open", {})]]);
  return true;
}

async function pullAll() {
  const todo = dvrFiles.filter((f) => !f.recording);
  for (let i = 0; i < todo.length; i++) {
    toast(`Downloading ${i + 1}/${todo.length}: ${todo[i].name}…`);
    if (!(await pull(todo[i].name, null))) return;
  }
  if (todo.length) toast(`Downloaded ${todo.length} recording(s)`, "ok", [["Open folder", () => post("/api/dvr/open", {})]]);
}

// ---------------------------------------------------------------- presets + backup

// same rules as files/fpv validate(); fpv checks again on save
function validValue(p, v) {
  if (p.specials && p.specials.includes(v)) return true;
  if (p.type === "str") return /^[A-Za-z0-9_.-]*$/.test(v) && v.length >= p.min && v.length <= p.max;
  if (p.type === "enum") return p.choices.includes(v);
  if (!/^\d+$/.test(v)) return false;
  return +v >= p.min && +v <= p.max;
}

function factoryPreset() {
  return {
    file: "", name: "Factory defaults",
    description: "The built-in default of every setting (from fpv schema). Not the same as " +
      "'fpv reset', which restores the image's config files (e.g. mirror/flip for your camera mount).",
    values: Object.fromEntries(state.schema.map((p) => [p.key, p.default])),
  };
}

async function loadPresets() {
  const list = $("#presetList");
  list.textContent = "loading…";
  let presets = [];
  try {
    const r = await fetch("/api/presets", { headers: { "X-Token": window.FPV_TOKEN } }).then((x) => x.json());
    if (r.ok) presets = r.presets;
    else toast(r.error, "err");
  } catch (e) { toast("presets: " + e, "err"); }
  presets.unshift(factoryPreset());
  list.innerHTML = "";
  for (const pr of presets) {
    const card = document.createElement("div");
    card.className = "preset";
    const keys = Object.keys(pr.values);
    card.innerHTML = `<div class="pname"></div><div class="pdesc"></div><div class="pkeys"></div>`;
    card.querySelector(".pname").textContent = pr.name;
    card.querySelector(".pdesc").textContent = pr.description || "";
    card.querySelector(".pkeys").textContent =
      keys.length > 8 ? `${keys.length} settings` : keys.join(" · ");
    const b = document.createElement("button");
    b.textContent = "Compare & apply…";
    b.onclick = () => showDiff(`Preset: ${pr.name}`, pr.description || "", pr.values);
    card.appendChild(b);
    list.appendChild(card);
  }
}

async function backup() {
  const r = await fpv("get");
  if (!r.ok) return toast(r.error, "err");
  const now = new Date();
  const doc = { format: "rkfpv-backup", version: 1, created: now.toISOString(), values: r.values };
  const pad = (n) => String(n).padStart(2, "0");
  const name = `rkfpv-backup-${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}` +
    `-${pad(now.getHours())}${pad(now.getMinutes())}.json`;
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([JSON.stringify(doc, null, 2) + "\n"], { type: "application/json" }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  toast(`Backup saved: ${name}\n${Object.keys(r.values).length} settings`, "ok");
}

async function restoreFile(file) {
  let doc;
  try { doc = JSON.parse(await file.text()); } catch (e) { return toast(`${file.name}: not JSON (${e.message})`, "err"); }
  const values = doc && typeof doc.values === "object" && !Array.isArray(doc.values) && doc.values;
  if (!values) return toast(`${file.name}: no "values" object (not an rkfpv backup or preset)`, "err");
  const when = doc.created ? ` (saved ${new Date(doc.created).toLocaleString()})` : "";
  showDiff(`Restore: ${file.name}`, (doc.description || "Backup") + when, values);
}

// Compare `values` with the board and tick what to apply. Link settings
// start unticked: changing them without the ground station loses the link.
let diffRows = [];
function showDiff(title, desc, values) {
  if (Object.keys(state.edits).length &&
      !confirm("You have unsaved changes in the settings tabs. Discard them?")) return;
  revert();
  const box = $("#diffBox");
  $("#diffTitle").textContent = title;
  $("#diffDesc").textContent = desc;
  const tb = $("#diffTable tbody");
  tb.innerHTML = "";
  diffRows = [];
  const same = [], skipped = [];
  for (const [key, raw] of Object.entries(values)) {
    const p = state.schema.find((x) => x.key === key);
    const v = String(raw);
    if (!p) { skipped.push(`${key} (unknown on this board)`); continue; }
    if (v === state.values[key]) { same.push(key); continue; }
    const ok = validValue(p, v);
    const link = p.apply === "link";
    const tr = document.createElement("tr");
    tr.innerHTML = `<td><input type="checkbox"></td><td class="mono"></td><td class="old"></td><td>→</td><td class="new"></td><td class="note"></td>`;
    const cb = tr.querySelector("input");
    cb.checked = ok && !link;
    cb.disabled = !ok;
    tr.children[1].textContent = key;
    tr.children[2].textContent = state.values[key];
    tr.children[4].textContent = v;
    const [cls, label] = APPLY_TAG[p.apply] || ["restart", p.apply];
    tr.children[5].innerHTML = !ok ? `<span class="tag link">invalid here</span>`
      : link ? `<span class="tag link">link</span> ground station must match`
      : `<span class="tag ${cls}">${label}</span>`;
    if (!ok) tr.className = "bad";
    cb.onchange = updateDiffButton;
    tb.appendChild(tr);
    diffRows.push({ key, value: v, cb });
  }
  const extra = [];
  if (same.length) extra.push(`${same.length} already the same: ${same.join(", ")}`);
  if (skipped.length) extra.push(`skipped: ${skipped.join(", ")}`);
  $("#diffExtra").textContent = extra.join("\n");
  $("#diffEmpty").hidden = diffRows.length > 0;
  $("#diffTable").hidden = diffRows.length === 0;
  box.hidden = false;
  updateDiffButton();
  box.scrollIntoView({ behavior: "smooth", block: "start" });
}

function updateDiffButton() {
  const n = diffRows.filter((r) => r.cb.checked).length;
  $("#diffApply").disabled = n === 0;
  $("#diffApply").textContent = n ? `Apply ${n} change${n > 1 ? "s" : ""}` : "Nothing selected";
}

async function applyDiff() {
  const sel = Object.fromEntries(diffRows.filter((r) => r.cb.checked).map((r) => [r.key, r.value]));
  $("#diffApply").disabled = true;
  const r = await applyValues(sel);
  if (r && r.ok) $("#diffBox").hidden = true;
  else updateDiffButton();
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

// USB RNDIS address for fpvd today; the wfb tunnel address (issue #1) later
const DEFAULT_HOST = { fpvd: "172.32.0.93", ssh: "10.5.0.10", hotspot: "192.168.73.1" };
$("#transport").onchange = () => {
  const t = $("#transport").value;
  const net = t !== "adb";
  $("#host").hidden = !net;
  $("#device").hidden = net;
  $("#refresh").hidden = net;
  if (net && Object.values(DEFAULT_HOST).includes($("#host").value)) $("#host").value = DEFAULT_HOST[t];
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
$("#dvrToggle").onclick = toggleDvr;
$("#iqUseStock").onclick = () => useIq(IQ_STOCK);
$("#iqUseCustom").onclick = () => useIq(IQ_CUSTOM);
$("#iqCopy").onclick = async () => {
  const r = await fpv("iq", "copy-stock");
  r.ok ? (toast(`Copied ${r.copied} stock file(s) to ${IQ_CUSTOM} (edited files are never overwritten)`, "ok"), loadIq())
       : toast(r.error, "err");
};
$("#iqUpload").onclick = () => $("#iqFile").click();
$("#iqFile").onchange = (e) => { const f = e.target.files[0]; e.target.value = ""; if (f) iqUpload(f); };
$("#dvrRefresh").onclick = () => { refreshDvr(); loadDvrFiles(); };
$("#dvrAll").onclick = pullAll;
$("#dvrOpen").onclick = async () => { const r = await post("/api/dvr/open", {}); if (!r.ok) toast(r.error, "err"); };
$("#dvrDelAll").onclick = async () => {
  if (!confirm("Delete ALL recordings from the card (except one being recorded)?")) return;
  const r = await fpv("dvr", "delete-all");
  r.ok ? (toast(`Deleted ${r.deleted} file(s)`, "ok"), loadDvrFiles()) : toast(r.error, "err");
};
$("#backup").onclick = backup;
$("#restore").onclick = () => $("#restoreFile").click();
$("#restoreFile").onchange = (e) => { const f = e.target.files[0]; e.target.value = ""; if (f) restoreFile(f); };
$("#diffApply").onclick = applyDiff;
$("#diffCancel").onclick = () => ($("#diffBox").hidden = true);
$$("#diffAll, #diffNone").forEach((b) => (b.onclick = () => {
  diffRows.forEach((r) => { if (!r.cb.disabled) r.cb.checked = b.id === "diffAll"; });
  updateDiffButton();
}));
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
