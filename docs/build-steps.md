# rkfpv: steps from zero to a working link

Luckfox Pico Mini A (RV1103, TF-card boot) + SC3336 → rkfpv (H.265) → wfb-ng → ground station.
Status 2026-11-17: 720p30 end to end, 0 lost packets, RSSI −36…−44 dBm on the bench.

```
SC3336 → AIQ/ISP → VI → VENC H.265 → RTP (PT 97) → udp:5600 → wfb_tx → 8812CU ~~~ 8812AU → wfb_rx → udp:5600 → GStreamer
UART3 ↔ mavbridge ↔ wfb ports 16/144 (MAVLink)
```

---

## 1. Host build environment (Debian 13)

```sh
sudo apt install flex bison gawk expect cmake lz4 build-essential git
```

## 2. Luckfox SDK

```sh
cd ~/github
git clone --depth 1 https://github.com/LuckfoxTECH/luckfox-pico
cd luckfox-pico
./build.sh lunch      # RV1103_Luckfox_Pico_Mini, SD card, Buildroot
```

### 2.1 SDK edits (local, not upstream)

| File | Change | Why |
|---|---|---|
| Mini DTS, `&usbdrd_dwc3` | `dr_mode = "otg"; usb-role-switch; role-switch-default-mode = "peripheral";` | one USB port: device for ADB, host for the WiFi card |
| Mini DTS | `&uart3 { status = "okay"; }` | MAVLink to the flight controller |
| `luckfox_rv1106_linux_defconfig` | `CONFIG_CFG80211`, `WIRELESS`, `WLAN`, `WEXT` on | Realtek monitor-mode drivers need cfg80211 |
| same | `# CONFIG_USB_MASS_STORAGE is not set` | built-in `g_mass_storage` grabbed the UDC and broke ADB ("dwc3 failed to add gadget") |
| `drivers/usb/dwc3/drd.c` | `dwc3_role_switch.allow_userspace_control = true;` | otherwise no writable `/sys/class/usb_role/*/role` |
| `luckfox_pico_defconfig` (buildroot) | `IW`, `LIBPCAP`, `LIBSODIUM`, `USBUTILS`, `WIRELESS_REGDB` | wfb-ng deps + tools |
| buildroot `package/libffi/0004-...patch` | declare `open_temp_exec_file` | host-libffi fails with GCC 14 |
| Mini BoardConfig `RK_POST_OVERLAY` | add `overlay-rkfpv` | our files go into the image |

```sh
./build.sh            # first full build (kernel + buildroot + oem)
```

## 3. rkfpv repo

```sh
git clone https://github.com/kkbin505/rkfpv ~/github/rkfpv
cd ~/github/rkfpv
make                  # build/rkfpv + build/mavbridge (uClibc toolchain, rpath /oem/usr/lib)
make test             # RTP packetizer unit test on the PC
```

## 4. wfb-ng + WiFi drivers for the board

```sh
luckfox/build-deps.sh     # -> luckfox/out/
```

Builds, at pinned versions (same as OpenIPC → ground stations stay compatible):

| Output | Source | For |
|---|---|---|
| `wfb_tx`, `wfb_rx`, `wfb_tx_cmd` | wfb-ng `ae2122c1` | link |
| `88XXau.ko` | openipc/realtek-wlan `rtl8812au_fpv` + 2 patches | RTL8812AU |
| `88x2cu.ko` | libc0607 rtl88x2cu | RTL8812CU / 8822CU |
| `8812eu.ko` | libc0607 rtl88x2eu + cflags patch | RTL8812EU |

Drivers are built through kbuild (`make -C $KSRC M=...`), not the driver's own Makefile.

## 5. Overlay + image

```sh
DRONE_KEY=~/github/openipc/keys/drone.key luckfox/mkoverlay.sh
cd ../luckfox-pico && ./build.sh      # -> output/image/sd_update.img
```

Overlay contents:
- `/usr/bin/rkfpv`, `mavbridge`, `wfb_tx`, `wfb_rx`, `wfb_tx_cmd`, `wfb.sh`
- `/etc/init.d/S99fpv`, `/etc/rkfpv.conf`, `/etc/wfb.yaml`, `/etc/drone.key`
- `/oem/usr/ko/{88XXau,88x2cu,8812eu}.ko`

The key is copied in at build time and never committed (`*.key` in `.gitignore`).

## 6. Flash the TF card

```sh
lsblk                                   # find the card, e.g. /dev/sdc — double check!
sudo dd if=output/image/sd_update.img of=/dev/sdc bs=4M conv=fsync status=progress
sync
```

## 7. Boot logic (`S99fpv`)

1. `/userdata/fpv.off` → do nothing. `/userdata/fpv.force` → always FPV.
2. Wait up to 10 s for a PC: UDC state `default|addressed|configured` → stay **device** (ADB works, FPV off).
3. Otherwise `echo host > /sys/class/usb_role/*/role` → `wfb.sh start` → start rkfpv (log `/var/log/rkfpv.log`).

`wfb.sh` detects the adapter by USB ID, loads the right driver
(non-88XXau get `rtw_tx_pwr_by_rate=0 rtw_tx_pwr_lmt_enable=0 MaxTxBufLen=32`),
sets monitor mode, starts `wfb_tx -u 5600`, telemetry `wfb_tx -p 16` / `wfb_rx -p 144`, and mavbridge on `/dev/ttyS3`.

