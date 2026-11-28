// rkfpv configurator UI, one page for the PC (wide layout) and phones
// (portrait). Every board action is an `fpv ... --json` call; board.js
// sends it through the PC configurator (rkfpvconf.py), straight to the
// board over its WiFi hotspot, or over USB from the browser (WebUSB).
import * as board from "./board.js";
import { Preview } from "./preview.js";
import { t, tf, applyDom, setLang, initialLang, LANGS, lang } from "./i18n.js";

await setLang(initialLang());   // the words come from i18n/<lang>.json

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

const NON_FORM_TABS = ["status", "live", "image", "dvr", "presets", "logs", "cli"];
// the Live tab's quick controls: what you tune while watching (all apply live)
const LIVE_KEYS = ["bitrate", "isp_exp_max", "isp_brightness", "isp_contrast", "isp_saturation",
  "isp_sharpness", "isp_nr_temporal", "isp_nr_spatial"];
// apply modes that change the picture at once; edited while the preview runs,
// these are written right away (no Save needed)
const LIVE_APPLY = ["venc"];
const IMAGE_GROUPS = [["Picture", "#imgPicture"], ["Exposure", "#imgExposure"], ["Noise & detail", "#imgNoise"]];

// apply mode -> [tag style, i18n key of its label]
const APPLY_TAG = {
  venc: ["live", "apply.live"], radio: ["live", "apply.live"], fec: ["live", "apply.live"], txpower: ["live", "apply.live"],
  video: ["restart", "apply.restart"], link: ["link", "apply.link"], boot: ["restart", "apply.nextBoot"],
  hotspot: ["restart", "apply.nextHotspot"],
};
const applyTag = (p) => {
  const [cls, key] = APPLY_TAG[p.apply] || ["restart", ""];
  return [cls, key ? t(key) : p.apply];
};

// ---------------------------------------------------------------- transport

function conn() {
  return {
    transport: $("#transport").value,
    device: $("#device").value,
    host: $("#host").value.trim(),
  };
}
board.setConnection(conn);

const post = board.post;
const fpv = board.fpv;
// wired preview through the PC configurator needs ADB; on the hotspot and over WebUSB it always works
const previewPossible = () => board.kind !== "local" || conn().transport === "adb";

// ---------------------------------------------------------------- ui helpers

let toastTimer;
function toast(msg, kind = "", actions = []) {
  const el = $("#toast");
  el.className = kind;
  el.textContent = msg;
  for (const [label, fn] of actions) {
    const b = document.createElement("button");
    b.textContent = label;
    b.className = "primary";
    b.style.marginRight = "6px";
    b.onclick = () => { el.hidden = true; fn(); };
    el.appendChild(document.createElement("br"));
    el.appendChild(b);
  }
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (el.hidden = true), actions.length ? 15000 : 5000);
}

function setBadge(mode) {
  const b = $("#modeBadge");
  b.className = "badge " + (mode || "off");
  b.textContent = mode ? tf(`badge.${mode}`, mode) : t("badge.off");
}

function showTab(tab) {
  if (!state.connected && tab !== "status") return;
  state.tab = tab;
  $$("#tabs a").forEach((a) => a.classList.toggle("active", a.dataset.tab === tab));
  $("#notConnected").hidden = state.connected;
  $$(".tab").forEach((s) => (s.hidden = true));
  if (!state.connected) return;
  placeCanvas();
  if (tab === "status") { $("#tab-status").hidden = false; refreshStatus(); renderForm("Hotspot", $("#hsSettings")); loadHotspot(); }
  else if (tab === "live") { $("#tab-live").hidden = false; renderLive(); if (!pv.on && previewPossible()) previewStart(); }
  else if (tab === "logs") { $("#tab-logs").hidden = false; loadLog(); }
  else if (tab === "cli") { $("#tab-cli").hidden = false; $("#cliIn").focus(); }
  else if (tab === "presets") { $("#tab-presets").hidden = false; loadPresets(); }
  else if (tab === "image") { $("#tab-image").hidden = false; renderImage(); loadIq(); }
  else if (tab === "dvr") { $("#tab-dvr").hidden = false; renderForm("DVR", $("#dvrSettings")); refreshDvr(); loadDvrFiles(); }
  else { $("#tab-form").hidden = false; renderForm(tab); }
}

// ---------------------------------------------------------------- connect

