// rkfpv web configurator (prototype): WebUSB -> ADB (Tango) -> `fpv ... --json` on the board.
// Same idea as the desktop configurator, without Python or adb.exe on the PC.
import { Adb, AdbDaemonTransport } from "@yume-chan/adb";
import { AdbDaemonWebUsbDeviceManager } from "@yume-chan/adb-daemon-webusb";
import AdbWebCredentialStore from "@yume-chan/adb-credential-web";

const $ = (s) => document.querySelector(s);
const manager = AdbDaemonWebUsbDeviceManager.BROWSER;
const credentials = new AdbWebCredentialStore("rkfpv");

let adb = null;
let queue = Promise.resolve();      // one shell command at a time

function log(msg) {
  const t = new Date().toTimeString().slice(0, 8);
  $("#log").textContent += `${t}  ${msg}\n`;
  $("#log").scrollTop = $("#log").scrollHeight;
}

function hint(html, kind = "warn") {
  const el = $("#hint");
  if (!html) { el.hidden = true; return; }
  el.className = `notice ${kind}`;
  el.innerHTML = html;
  el.hidden = false;
}

// ---------------------------------------------------------------- errors

function explain(e) {
  const name = e?.name || "";
  const text = String(e?.message || e);
  if (e?.constructor?.name === "DeviceBusyError" || /already in use/i.test(text) || name === "NetworkError") {
    return "板子被别的程序占用了。电脑上开着 <code>adb</code> 的话，先运行 <code>adb kill-server</code>；" +
      "也要关掉桌面版配置工具、Android Studio 这类会用 adb 的程序。然后再点“连接板子”。";
  }
  if (name === "SecurityError" || /access denied/i.test(text)) {
    const os = navigator.userAgent;
    if (/Windows/.test(os)) {
      return "Windows 不允许浏览器访问这个设备：板子的 ADB 接口没有用 WinUSB 驱动。" +
        "装过 Rockchip 驱动助手或 Google USB 驱动的话，插拔一次再试；还不行请把这条信息反馈给我们。";
    }
    if (/Linux/.test(os) && !/Android/.test(os)) {
      return "Linux 上没有权限访问 USB 设备。展开“Linux 上第一次使用”，运行那两行命令，然后重新插拔板子。";
    }
    return "浏览器没有权限访问这个设备。重新插拔板子后再试。";
  }
  return null;
}

function fail(where, e) {
  log(`${where}: ${e?.name || "Error"}: ${e?.message || e}`);
  hint(explain(e) || `${where}失败：${e?.message || e}`);
}

// ---------------------------------------------------------------- connect

async function connect() {
  hint(null);
  let device;
  try {
    device = await manager.requestDevice();
  } catch (e) {
    return fail("选择设备", e);
  }
  if (!device) { log("没有选择设备"); return; }
  log(`连接 ${device.name || ""} (${device.serial})`);
  try {
    const connection = await device.connect();
    const transport = await AdbDaemonTransport.authenticate({
      serial: device.serial,
      connection,
      credentialStore: credentials,
    });
    adb = new Adb(transport);
    haveExec = true;
  } catch (e) {
    return fail("连接", e);
  }
  const b = adb.banner;
  $("#devName").textContent = `${b.model || b.product || "板子"} · ${device.serial.slice(-4)}`;
  log(`已连接：model=${b.model} product=${b.product} device=${b.device}`);
  $("#btnConnect").hidden = true;
  $("#btnDisconnect").hidden = false;
  $("#welcome").hidden = true;
  $("#statusBox").hidden = false;
  $("#cliBox").hidden = false;
  adb.disconnected.then(() => onDisconnected("板子断开了（拔线或重启）"));
  refreshStatus();
}

async function disconnect() {
  if (!adb) return;
  const a = adb;
  adb = null;
  try { await a.close(); } catch (e) { log(`断开: ${e?.message || e}`); }
  onDisconnected("已断开，现在 adb 等其他程序可以使用板子了");
}

function onDisconnected(msg) {
  if (adb) adb = null;
  if ($("#btnConnect").hidden) log(msg);
  $("#devName").textContent = "";
  $("#btnConnect").hidden = false;
  $("#btnDisconnect").hidden = true;
  $("#statusBox").hidden = true;
  $("#cliBox").hidden = true;
  $("#welcome").hidden = false;
}

