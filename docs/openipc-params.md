# OpenIPC adjustable parameters (reference) and their rkfpv equivalents

Taken from the OpenIPC RV1103 build in `~/github/openipc/firmware-rv1103-nand`:
- `wfb.yaml`, `/usr/bin/wifibroadcast`
- majestic's default `majestic.yaml` and the key names inside the majestic binary
- wfb-ng `wfb_tx_cmd`

How OpenIPC changes them:
- **wfb**: `wifibroadcast cli -s .broadcast.mcs_index 3`, which edits `/etc/wfb.yaml`. It takes effect after `wifibroadcast start` / reboot.
- **majestic**: `cli -s .video0.bitrate 6000`, which edits `/etc/majestic.yaml`. Majestic then reloads (`killall -1 majestic`) or applies live (see the "Live" column).
- **wfb_tx_cmd**: changes a running `wfb_tx` with no restart (`wfb_tx_cmd 8000 ...`).

---

## 1. Radio link — `/etc/wfb.yaml`

| Key | OpenIPC default | FPV default (`video_settings`) | Meaning | rkfpv `wfb.yaml` |
|---|---|---|---|---|
| `wireless.txpower` | 1 | 20 (8812AU), 40 (8812EU) | TX power. Driver units: 88XXau ×−100 (raw power index), others ×50 (level ÷ 2 = dBm) | `txpower` in **dBm** (1-30), one key for every card; `wfb.sh` converts: 88x2cu / 8812eu ×100 (mBm), 88XXau ×−200 (power index = 2 × dBm, approximate). Unset → 10 / 10 / 20 dBm (8812AU / 8812CU / 8812EU). An OpenIPC `txpower` value is **not** in dBm: halve it |
| `wireless.channel` | 161 | | 5 GHz channel | 161 |
| `wireless.width` | 20 | | 20 or 40 MHz (40 = HT40+) | 20 |
| `wireless.mlink` | 1500 | 3994 | Interface MTU; majestic RTP size = mlink − 100 | n/a → `pkt_size` in rkfpv.conf (1400) |
| `wireless.wlan_adapter` | — | bl-r8812af1 / bl-m8812eu2 | Adapter profile (used by alink) | n/a |
| `wireless.link_control` | — | alink | Adaptive link (`alink_drone`) | not implemented |
| `broadcast.mcs_index` | 2 | | Modulation. 0 BPSK½ · 1 QPSK½ · 2 QPSK¾ · 3 16QAM½ · 4 16QAM¾ · 5 64QAM⅔ … (1 stream) | 2 |
| `broadcast.fec_k` / `fec_n` | 8 / 12 | | FEC: k data + (n−k) recovery packets per block | 8 / 12 |
| `broadcast.stbc` | 0 | 1 (8812AU/EU) | Space-time block coding (needs 2 TX antennas) | 0 |
| `broadcast.ldpc` | 0 | 1 (8812AU/EU) | LDPC coding (more robust than BCC) | 0 |
| `broadcast.link_id` | 7669206 | | Must match the ground station | 7669206 |
| `broadcast.tun_index` | 1 | | MCS used for the tunnel / telemetry streams | 1 |
| `telemetry.telemetry_enabled` | false | | Start the telemetry router | true |
| `telemetry.router` | msposd | | msposd (OSD + MAVLink) or mavfwd | mavbridge (our own) |
| `telemetry.serial` | ttyS2 | | FC UART | ttyS3 |
| `telemetry.osd_fps` | 20 | | msposd refresh | n/a (no OSD) |

Fixed `wfb_tx` flags in both: `-C 8000` (control port), `-J 10`, `-E 5000` (FEC timeout ms).

### Live changes with `wfb_tx_cmd 8000 …` (no restart)

| Command | Options |
|---|---|
| `set_radio` | `-M mcs` `-B 20/40` `-G long/short` (guard interval) `-S stbc` `-L ldpc` `-N nss` `-V` (VHT) |
| `set_fec` | `-k K` `-n N` |
| `set_fec_timeout` | timeout ms |
| `get_radio` / `get_fec` | show current |