async function loadDevices() {
  if (board.kind !== "local") return;
  const r = await fetch("/api/devices", { headers: { "X-Token": window.FPV_TOKEN } }).then((x) => x.json());
  const sel = $("#device");
  const prev = sel.value;
  sel.innerHTML = "";
  if (!r.ok) {
    sel.add(new Option(r.error, ""));
    return;
  }
  if (!r.devices.length) sel.add(new Option(t("conn.noDevice"), ""));
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
  if (!board.hotspotSupported()) return toast($("#browserNote").textContent.trim(), "err");
  $("#connect").disabled = true;
  $("#connect").textContent = t("conn.connecting");
  try {
    if (board.kind === "usb" && !board.usbConnected()) {
      // the browser's device chooser: has to start from this click
      try {
        if (!(await board.usbConnect())) return;          // chooser cancelled
      } catch (e) {
        toast(board.usbExplain(e) || t("err.usb", { e: e.message || e }), "err");
        return;
      }
    }
    const r = await fpv("schema");
    if (!r.ok) {
      if (r.missing_fpv && board.caps.install) {
        toast(t("toast.noFpv"), "err", [[t("toast.installNow"), async () => {
          const i = await board.install();
          i.ok ? (toast(t("toast.installed", { files: i.installed.join(", ") }), "ok"), connect()) : toast(i.error, "err");
        }]]);
      } else toast(r.error, "err");
      return;
    }
    state.schema = r.params;
    state.values = Object.fromEntries(r.params.map((p) => [p.key, p.value]));
    state.edits = {};
    state.connected = true;
    $("#connect").textContent = t("conn.disconnect");
    $("#tabs").classList.remove("disabled");
    $("#previewBtn").disabled = !previewPossible();
    showTab(state.tab);
    if (state.tab !== "status") refreshStatus();   // mode badge
    state.pollTimer = setInterval(() => {
      if (state.tab === "status") { refreshStatus(); loadHotspot(); }
      if (state.tab === "dvr") refreshDvr();
    }, 2000);
  } catch (e) {
    toast(t(board.kind === "local" ? "toast.serverUnreachable" : "toast.boardUnreachable", { e }), "err");
  } finally {
    $("#connect").disabled = false;
    if (!state.connected) {
      $("#connect").textContent = t("conn.connect");
      // free the device for adb / another tab when the board did not answer
      if (board.kind === "usb") board.usbDisconnect();
    }
  }
}

function disconnect() {
  // over USB: stop the preview on the board, then let go of the device
  if (board.kind === "usb" && board.usbConnected()) {
    (pv.on ? previewStop() : Promise.resolve()).finally(() => board.usbDisconnect());
  } else if (pv.on) previewStop();
  $("#previewBtn").disabled = true;
  state.connected = false;
  clearInterval(state.pollTimer);
  $("#connect").textContent = t("conn.connect");
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
  return h ? t("fmt.hm", { h, m }) : t("fmt.ms", { m, s: s % 60 });
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
  $("#stMode").textContent = tf(`mode.${s.mode}`, s.mode);
  $("#stModeSub").textContent = t("status.modeSub", { role: s.usb_role || "?", override: s.override });

  if (s.rkfpv_pid) {
    $("#stStream").textContent = s.fps ? `${s.fps} fps · ${(s.kbps / 1000).toFixed(2)} Mbps` : t("status.starting");
    $("#stStreamSub").textContent = `rkfpv pid ${s.rkfpv_pid}\n${s.last_stats || ""}`;
  } else {
    $("#stStream").textContent = t("status.off");
    $("#stStreamSub").textContent = s.last_stats ? t("status.lastRun") + "\n" + s.last_stats : t("status.noRkfpv");
  }

  if (s.wfb_tx_pid) {
    const r = kv(s.radio), f = kv(s.fec);
    $("#stRadio").textContent = `MCS ${r.mcs_index ?? "?"} · FEC ${f.k ?? "?"}/${f.n ?? "?"}`;
    $("#stRadioSub").textContent = `${s.driver || "?"} · ${r.bandwidth ?? "?"} MHz · STBC ${r.stbc ?? "?"} · LDPC ${r.ldpc ?? "?"}` +
      (s.txpower_key ? "\n" + t("status.txpower", { p: s.txpower, card: s.txpower_key.replace("txpower_", "") }) +
        (s.txpower_iw_dbm ? t("status.iwDbm", { dbm: s.txpower_iw_dbm }) : "") : "");
  } else {
    $("#stRadio").textContent = t("status.off");
    $("#stRadioSub").textContent = s.driver ? t("status.driverLoaded", { d: s.driver }) : t("status.noDriver");
  }

  $("#stBoard").textContent = s.temp_c ? `${s.temp_c} °C` : "–";
  const mem = s.mem_avail_kb ? t("status.memFree", { mb: (s.mem_avail_kb / 1024).toFixed(1) }) : "";
  $("#stBoardSub").textContent = `${mem}\n` + t("status.up", { t: fmtUptime(s.uptime_s) });

  $$("#bootMode button").forEach((b) => b.classList.toggle("on", b.dataset.mode === s.override));
}

async function setBootMode(mode) {
  if (mode === "fpv" && !confirm(t("boot.confirmFpv"))) return;
  const r = await fpv("mode", mode);
  r.ok ? toast(t("toast.bootMode", { mode: r.mode }) + (r.note ? "\n" + r.note : ""), "ok") : toast(r.error, "err");
  refreshStatus();
}

// ---------------------------------------------------------------- settings forms