// ---------------------------------------------------------------- shell

// The board's adbd (luckfox / buildroot) has no `exec:` service, which
// spawnWaitText uses, and answers "Socket open failed". `shell:<cmd>` is what
// the desktop `adb shell` uses on such daemons; it may turn \n into \r\n.
let haveExec = true;

async function shellService(cmd) {
  const socket = await adb.createSocket(`shell:${cmd}`);
  const reader = socket.readable.getReader();
  const dec = new TextDecoder();
  let text = "";
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    text += dec.decode(value, { stream: true });
  }
  return (text + dec.decode()).replace(/\r\n/g, "\n");
}

function shell(cmd) {
  const run = async () => {
    if (!adb) throw new Error("没有连接板子");
    if (haveExec) {
      try {
        return await adb.subprocess.noneProtocol.spawnWaitText(cmd);
      } catch (e) {
        if (!/socket open failed/i.test(String(e?.message || e))) throw e;
        haveExec = false;
        log("板子不支持 exec:，改用 shell:");
      }
    }
    return await shellService(cmd);
  };
  const p = queue.then(run, run);
  queue = p.catch(() => {});
  return p;
}

// ---------------------------------------------------------------- status

const LABELS = {
  mode: "模式", override: "开机模式", usb_role: "USB 角色", fps: "帧率", kbps: "码率 (kbps)",
  radio: "无线", fec: "FEC", driver: "网卡驱动", txpower: "发射功率", txpower_iw_dbm: "发射功率 (dBm)",
  mem_avail_kb: "可用内存 (KB)", temp_c: "温度 (°C)", uptime_s: "运行时间 (秒)",
  rkfpv_pid: "rkfpv 进程", wfb_tx_pid: "wfb_tx 进程", last_stats: "最近统计",
};

async function refreshStatus() {
  const box = $("#status");
  box.textContent = "读取中…";
  let out;
  try {
    out = await shell("fpv status --json 2>/dev/null || echo __NOFPV__");
  } catch (e) {
    box.textContent = "";
    return fail("读取状态", e);
  }
  if (out.includes("__NOFPV__") || !out.trim().startsWith("{")) {
    box.innerHTML = "<p class='muted'>板子上没有 <code>fpv</code> 命令（旧镜像）。下面的命令行仍然可以用。</p>";
    log("fpv status: 不可用");
    return;
  }
  let s;
  try { s = JSON.parse(out); } catch { box.textContent = out; return; }
  box.innerHTML = "";
  for (const [k, v] of Object.entries(s)) {
    if (k === "ok" || v === "" || v === null) continue;
    const row = document.createElement("div");
    const key = document.createElement("span");
    const val = document.createElement("span");
    key.textContent = LABELS[k] || k;
    val.textContent = typeof v === "object" ? JSON.stringify(v) : String(v);
    row.append(key, val);
    box.append(row);
  }
  log("状态已刷新");
}

// ---------------------------------------------------------------- CLI

async function runCli(ev) {
  ev.preventDefault();
  const cmd = $("#cliInput").value.trim();
  if (!cmd) return;
  $("#cliOut").textContent += `# ${cmd}\n`;
  try {
    const out = await shell(cmd);
    $("#cliOut").textContent += out.endsWith("\n") || !out ? out : out + "\n";
  } catch (e) {
    $("#cliOut").textContent += `(错误) ${e?.message || e}\n`;
  }
  $("#cliOut").scrollTop = $("#cliOut").scrollHeight;
}

// ---------------------------------------------------------------- start

if (!manager || !window.isSecureContext) {
  $("#unsupported").hidden = false;
  $("#btnConnect").disabled = true;
  log(!window.isSecureContext ? "需要 HTTPS（或 localhost）才能使用 WebUSB" : "浏览器不支持 WebUSB");
} else {
  $("#btnConnect").addEventListener("click", connect);
  $("#btnDisconnect").addEventListener("click", disconnect);
  $("#btnRefresh").addEventListener("click", refreshStatus);
  $("#cliForm").addEventListener("submit", runCli);
  window.addEventListener("pagehide", () => { if (adb) adb.close(); });
  log("就绪：点“连接板子”");
}
