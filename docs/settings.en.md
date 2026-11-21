# Settings reference

[中文](settings.zh.md)

Every setting the air unit has, as the `fpv` command and the PC configurator see them. The table in `files/fpv` (`params()`) is the source: a setting exists only if it is listed there, and `test/test_fpv.sh` checks that every key in that table is also in both versions of this page.

## Changing a setting

```sh
fpv get                       # all current values
fpv get bitrate mcs_index     # some of them
fpv set bitrate 4000 gop 60   # checks every value first, writes, applies what can be applied live
fpv restart                   # applies what needs a restart (only what pending changes need)
fpv schema                    # this table, from the board itself
fpv reset                     # shipped defaults
```

The PC configurator builds its pages from the same table, so each group below is one block on its page.

Settings are stored in two files:
- `rkfpv` in the **File** column: `/etc/rkfpv.conf` (`key = value`).
- `wfb` in the **File** column: `/etc/wfb.yaml`, under the section shown (`key: value`).

### When a change takes effect

| Apply | Takes effect | How |
|---|---|---|
| `venc` | at once | rkfpv re-reads its config (SIGHUP) |
| `radio` | at once | `wfb_tx_cmd set_radio` |
| `fec` | at once | `wfb_tx_cmd set_fec` |
| `txpower` | at once | `iw set txpower` |
| `video` | after `fpv restart video` | rkfpv restarts (video drops for about 2 s) |
| `link` | after `fpv restart link` | wfb-ng restarts; **the ground station must use the same value** |
| `boot` | next boot | |

`auto` as a default means the key is left unset (commented out in the file) and the driver or card default is used.

## Settings in `stable`

### Video

| Key | Default | Values | File | Apply | Description |
|---|---|---|---|---|---|
| `size` | 1280x720 | 1280x720, 1920x1080, 2304x1296 | rkfpv | video | Capture resolution |
| `fps` | 30 | 1–60 | rkfpv | video | Frame rate |
| `rc` | cbr | cbr, vbr | rkfpv | venc | Rate control: cbr = constant bitrate, vbr = variable |
| `bitrate` | 3500 | 100–50000 | rkfpv | venc | Target bitrate, kbit/s |
| `max_bitrate` | 0 | 0–50000 | rkfpv | venc | VBR peak, kbit/s (0 = encoder default) |
| `gop` | 30 | 1–600 | rkfpv | venc | Keyframe interval, frames (= fps for one keyframe per second) |
| `stat_time` | 1 | 1–60 | rkfpv | venc | Rate-control window, s (shorter = smaller peaks) |
| `slice_bytes` | 0 | 0–65536 | rkfpv | video | Slice size, bytes (0 = one slice per frame; 1300 = one slice per packet) |
| `intra_refresh` | 0 | 0–64 | rkfpv | video | Rolling intra refresh, rows per frame (0 = off, IDR every `gop`) |
| `pkt_size` | 1400 | 200–4000 | rkfpv | video | RTP packet size, bytes |
| `pt` | 97 | 96–127 | rkfpv | video | RTP payload type (97 = H.265 for OpenIPC ground stations) |

### Camera

| Key | Default | Values | File | Apply | Description |
|---|---|---|---|---|---|
| `mirror` | 1 | 0, 1 | rkfpv | video | Mirror left-right |
| `flip` | 1 | 0, 1 | rkfpv | video | Flip upside down (mirror + flip = rotate 180°) |

### Radio

| Key | Default | Values | File | Apply | Description |
|---|---|---|---|---|---|
| `channel` | 161 | 1–196 | wfb `wireless` | link | WiFi channel (ground station must match) |
| `width` | 20 | 20, 40 | wfb `wireless` | link | Channel width, MHz |
| `txpower_8812au` | auto (20) | 1–63 | wfb `wireless` | txpower | RTL8812AU TX power: power index, about 0.5 dB per step, 63 = max |
| `txpower_8812cu` | auto (20 = 10 dBm) | 1–63 | wfb `wireless` | txpower | RTL8812CU / 8822CU TX power: level / 2 = dBm |
| `txpower_8812eu` | auto (40 = 20 dBm) | 1–63 | wfb `wireless` | txpower | RTL8812EU TX power: level / 2 = dBm; high levels draw a lot of current |
| `mcs_index` | 2 | 0–7 | wfb `broadcast` | radio | Modulation: 0 slowest / longest range … 7 fastest |
| `stbc` | 0 | 0, 1 | wfb `broadcast` | radio | STBC (needs 2 TX antennas) |
| `ldpc` | 0 | 0, 1 | wfb `broadcast` | radio | LDPC coding |
| `fec_k` | 8 | 1–32 | wfb `broadcast` | fec | FEC data packets per block |
| `fec_n` | 12 | 2–32 | wfb `broadcast` | fec | FEC packets per block including recovery packets (must be > `fec_k`) |
| `link_id` | 7669206 | 0–16777215 | wfb `broadcast` | link | Link ID (ground station must match) |