function control(p, value, slider = false) {
  const wrap = document.createElement("div");
  const onChange = (v) => {
    if (String(v) === String(state.values[p.key])) delete state.edits[p.key];
    else state.edits[p.key] = String(v);
    wrap.closest(".param").classList.toggle("changed", p.key in state.edits);
    updateSaveBar();
    if (pv.on && LIVE_APPLY.includes(p.apply)) scheduleLiveApply();
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
      sel.add(new Option(t("form.value"), ""));
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
    if (slider) {
      const outer = document.createElement("div");
      outer.appendChild(wrap);
      addSlider(p, outer, wrap, num, validate, onChange);
      return outer;
    }
  }
  return wrap;
}

// A slider under the number field (Live tab): drag it and the picture follows.
// Moving it while a word (iq / auto / off) is selected switches to "value".
function addSlider(p, outer, wrap, num, validate, onChange) {
  const box = document.createElement("div");
  box.className = "slider";
  const r = document.createElement("input");
  r.type = "range";
  r.min = p.min;
  r.max = p.max;
  r.step = p.max - p.min > 2000 ? 50 : 1;
  r.value = num.value === "" ? p.min : num.value;
  box.appendChild(r);
  outer.appendChild(box);
  const sel = wrap.querySelector("select");
  box.classList.toggle("off", num.disabled);
  r.oninput = () => {
    if (sel && sel.value !== "") { sel.value = ""; num.disabled = false; box.classList.remove("off"); }
    num.value = r.value;
    if (validate()) onChange(num.value);
  };
  num.addEventListener("input", () => { if (num.value !== "") r.value = num.value; });
  if (sel) sel.addEventListener("change", () => {
    box.classList.toggle("off", sel.value !== "");
    if (sel.value === "") r.value = num.value || p.min;
  });
}

function renderForm(group, body = $("#formBody")) {
  renderParams(tf(`group.${group}`, group), state.schema.filter((p) => p.group === group), body);
  if (group === "Radio" || group === "Telemetry") radioNotes(group, body);
}

// the Live tab: a few picture settings under the video
function renderLive() {
  const params = LIVE_KEYS.map((k) => state.schema.find((p) => p.key === k)).filter(Boolean);
  renderParams(t("live.tune"), params, $("#liveControls"), true);
  const note = document.createElement("p");
  note.className = "sub";
  note.textContent = t("live.tuneNote");
  $("#liveControls").appendChild(note);
}

function renderParams(title, params, body, sliders = false) {
  body.innerHTML = "";
  const box = document.createElement("div");
  box.className = "box";
  box.innerHTML = "<h3></h3>";
  box.firstChild.textContent = title;
  for (const p of params) {
    const row = document.createElement("div");
    row.className = "param" + (p.key in state.edits ? " changed" : "");
    row.dataset.key = p.key;
    const [cls, label] = applyTag(p);
    const range = p.type === "int" ? ` · ${p.min}–${p.max}` : "";
    const inUse = p.key === state.txpowerKey ? `<span class="tag inuse">${t("form.inUse")}</span>` : "";
    row.innerHTML = `<div class="name">${p.key}<span class="tag ${cls}">${label}</span>${inUse}</div>`;
    const value = p.key in state.edits && state.edits[p.key] !== null ? state.edits[p.key] : state.values[p.key];
    row.appendChild(control(p, value, sliders));
    const help = document.createElement("div");
    help.className = "help";
    // setting help: the language file's text, else the board's own (English)
    const h = tf(`param.${p.key}`, p.help);
    help.textContent = p.default === "" ? h : h + t("form.default", { d: p.default, range });
    row.appendChild(help);
    box.appendChild(row);
  }
  body.appendChild(box);
}

function radioNotes(group, body) {
  if (group === "Radio") {
    const note = document.createElement("div");
    note.className = "box note";
    note.innerHTML = `<h3>${t("radio.txTitle")}</h3>` + t("radio.txNote.html");
    body.appendChild(note);
  }
  if (group === "Radio" || group === "Telemetry") {
    const note = document.createElement("p");
    note.className = "muted";
    note.innerHTML = t("radio.linkNote.html");
    body.appendChild(note);
  }
}

function updateSaveBar() {
  const keys = Object.keys(state.edits);
  const invalid = keys.filter((k) => state.edits[k] === null);
  $("#saveBar").hidden = keys.length === 0;
  $("#save").disabled = invalid.length > 0;
  $("#saveInfo").textContent = invalid.length
    ? t("save.fix", { keys: invalid.join(", ") })
    : t(keys.length > 1 ? "save.unsavedN" : "save.unsaved1", { n: keys.length }) + " " +
      keys.map((k) => `${k}=${state.edits[k]}`).join("  ");
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
  if (!r.fpv_running) lines.push("", t("toast.savedOff"));
  const needs = new Set(r.results.filter((x) => x.result === "restart-needed").map((x) => x.apply));
  const actions = [];
  if (needs.size) {
    const what = needs.has("video") && needs.has("link") ? "all" : needs.has("link") ? "link" : "video";
    actions.push([t(`restart.${what}`), async () => {
      const rr = await fpv("restart", what);
      rr.ok ? toast(rr.restarted ? t(`restarted.${what}`) : rr.message, "ok") : toast(rr.error, "err");
    }]);
  }
  toast(lines.join("\n"), "ok", actions);
  return r;
}

