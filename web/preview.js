// Live preview: the board's link stream (u32 big-endian length + H.265
// Annex-B access unit) decoded with WebCodecs on the GPU and drawn on one
// canvas. WebCodecs needs a secure context: http://127.0.0.1 (the PC
// configurator) and the hosted HTTPS page both are.
import * as board from "./board.js";
import { t } from "./i18n.js";

const HEVC_CODECS = ["hev1.1.6.L123.B0", "hvc1.1.6.L123.B0", "hev1.1.6.L153.B0", "hev1.1.6.L93.B0", "hvc1.1.6.L93.B0"];

export async function pickHevc() {
  if (!("VideoDecoder" in window)) return "";
  for (const hw of ["prefer-hardware", "no-preference"]) {
    for (const codec of HEVC_CODECS) {
      try {
        const r = await VideoDecoder.isConfigSupported({ codec, hardwareAcceleration: hw, optimizeForLatency: true });
        if (r.supported) return codec;
      } catch (e) { /* try the next one */ }
    }
  }
  return "";
}

// first NAL type of an Annex-B access unit: VPS (32) or IRAP (16..23) = key
export function isKey(au) {
  for (let i = 0; i + 4 < au.length; i++) {
    if (au[i] || au[i + 1] || au[i + 2] !== 1) continue;
    const nal = (au[i + 3] >> 1) & 0x3f;
    if (nal === 32 || (nal >= 16 && nal <= 23)) return true;
    if (nal < 32) return false;
    i += 2;
  }
  return false;
}

// Splits the byte stream into access units; feed() it chunks as they come.
export class FrameReader {
  constructor(onFrame) { this.onFrame = onFrame; this.buf = new Uint8Array(0); }
  feed(value) {
    const b = new Uint8Array(this.buf.length + value.length);
    b.set(this.buf); b.set(value, this.buf.length);
    let off = 0;
    while (b.length - off >= 4) {
      const n = ((b[off] << 24) >>> 0) + (b[off + 1] << 16) + (b[off + 2] << 8) + b[off + 3];
      if (n === 0 || n > 4 << 20) throw new Error("garbled stream");
      if (b.length - off - 4 < n) break;
      this.onFrame(b.subarray(off + 4, off + 4 + n));
      off += 4 + n;
    }
    this.buf = b.slice(off);
  }
}

export class Preview {
  // canvas: where frames go; onMsg(text, offerFfplay); onInfo(text, ...) once a second
  constructor(canvas, { onMsg = () => {}, onInfo = () => {} } = {}) {
    this.canvas = canvas;
    this.g = canvas.getContext("2d");
    this.onMsg = onMsg;
    this.onInfo = onInfo;
    this.on = false;
    this.codec = "";
    this.dec = null;
    this.ctl = null;
    this.waitKey = true;
    this.frames = this.bytes = this.shown = this.skipped = 0;
    this.t0 = 0;
    this.timer = null;
    this.retry = null;
  }

  async start() {
    if (this.on) return { ok: true };
    this.codec = await pickHevc();
    if (!this.codec) this.onMsg(t("preview.noHevc"), board.caps.ffplay);
    else this.onMsg(t("preview.starting"));
    const r = await board.previewStart();
    if (!r.ok) { this.onMsg(r.error); return r; }
    this.on = true;
    this.waitKey = true;
    this.t0 = performance.now();
    this.timer = setInterval(() => this._stats(), 1000);
    if (this.codec) this._read();
    return r;
  }

  async stop() {
    const was = this.on;
    this._halt();
    this.onInfo("");
    if (was) return board.previewStop();
    return { ok: true };
  }

  // stop reading but leave the board alone (ffplay takes over the stream)
  detach() {
    if (this.ctl) this.ctl.abort();
    clearTimeout(this.retry);
  }

  _halt() {
    this.on = false;
    clearTimeout(this.retry);
    clearInterval(this.timer);
    if (this.ctl) this.ctl.abort();
    this._closeDecoder();
  }

  _closeDecoder() {
    if (this.dec) { try { this.dec.close(); } catch (e) { /* closed */ } this.dec = null; }
  }

  _newDecoder() {
    const dec = new VideoDecoder({
      output: (f) => {
        const cv = this.canvas;
        if (cv.width !== f.displayWidth || cv.height !== f.displayHeight) {
          cv.width = f.displayWidth; cv.height = f.displayHeight;
        }
        this.g.drawImage(f, 0, 0);
        f.close();
        this.shown++;
      },
      error: (e) => {
        this.onMsg(t("preview.decoderError", { e: e.message }), board.caps.ffplay);
        this.waitKey = true;
        this.dec = null;
      },
    });
    dec.configure({ codec: this.codec, hardwareAcceleration: "prefer-hardware", optimizeForLatency: true });
    return dec;
  }

  _feed(au) {
    this.frames++;
    this.bytes += au.length;
    const key = isKey(au);
    if (!this.dec) {
      if (!key) return;
      this.dec = this._newDecoder();
    }
    // keep the latency low: if the decoder falls behind, skip to the next key frame
    if (this.dec.decodeQueueSize > 3) this.waitKey = true;
    if (this.waitKey) {
      if (!key) { this.skipped++; return; }
      this.waitKey = false;
    }
    this.dec.decode(new EncodedVideoChunk({ type: key ? "key" : "delta", timestamp: performance.now() * 1000, data: au }));
  }

  async _read() {
    if (!this.on) return;
    this.ctl = new AbortController();
    let got = false;
    try {
      const r = await board.previewFetch(this.ctl.signal);
      if (!r.ok) throw new Error(await r.text());
      const rd = r.body.getReader();
      const fr = new FrameReader((au) => this._feed(au));
      for (;;) {
        const { value, done } = await rd.read();
        if (done) break;
        if (!got) { got = true; this.onMsg(""); }
        fr.feed(value);
      }
    } catch (e) {
      if (e.name === "AbortError") return;
      this.onMsg(t("preview.streamError", { e: e.message }));
    }
    if (!this.on) return;
    // rkfpv restarted (a setting needed it) or is still starting: reconnect
    this._closeDecoder();
    this.waitKey = true;
    if (!got) this.onMsg(t("preview.waiting"));
    this.retry = setTimeout(() => this._read(), got ? 300 : 1500);
  }

  _stats() {
    const s = (performance.now() - this.t0) / 1000;
    if (s <= 0) return;
    this.onInfo(this.frames
      ? `${(this.shown / s).toFixed(1)} fps · ${Math.round(this.bytes * 8 / s / 1000)} kbps` +
        (this.dec ? ` · queue ${this.dec.decodeQueueSize}` : "") + (this.skipped ? ` · skipped ${this.skipped}` : "")
      : "");
    this.frames = this.bytes = this.shown = this.skipped = 0;
    this.t0 = performance.now();
  }
}