The air unit uses one `txpower_*` key, the one for the card it detects. The other two keys are ignored.

### Telemetry

| Key | Default | Values | File | Apply | Description |
|---|---|---|---|---|---|
| `telemetry_enabled` | true | true, false | wfb `telemetry` | link | MAVLink telemetry over wfb-ng |
| `baud` | 115200 | 57600, 115200, 230400, 460800, 921600 | wfb `telemetry` | link | Flight-controller UART baud rate |

## In open pull requests (not in `stable` yet)

These keys may still change before they are merged. When a pull request is merged, its rows move up into the section above.

### #5 DVR and Image tab

`iq` as a default means "use the IQ (sensor tuning) file's value", so the setting changes nothing until it is set. `off` means the feature is off.

| Key | Default | Values | File | Apply | Group | Description |
|---|---|---|---|---|---|---|
| `isp_brightness` | iq | 0–255 | rkfpv | venc | Picture | Brightness, 128 = neutral |
| `isp_contrast` | iq | 0–255 | rkfpv | venc | Picture | Contrast, 128 = neutral |
| `isp_saturation` | iq | 0–255 | rkfpv | venc | Picture | Saturation, 128 = neutral (0 = black and white) |
| `isp_hue` | iq | 0–255 | rkfpv | venc | Picture | Hue, 128 = neutral |
| `isp_sharpness` | iq | 0–100 | rkfpv | venc | Picture | Sharpness (high values also sharpen noise) |
| `isp_wb` | auto | auto, daylight, cloudy, shade, twilight, fluorescent, incandescent | rkfpv | venc | Picture | White balance |
| `isp_exp_max` | iq (30) | 1–33 | rkfpv | venc | Exposure | Longest exposure, ms. Shorter = less motion blur, but darker and noisier in low light; FPV often uses 2–8. Below 10 ms anti-flicker is turned off |
| `isp_gain_max` | iq (512) | 1–512 | rkfpv | venc | Exposure | Highest gain, x. Lower = less noise at night, darker picture |
| `isp_antiflicker` | iq | iq, off, 50, 60 | rkfpv | venc | Exposure | Mains-light flicker, Hz (indoors); needs exposure steps of 10 / 8.3 ms |
| `isp_ae_delay` | iq (2) | 0–4 | rkfpv | venc | Exposure | Frames before auto-exposure reacts. 0 = fastest, may hunt under flickering light |
| `isp_ae_damp` | iq (45) | 0–100 | rkfpv | venc | Exposure | AE damping from bright to dark, × 0.01 |
| `isp_blc` | off | off, 1–100 | rkfpv | venc | Exposure | Backlight compensation: brighter subject against a bright sky (do not combine with `isp_hlc`) |
| `isp_hlc` | off | off, 1–100 | rkfpv | venc | Exposure | Highlight compensation: holds back bright areas such as the sky (do not combine with `isp_blc`) |
| `isp_nr_spatial` | off | iq, off, 1–100 | rkfpv | venc | Noise & detail | Spatial noise reduction. off = sharpest, noisier in low light |
| `isp_nr_temporal` | off | iq, off, 1–100 | rkfpv | venc | Noise & detail | Temporal noise reduction (3DNR). Smears / ghosts on fast motion |
| `isp_dehaze` | iq | iq, off, 1–100 | rkfpv | venc | Noise & detail | Dehaze strength (going back to iq needs a video restart) |
| `iq_dir` | /oem/usr/share/iqfiles | /oem/usr/share/iqfiles, /userdata/iq | rkfpv | video | IQ | IQ files: stock, or your edited copy in /userdata/iq. A custom file that fails to load falls back to stock |
| `dvr` | 0 | 0, 1 | rkfpv | boot | DVR | DVR on/off. Off = nothing allocated, saves RAM. On = record from FPV start |
| `dvr_segment` | 600 | 10–3600 | rkfpv | venc | DVR | Seconds per file; the next file starts at the next keyframe |
| `dvr_min_free` | 300 | 10–100000 | rkfpv | venc | DVR | MB to keep free on the card |
| `dvr_overwrite` | 1 | 0, 1 | rkfpv | venc | DVR | When the card is full: 1 = delete the oldest recordings, 0 = stop recording |
| `dvr_size` | link | link, 1920x1080, 1280x720, 2304x1296 | rkfpv | video | DVR | Recording resolution. link = record the radio stream (no extra RAM); a size = a second encoder (needs much more RAM) |
| `dvr_bitrate` | 8000 | 1000–20000 | rkfpv | venc | DVR | Recording bitrate, kbit/s |
| `dvr_rc` | vbr | vbr, cbr | rkfpv | venc | DVR | Recording rate control |
| `dvr_gop` | 30 | 1–120 | rkfpv | venc | DVR | Recording keyframe interval, frames. MP4 holds one GOP in RAM: keep it about 1 s |
| `dvr_format` | mp4 | mp4, ts | rkfpv | venc | DVR | mp4 = fragmented MP4 (plays after a power cut), ts = MPEG-TS; takes effect from the next file |