// While the preview shows, picture settings are written as you move them
// (one `fpv set` ~300 ms after the last change): you see the result at once.
let liveTimer = null, liveBusy = false;
function scheduleLiveApply() {
  clearTimeout(liveTimer);
  liveTimer = setTimeout(liveApply, 300);
}

async function liveApply() {
  if (liveBusy) return scheduleLiveApply();
  const vals = {};
  for (const [k, v] of Object.entries(state.edits)) {
    const p = state.schema.find((x) => x.key === k);
    if (v !== null && p && LIVE_APPLY.includes(p.apply)) vals[k] = v;
  }
  if (!Object.keys(vals).length) return;
  liveBusy = true;
  const args = ["set"];
  for (const [k, v] of Object.entries(vals)) args.push(k, v);
  const r = await fpv(...args);
  liveBusy = false;
  if (!r.ok) return toast(r.error, "err");
  for (const res of r.results) {
    state.values[res.key] = res.value;
    if (state.edits[res.key] === vals[res.key]) delete state.edits[res.key];
  }
  state.schema.forEach((p) => (p.value = state.values[p.key]));
  $$(".param.changed").forEach((row) => {
    if (!(row.dataset.key in state.edits)) row.classList.remove("changed");
  });
  updateSaveBar();
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
  else if (state.tab === "live") renderLive();
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
  b.textContent = t(r.mode === "hotspot" ? "hs.on" : r.mode === "switching" ? "hs.switching" : "hs.off");
  const sub = document.createElement("span");
  sub.className = "sub";
  sub.textContent = t("hs.info", { sw: r.switch, ssid: r.ssid || t("hs.ssidLater"), pass: r.pass || "-", ch: r.channel }) +
    (r.mode === "hotspot" ? t("hs.clients", { n: r.clients, addr: r.address }) : "");
  box.append(b, sub);
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
  b.textContent = t(r.fallback ? "iq.fallback" : custom ? "iq.custom" : "iq.stock");
  const sub = document.createElement("span");
  sub.className = "sub";
  sub.textContent = (r.fallback ? " " + t("iq.fallbackNote", { note: r.note }) : "") +
    (r.sensor ? "  " + t("iq.sensor", { s: r.sensor }) : "") + (+r.tries ? "  · " + t("iq.tries", { n: r.tries }) : "");
  $("#iqState").append(b, sub);
  $("#iqUseStock").disabled = !custom;
  $("#iqUseCustom").disabled = custom || !r.custom.length;

  const adb = board.caps.iqFiles && (board.kind === "usb" || conn().transport === "adb");
  const tb = $("#iqFiles");
  tb.innerHTML = "";
  const rows = [...r.custom.map((f) => ({ ...f, where: "custom" })), ...r.stock.map((f) => ({ ...f, where: "stock" }))];
  for (const f of rows) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td></td><td class="mono"></td><td></td><td></td>`;
    const active = (f.where === "custom") === custom;
    tr.children[0].textContent = t(`iq.where.${f.where}`) + (active ? " ●" : "");
    tr.children[1].textContent = f.name;
    tr.children[2].textContent = fmtMB(f.size);
    const dl = document.createElement("button");
    dl.textContent = t("btn.download");
    dl.disabled = !adb;
    dl.onclick = () => iqDownload(f.name, f.where);
    tr.children[3].append(dl);
    tb.appendChild(tr);
  }
  $("#iqHint").textContent = adb ? "" : t(board.kind === "local" ? "iq.needAdb" : "iq.needUsb");
  $("#iqUpload").disabled = !adb;

  const hb = $("#iqHistory");
  hb.innerHTML = "";
  const hist = h.ok ? h.history.sort((a, b2) => b2.version - a.version) : [];
  $("#iqHistBox").hidden = !hist.length;
  for (const v of hist) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td class="mono"></td><td></td><td></td><td></td>`;
    tr.children[0].textContent = v.name;
    tr.children[1].textContent = t("iq.version", { v: v.version });
    tr.children[2].textContent = fmtMB(v.size);
    const rb = document.createElement("button");
    rb.textContent = t("iq.rollback");
    rb.onclick = async () => {
      if (!confirm(t("iq.rollbackConfirm", { name: v.name, v: v.version }))) return;
      const x = await fpv("iq", "rollback", v.name, String(v.version));
      x.ok ? (toast(t("iq.restored", { name: v.name, v: v.version }), "ok",
                    [[t("restart.video"), restartVideo]]), loadIq()) : toast(x.error, "err");
    };
    tr.children[3].append(rb);
    hb.appendChild(tr);
  }
}

async function restartVideo() {
  const r = await fpv("restart", "video");
  r.ok ? toast(r.restarted ? t("toast.videoRestarted") : r.message, "ok") : toast(r.error, "err");
  setTimeout(loadIq, 4000);
}

async function useIq(dir) {
  const r = await applyValues({ iq_dir: dir });
  if (r && r.ok) loadIq();
}