Example: `wfb_tx_cmd 8000 set_radio -M 3 -L 1`. This works the same on rkfpv.

---

## 2. Video encoder — majestic `video0.*` → rkfpv `rkfpv.conf`

| majestic key | OpenIPC FPV value | Live? | Meaning | rkfpv key |
|---|---|---|---|---|
| `video0.size` | 1920x1080 (1920x1440 imx335) | restart | Resolution | `size` (1280x720) |
| `video0.fps` | 60 | restart | Frame rate | `fps` (30) |
| `video0.codec` | h265 | restart | h264 / h265 | always H.265 |
| `video0.bitrate` | 8000 | **live** | kbps | `bitrate` (3500), live via SIGHUP |
| `video0.rcMode` | cbr | restart | cbr / vbr / avbr | `rc` (cbr) |
| `video0.gopSize` | 1 (sec, default yaml) | restart | Keyframe interval | `gop` in frames (30) |
| `video0.maxQp` | — | restart | QP ceiling for rate control | not exposed |
| `video0.sliceUnits` | — | restart | Slices per frame | `slice_bytes` (0 = off) |
| `video0.noiseLevel` / `fpv.noiseLevel` | 0 | restart | 3DNR strength; 0 = off for latency | not exposed (ISP/IQ default) |
| `fpv.enabled` | true | restart | FPV low-latency mode | rkfpv is always low-latency |
| `fpv.roiQp` / `fpv.roiRect` | — | restart | Region of interest: better QP in a rectangle (e.g. centre) | not implemented |
| `rtsp.naluSize` | mlink − 100 | reload | RTP payload size | `pkt_size` |
| `outgoing.servers` | udp://127.0.0.1:5600 | reload | Where RTP goes | `dst` + `port` |
| — (RTP PT) | 96 H.264 / 97 H.265 | | | `pt` (97) |
| — | | | Rolling intra refresh | `intra_refresh` |
| — | | | Max bitrate (VBR) | `max_bitrate` |
| — | | | RC window (s) | `stat_time` |

---

## 3. Image / ISP — majestic `image.*`, `isp.*`

Majestic applies all of these **live**. In rkfpv only mirror and flip exist; everything else comes from the IQ file.

| majestic key | Meaning | rkfpv |
|---|---|---|
| `image.mirror` / `image.flip` | Horizontal / vertical flip | `mirror` / `flip` (restart) |
| `image.rotate` | 0/90/180/270 | use mirror+flip for 180 |
| `image.contrast` / `hue` / `saturation` / `luminance` | 0–100, default 50 | not implemented |
| `isp.exposure` | Max exposure time (ms). OpenIPC FPV sets 16 → limits motion blur | not implemented (rkaiq AE default) |
| `isp.aGain` / `dGain` / `ispGain` | Gain limits | not implemented |
| `isp.slowShutter` | Allow fps drop in the dark | not implemented |
| `isp.aeMode`, `aeStrategy`, `aeSpeed`, `aeTolerance`, `meterRect` | Auto-exposure behaviour | not implemented |
| `isp.antiFlicker` | disabled / 50 / 60 Hz | not implemented |
| `isp.dehaze`, `isp.drc` | Dehaze, dynamic range compression | not implemented |
| `isp.awbMode`, `isp.awbGains` | White balance | not implemented |
| `isp.sensorConfig` | Sensor IQ file | `iq_dir` |

---

## 4. Worth porting into rkfpv next (FPV relevance)

1. **`isp.exposure` cap (16 ms or less)**: less motion blur when panning. This is probably the biggest visual win.
2. **`noiseLevel` = 0 (3DNR off)**: lower latency and less ghosting on motion.
3. **`maxQp`**: stops the encoder from smearing when it hits the bitrate cap.
4. **ROI QP**: a sharper centre at the same bitrate.
5. **STBC / LDPC = 1** in `wfb.yaml`: free robustness if the 8812CU and the ground card support it. Test with `wfb_tx_cmd 8000 set_radio -S 1 -L 1`.
6. **Adaptive link (alink)**: changes MCS / bitrate based on ground RSSI. Bigger project.