### #12 AE metering

| Key | Default | Values | File | Apply | Group | Description |
|---|---|---|---|---|---|---|
| `isp_metering` | iq | iq, average, bottom, bottom_strong | rkfpv | venc | Exposure | Which part of the picture auto-exposure exposes for. iq = the IQ file (center-weighted); bottom / bottom_strong = ground over sky, so a backlit horizon does not turn the ground black |

### #6 Hotspot and OSD (experimental)

| Key | Default | Values | File | Apply | Group | Description |
|---|---|---|---|---|---|---|
| `hotspot_ssid` | rkfpv-XXXX (first boot) | 1–32 characters | wfb `hotspot` | hotspot | Hotspot | Hotspot network name |
| `hotspot_pass` | random (first boot) | 8–63 characters | wfb `hotspot` | hotspot | Hotspot | WPA2 password |
| `hotspot_channel` | 36 | 36, 40, 44, 48, 149, 153, 157, 161, 1, 6, 11 | wfb `hotspot` | hotspot | Hotspot | 36–161 = 5 GHz (fast), 1 / 6 / 11 = 2.4 GHz (if 5 GHz does not start) |
| `hotspot_country` | US | two letters | wfb `hotspot` | hotspot | Hotspot | Country code for the radio rules (e.g. US, DE, CN): decides which 5 GHz channels may run an access point |
| `osd` | 0 | 0, 1 | rkfpv | video | Video | OSD burned into the video (flight mode, battery, link quality). Off until tested on hardware |

### #9 TX power in dBm

Replaces the three `txpower_8812au` / `txpower_8812cu` / `txpower_8812eu` keys with one key.

| Key | Default | Values | File | Apply | Group | Description |
|---|---|---|---|---|---|---|
| `txpower` | auto (8812AU / 8812CU 10, 8812EU 20) | 1–30 | wfb `wireless` | txpower | Radio | TX power, dBm, converted for each card. 8812AU: approximate. High power draws a lot of current |

### Adaptive link (branch `adaptive-link`, no pull request yet)

| Key | Default | Values | File | Apply | Group | Description |
|---|---|---|---|---|---|---|
| `link_control` | none | none, alink | wfb `wireless` | link | Radio | alink = MCS, FEC and bitrate follow the ground station's link score (PixelPilot, OpenIPC alink_gs). The ground station needs no change |
| `alink_max_bitrate` | 0 | 0–50000 | rkfpv | venc | Radio | Highest bitrate it may pick, kbit/s (0 = `bitrate`) |
| `alink_fallback_ms` | 1000 | 200–10000 | rkfpv | venc | Radio | No report from the ground for this long, ms → most robust profile |
| `alink_keyframe` | 1 | 0, 1 | rkfpv | venc | Radio | Send a keyframe when the ground station asks for one |

The profiles themselves (score range → MCS, FEC, bitrate) are in `/etc/txprofiles.conf`, in OpenIPC's format.