async function iqDownload(name, where) {
  const r = await board.iqPull(name, where);
  if (!r.ok) return toast(r.error, "err");
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([r.bytes], { type: "application/json" }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}

async function iqUpload(file) {
  const buf = new Uint8Array(await file.arrayBuffer());
  toast(t("iq.uploading", { name: file.name }));
  const r = await board.iqPush(file.name, buf);
  if (!r.ok) return toast(r.error, "err");
  const custom = iqInfo && iqInfo.configured === IQ_CUSTOM;
  toast(t("iq.uploaded", { name: file.name, dir: IQ_CUSTOM }) + " " + t(custom ? "iq.thenRestart" : "iq.thenSwitch"), "ok",
        custom ? [[t("restart.video"), restartVideo]] : [[t("iq.useCustomIq"), () => useIq(IQ_CUSTOM)]]);
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
  const label = tf(`dvr.state.${r.state}`, r.state);
  badge.textContent = label;
  badge.className = "recbadge" + (r.state === "recording" || r.state === "waiting" ? " rec"
    : ["full", "error", "no-card", "unformatted"].includes(r.state) ? " warn" : "");
  $("#dvrFormat").hidden = r.state !== "unformatted";
  const rec = r.state === "recording" || r.state === "waiting";
  btn.textContent = t(rec ? "dvr.stop" : "dvr.start");
  btn.disabled = !r.fpv_running || r.state === "no-card" || r.state === "unformatted";
  $("#dvrFile").textContent = rec && r.file !== "-" ? r.file : "";
  $("#dvrSub").textContent = r.state === "unformatted" ? t("dvr.unformatted")
    : !r.fpv_running ? t("dvr.fpvOff")
    : rec ? t("dvr.written", { mb: fmtMB(+r.bytes) }) + (+r.drops ? t("dvr.drops", { n: r.drops }) : "") +
      (r.mem_avail_kb ? t("dvr.ram", { mb: (r.mem_avail_kb / 1024).toFixed(1) }) : "")
    : r.error ? t("dvr.error", { e: r.error.replace(/_/g, " ") }) : "";
  const total = +r.total_mb, free = +r.free_mb;
  $("#dvrBar").style.width = total ? `${Math.round(100 * (total - free) / total)}%` : "0";
  $("#dvrSpace").textContent = total ? t("dvr.space", { free: (free / 1024).toFixed(1), total: (total / 1024).toFixed(1) }) : "";
}

async function toggleDvr() {
  const start = !(dvrState === "recording" || dvrState === "waiting");
  const r = await fpv("dvr", start ? "start" : "stop");
  if (!r.ok) return toast(r.error, "err");
  toast(t(start ? "dvr.started" : "dvr.stopped"), "ok");
  setTimeout(() => { refreshDvr(); loadDvrFiles(); }, 1200);
}

let dvrFiles = [];
async function loadDvrFiles() {
  const tb = $("#dvrFiles");
  const r = await fpv("dvr", "list");
  tb.innerHTML = "";
  if (!r.ok) { $("#dvrHint").textContent = r.error; dvrFiles = []; return; }
  dvrFiles = r.files;
  const direct = board.kind === "hotspot";
  const adb = direct || board.kind === "usb" || ["adb", "hotspot"].includes(conn().transport);
  $("#dvrHint").textContent = t(!r.files.length ? "dvr.none" : direct ? "dvr.hintHotspot"
    : board.kind === "usb" ? "dvr.hintUsb" : adb ? "dvr.hintLocal" : "dvr.hintNeed");
  for (const f of r.files) {
    const tr = document.createElement("tr");
    tr.innerHTML = `<td class="mono"></td><td></td><td></td>`;
    const bbl = f.name.endsWith(".bbl");   // flight controller blackbox log (bblog), for Gyroflow
    tr.children[0].textContent = f.name + (bbl ? "  " + t("dvr.blackbox") : "") + (f.recording ? "  " + t("dvr.recording") : "");
    tr.children[1].textContent = fmtMB(f.size);
    const dl = document.createElement("button");
    dl.textContent = t("btn.download");
    dl.disabled = !adb || f.recording;
    dl.onclick = () => pull(f.name, dl);
    const play = document.createElement("a");
    play.textContent = t("dvr.play");
    play.className = "btn ghost";
    play.target = "_blank";
    play.rel = "noopener";
    play.href = board.dvrFileUrl(f.name, false);
    play.hidden = !direct || f.recording || bbl;
    const del = document.createElement("button");
    del.textContent = t("btn.delete");
    del.className = "ghost danger";
    del.disabled = f.recording;
    del.onclick = async () => {
      if (!confirm(t("dvr.delConfirm", { name: f.name }))) return;
      const d = await fpv("dvr", "delete", f.name);
      d.ok ? loadDvrFiles() : toast(d.error, "err");
    };
    tr.children[2].append(play, dl, del);
    tb.appendChild(tr);
  }
}

async function pull(name, btn, folder = null) {
  if (board.kind === "hotspot") {
    // the board sends it as an attachment: the browser saves it, the page stays
    const a = document.createElement("a");
    a.href = board.dvrFileUrl(name, true);
    a.rel = "noopener";
    document.body.appendChild(a);
    a.click();
    a.remove();
    toast(t("dvr.downloading", { name }), "ok");
    return true;
  }
  const old = btn ? btn.textContent : "";
  if (btn) { btn.disabled = true; btn.textContent = t("dvr.busy"); }
  const total = (dvrFiles.find((f) => f.name === name) || {}).size || 0;
  const onBytes = (n) => {
    if (btn && total) btn.textContent = `${Math.floor(100 * n / total)} %`;
  };
  const r = await board.dvrSave(name, { folder, onBytes });
  if (btn) { btn.disabled = false; btn.textContent = old; }
  if (r.cancelled) return false;
  if (!r.ok) { toast(r.error, "err"); return false; }
  toast(t("dvr.done", { name, mb: fmtMB(r.size), s: r.seconds, rate: r.mbps }) + "\n" + r.path, "ok",
        board.caps.savesOnPc ? [[t("dvr.open"), () => post("/api/dvr/open", {})]] : []);
  return true;
}

async function pullAll() {
  const todo = dvrFiles.filter((f) => !f.recording);
  // over USB: one folder for all of them (asked once) instead of a dialog per file
  let folder = null;
  if (todo.length && board.kind === "usb") {
    try { folder = await board.pickDvrFolder(); } catch (e) { if (e.name === "AbortError") return; toast(String(e), "err"); return; }
  }
  for (let i = 0; i < todo.length; i++) {
    toast(t("dvr.allProgress", { i: i + 1, n: todo.length, name: todo[i].name }));
    if (!(await pull(todo[i].name, null, folder))) return;
  }
  if (todo.length) toast(t("dvr.allDone", { n: todo.length }), "ok",
    board.caps.savesOnPc ? [[t("dvr.open"), () => post("/api/dvr/open", {})]] : []);
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
    file: "", name: t("presets.factory"),
    description: t("presets.factoryDesc"),
    values: Object.fromEntries(state.schema.map((p) => [p.key, p.default])),
  };
}