Back to ADB from FPV mode: `echo device > /sys/class/usb_role/*/role`.

## 8. Debugging that was needed

| Symptom | Cause | Fix |
|---|---|---|
| rkfpv: libs not found at boot | librockit lives in `/oem/usr/lib`, uClibc only searches `/lib`, `/usr/lib` | `-Wl,-rpath,/oem/usr/lib` + source `RkEnv.sh` |
| No ADB | built-in `g_mass_storage` bind failed (-22) | disable `CONFIG_USB_MASS_STORAGE` |
| "no USB role switch found" | role file not writable | `allow_userspace_control` in drd.c |
| Switched to host while Windows was enumerating | checked only `configured`, too quickly | 10 s window, accept `default/addressed` |
| rkfpv SIGSEGV ~3.5 s into streaming | pack read with `u32Offset` | read packs like rkipc (`Handle2VirAddr`, `u32Len`), crash handler logs stage |
| GS shows no video | RTP PT 96 | PT 97 (what majestic uses for H.265) |
| Upside-down image | camera mount | `mirror = 1`, `flip = 1` |
| Serial EIO | flaky CP210x | replug; udev rule `MODE=0666` |

Serial console: `/dev/ttyUSB0`, 115200, login `root` / `luckfox`.

## 9. Parameters in use

**rkfpv.conf**: 1280x720@30, H.265 CBR 3500 kbps, GOP 30, stat_time 1, pkt_size 1400, PT 97, slices / intra refresh off, mirror+flip.

**wfb.yaml**: channel 161, 20 MHz, MCS 2, FEC 8/12, no STBC/LDPC, link_id 7669206, txpower = driver default (unset); telemetry ttyS3 @ 115200, FEC 1/2.

## 10. Bench ground station (Linux laptop + RTL8812AU)

The OpenIPC `rtl8812au_fpv` driver hangs nl80211 on kernel 6.12 → use svpcom `rtl8812au` v5.2.20 (module `88XXau_wfb`).

```sh
gs/build.sh                          # once: driver + host wfb_rx into gs/build/
sudo gs/gs_start.sh [channel]        # load driver, monitor mode (verified, retried), start wfb_rx
gs/stats.sh                          # live: pkts, lost, fec, bad, Mbps, RSSI/SNR per antenna
gs/view.sh                           # GStreamer viewer, run as normal user (not root)
sudo pkill wfb_rx; sudo rmmod 88XXau_wfb   # stop
```

Viewer pipeline: `udpsrc :5600 (payload=97) → rtpjitterbuffer latency=30 → rtph265depay → h265parse → avdec_h265 → xvimagesink sync=false`.

Result: 1280x720 @ 30 fps, 0 lost, a few FEC recoveries/s, SNR 23–26 dB.

## 11. Open items

1. Check whether panning still tears (try `slice_bytes = 1300`, `intra_refresh`).
2. OOM at boot kills the telemetry `wfb_rx` (~2 MB free) → trim services (dhcpcd, ntpd, telnet…).
3. Reflash: card still has the old S99fpv.
4. Push the repo.
5. Move SDK edits into Mini-only files / fork luckfox-pico.
6. Later: wfb_tun + SSH over the link, WiFi hot-plug re-detect, 8812CU TX power tuning.

## 12. Settings tool: `fpv` CLI + PC configurator

- Board: `fpv get | set k v … | schema | status | log | restart | mode | reset` (add `--json`). The parameter table in `files/fpv` is the single list of settings.
- Logs now persist in `/userdata/log` (`rkfpv.log`, `rkfpv.prev.log`, `fpv.log`) so they survive the reboot into ADB mode.
- PC: `python3 configurator/fpvconf.py` → Betaflight-style UI in the browser, talks to the board via `adb shell fpv … --json`.
- Before reflashing: `adb push files/fpv /usr/bin/` (or the UI's *Install / update on board*).
- Tests: `test/test_fpv.sh` (dash + busybox sh), part of `make test`.

## 13. DVR (recording to the TF card)

- rkfpv records the same H.265 stream it transmits, through the SDK's `librkmuxer`. The file is fragmented MP4 (default) or TS, on the `dvr` partition mounted at `/mnt/dvr`.
- New partition table (set by `luckfox/mkoverlay.sh`): `…,256M(userdata),512M(rootfs),-(dvr)`. **Reflash needed.** First boot formats the DVR partition as ext4.
- Settings: `dvr`, `dvr_segment` (600 s), `dvr_min_free` (300 MB), `dvr_overwrite`, `dvr_format`. Control: `fpv dvr status|start|stop|list|delete|delete-all`.
- Download without removing the card: boot with the PC on USB (debug mode), then use the configurator's DVR tab or `adb pull /mnt/dvr/<file>`.

## 14. Image (ISP) settings and IQ files

- `isp_*` settings call rkaiq at runtime (exposure cap, gain cap, AE delay/damping, NR, BLC/HLC, colour, sharpness, WB, dehaze, anti-flicker). `iq` = the IQ file's value; spatial and temporal NR default to **off**.
- IQ files: stock `/oem/usr/share/iqfiles` is read-only; edits live in `/userdata/iq` (`fpv iq copy-stock|install|history|rollback`, `iq_dir`). Uploads are validated, md5-checked and swapped atomically; the last 5 versions are kept. A custom IQ that fails, or two starts in a row without a frame, falls back to stock.