async function loadPresets() {
  const list = $("#presetList");
  list.textContent = t("misc.loading");
  let presets = [];
  try {
    const r = await board.presets();
    if (r.ok) presets = r.presets;
    else toast(r.error, "err");
  } catch (e) { toast(t("presets.err", { e }), "err"); }
  presets.unshift(factoryPreset());
  list.innerHTML = "";
  for (const pr of presets) {
    const card = document.createElement("div");
    card.className = "preset";
    const keys = Object.keys(pr.values);
    card.innerHTML = `<div class="pname"></div><div class="pdesc"></div><div class="pkeys"></div>`;
    // preset files carry English; the language file may translate them by file name
    const name = pr.file ? tf(`preset.${pr.file}.name`, pr.name) : pr.name;
    const desc = pr.file ? tf(`preset.${pr.file}.desc`, pr.description || "") : pr.description || "";
    card.querySelector(".pname").textContent = name;
    card.querySelector(".pdesc").textContent = desc;
    card.querySelector(".pkeys").textContent =
      keys.length > 8 ? t("presets.nSettings", { n: keys.length }) : keys.join(" · ");
    const b = document.createElement("button");
    b.textContent = t("presets.compare");
    b.onclick = () => showDiff(t("presets.diffTitle", { name }), desc, pr.values);
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
  toast(t("backup.saved", { name, n: Object.keys(r.values).length }), "ok");
}

async function restoreFile(file) {
  let doc;
  try { doc = JSON.parse(await file.text()); } catch (e) { return toast(t("backup.notJson", { name: file.name, e: e.message }), "err"); }
  const values = doc && typeof doc.values === "object" && !Array.isArray(doc.values) && doc.values;
  if (!values) return toast(t("backup.noValues", { name: file.name }), "err");
  const when = doc.created ? t("backup.when", { d: new Date(doc.created).toLocaleString(lang === "zh" ? "zh-CN" : undefined) }) : "";
  showDiff(t("backup.diffTitle", { name: file.name }), (doc.description || t("backup.backup")) + when, values);
}

// Compare `values` with the board and tick what to apply. Link settings
// start unticked: changing them without the ground station loses the link.
let diffRows = [];
function showDiff(title, desc, values) {
  if (Object.keys(state.edits).length &&
      !confirm(t("diff.discard"))) return;
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
    if (!p) { skipped.push(t("diff.unknown", { key })); continue; }
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
    const [cls, label] = applyTag(p);
    tr.children[5].innerHTML = !ok ? `<span class="tag link">${t("diff.invalid")}</span>`
      : link ? `<span class="tag link">${t("apply.link")}</span> ${t("diff.gsMatch")}`
      : `<span class="tag ${cls}">${label}</span>`;
    if (!ok) tr.className = "bad";
    cb.onchange = updateDiffButton;
    tb.appendChild(tr);
    diffRows.push({ key, value: v, cb });
  }
  const extra = [];
  if (same.length) extra.push(t("diff.same", { n: same.length, keys: same.join(", ") }));
  if (skipped.length) extra.push(t("diff.skipped", { keys: skipped.join(", ") }));
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
  $("#diffApply").textContent = n ? t(n > 1 ? "diff.applyN" : "diff.apply1", { n }) : t("diff.nothing");
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
  $("#logText").textContent = t("misc.loading");
  const r = await fpv("log", state.log, "300");
  $("#logText").textContent = r.ok ? r.text || t("logs.empty") : r.error;
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
  const r = await board.cli(line);
  out.textContent += (r.ok ? r.output : t("cli.error", { e: r.error })) + "\n";
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

// ---------------------------------------------------------------- live preview
// One decoder, one canvas. The canvas sits in the preview dock (right side on
// a wide screen, top on a phone) and moves into the Live tab while that is open.

const pv = new Preview($("#pvCanvas"), {
  onMsg: (text, ffplay = false) => {
    $("#pvMsg").textContent = text;
    $("#liveMsg").textContent = text;
    $("#pvFfplay").hidden = !ffplay;
  },
  onInfo: (text) => {
    $("#pvInfo").textContent = text;
    $("#liveInfo").textContent = text;
  },
});

function placeCanvas() {
  const cv = $("#pvCanvas");
  const live = state.tab === "live" && state.connected;
  const slot = live ? $("#liveStage") : $("#pvSlot");
  if (cv.parentElement !== slot) slot.appendChild(cv);
  // the dock is not needed while the Live tab shows the picture itself
  $("#previewDock").hidden = !(pv.on || pvStarting) || live;
}

let pvStarting = false;
async function previewStart() {
  if (!previewPossible()) return toast(t("preview.needs"), "err");
  if (pv.on || pvStarting) return;
  $("#previewBtn").classList.add("on");
  pvStarting = true;          // dock visible with "Starting video…" meanwhile
  placeCanvas();
  const r = await pv.start();
  pvStarting = false;
  if (!r.ok) $("#previewBtn").classList.remove("on");
  placeCanvas();
  if (state.connected) refreshStatus();   // mode badge: preview
}

async function previewStop() {
  const r = await pv.stop();
  $("#previewBtn").classList.remove("on");
  placeCanvas();
  if (r && !r.ok && state.connected) toast(r.error, "err");
  if (state.connected) refreshStatus();
}

async function previewFfplay() {
  // only one reader at a time: the browser lets go of the stream
  pv.detach();
  const r = await board.previewFfplay();
  r.ok ? pv.onMsg(t("preview.inFfplay"))
       : pv.onMsg(r.error, true);
}

function liveFullscreen() {
  const el = $("#liveStage");
  if (document.fullscreenElement) return document.exitFullscreen();
  if (!el.requestFullscreen) return;
  el.requestFullscreen().then(() => {
    // phones: turn to landscape while full screen (not every browser allows it)
    if (screen.orientation && screen.orientation.lock) screen.orientation.lock("landscape").catch(() => {});
  }).catch(() => {});
}

const DEFAULT_HOST = { fpvd: "172.32.0.93", ssh: "10.5.0.10", hotspot: "10.0.0.1" };
$("#transport").onchange = () => {
  const tr = $("#transport").value;
  if (board.hosted) return hostedTransport(tr);
  const net = tr !== "adb";
  $("#host").hidden = !net;
  $("#device").hidden = net;
  $("#refresh").hidden = net;
  if (net && Object.values(DEFAULT_HOST).includes($("#host").value)) $("#host").value = DEFAULT_HOST[tr];
};
$("#refresh").onclick = loadDevices;
$("#previewBtn").onclick = () => (pv.on ? previewStop() : previewStart());
$("#pvStop").onclick = () => previewStop();
$("#pvSize").onclick = () => $("#previewDock").classList.toggle("big");
$("#pvFfplay").onclick = previewFfplay;
$("#connect").onclick = connect;
$$("#tabs a").forEach((a) => (a.onclick = () => showTab(a.dataset.tab)));
$$("#bootMode button").forEach((b) => (b.onclick = () => setBootMode(b.dataset.mode)));
$("#install").onclick = async () => {
  const r = await board.install();
  r.ok ? toast(t("toast.installed", { files: r.installed.join(", ") }), "ok") : toast(r.error, "err");
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
  r.ok ? (toast(t("iq.copied", { n: r.copied, dir: IQ_CUSTOM }), "ok"), loadIq())
       : toast(r.error, "err");
};
$("#iqUpload").onclick = () => $("#iqFile").click();
$("#iqFile").onchange = (e) => { const f = e.target.files[0]; e.target.value = ""; if (f) iqUpload(f); };
$("#dvrRefresh").onclick = () => { refreshDvr(); loadDvrFiles(); };
$("#dvrAll").onclick = pullAll;
$("#dvrOpen").onclick = async () => { const r = await post("/api/dvr/open", {}); if (!r.ok) toast(r.error, "err"); };
$("#dvrDelAll").onclick = async () => {
  if (!confirm(t("dvr.delAllConfirm"))) return;
  const r = await fpv("dvr", "delete-all");
  r.ok ? (toast(t("dvr.deleted", { n: r.deleted }), "ok"), loadDvrFiles()) : toast(r.error, "err");
};
$("#dvrFormat").onclick = async () => {
  if (!confirm(t("dvr.formatConfirm"))) return;
  if (!confirm(t("dvr.formatConfirm2"))) return;
  const r = await fpv("dvr", "format", "--yes");
  r.ok ? (toast(t("dvr.formatted", { dev: r.device }), "ok"), refreshDvr(), loadDvrFiles()) : toast(r.error, "err");
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
window.onbeforeunload = () => (Object.keys(state.edits).length ? t("save.unsavedWarn") : undefined);

// light / dark: follows the system until the button picks one (kept per browser)
const darkQuery = window.matchMedia("(prefers-color-scheme: dark)");
const isDark = () => {
  const th = document.documentElement.dataset.theme;
  return th ? th === "dark" : darkQuery.matches;
};
function showThemeBtn() {
  $("#themeBtn").textContent = isDark() ? "☀" : "☾";
  $("#themeBtn").title = t(isDark() ? "theme.light" : "theme.dark");
}
$("#themeBtn").onclick = () => {
  const th = isDark() ? "light" : "dark";
  document.documentElement.dataset.theme = th;
  try { localStorage.setItem("rkfpv.theme", th); } catch (e) { /* private mode */ }
  showThemeBtn();
};
darkQuery.addEventListener("change", showThemeBtn);
showThemeBtn();

$("#liveFull").onclick = liveFullscreen;
$("#liveStage").ondblclick = liveFullscreen;

// hosted page: USB (WebUSB) or the board's hotspot; no PC-only tools
function hostedTransport(tr) {
  if (state.connected) disconnect();
  board.setTransport(tr);
  try { localStorage.setItem("rkfpv.transport", tr); } catch (e) { /* private mode */ }
  const usb = board.kind === "usb";
  $("#host").hidden = usb;
  $("#ncUsb").hidden = !usb;
  $("#ncHotspot").hidden = usb;
  $("#browserNote").hidden = board.hotspotSupported();
  $("#browserNote").innerHTML = t(usb ? "note.usb.html" : "note.hotspot.html");
  $("#installBox").hidden = !board.caps.install;
  $("#dvrOpen").hidden = !board.caps.savesOnPc;
}

if (board.hosted) {
  document.body.classList.add("hosted");
  $("#device").hidden = $("#refresh").hidden = true;
  for (const o of [...$("#transport").options]) if (!["usb", "hotspot"].includes(o.value)) o.remove();
  try { $("#host").value = localStorage.getItem("rkfpv.host") || "10.0.0.1"; } catch (e) { $("#host").value = "10.0.0.1"; }
  $("#host").onchange = () => { try { localStorage.setItem("rkfpv.host", $("#host").value.trim()); } catch (e) { /* private mode */ } };
  $("#ncLocal").hidden = true;
  let tr = board.kind;
  try { tr = localStorage.getItem("rkfpv.transport") || tr; } catch (e) { /* private mode */ }
  if (tr === "usb" && !board.usbSupported()) tr = "hotspot";
  $("#transport").value = tr;
  hostedTransport(tr);
  // unplugged, or the board rebooted
  board.onUsbGone(() => {
    if (!state.connected) return;
    disconnect();
    toast(t("usb.gone"), "err");
  });
  // a phone puts the page in the background: free the board's encoder for nobody
  document.addEventListener("visibilitychange", () => {
    if (document.hidden && pv.on) previewStop();
  });
  window.addEventListener("pagehide", () => {
    if (pv.on) board.previewStop();
    if (board.kind === "usb") board.usbDisconnect();
  });
  // offline copy for use on the hotspot (no internet there)
  if ("serviceWorker" in navigator && location.protocol === "https:") {
    navigator.serviceWorker.register("sw.js").catch(() => {});
  }
}
if (!board.hosted) $("#transport").querySelector('option[value="usb"]').remove();   // the PC server has its own ADB
$("#installBox").hidden = !board.caps.install;
$("#dvrOpen").hidden = !board.caps.savesOnPc;

// language: every text comes from i18n/<lang>.json; switching redraws in place
for (const [code, label] of Object.entries(LANGS)) $("#langSel").add(new Option(label, code));
$("#langSel").value = lang;
function applyLang() {
  applyDom();
  document.title = t("page.title");
  showThemeBtn();
  $("#connect").textContent = t(state.connected ? "conn.disconnect" : "conn.connect");
  $("#previewBtn").textContent = "▶ " + t("preview.title");
  $("#browserNote").innerHTML = t(board.kind === "usb" ? "note.usb.html" : "note.hotspot.html");
}
$("#langSel").onchange = async () => {
  await setLang($("#langSel").value);
  applyLang();
  if (state.connected) { showTab(state.tab); refreshStatus(); } else setBadge(null);
  if (state.connected && !$("#saveBar").hidden) updateSaveBar();
};
applyLang();
setBadge(null);

$("#tabs").classList.add("disabled");
showTab("status");
// ?connect[&tab=Video]: connect on load (bookmarkable; the first ADB device locally)
const qs = new URLSearchParams(location.search);
loadDevices().then(() => {
  if (!qs.has("connect")) return;
  if (qs.get("tab")) state.tab = qs.get("tab");
  connect();
});
