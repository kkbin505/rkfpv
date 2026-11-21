# 设置项一览

[English](settings.en.md)

这里列出天空端的全部设置，和 `fpv` 命令、电脑配置工具里看到的一致。来源是 `files/fpv` 里的参数表（`params()`）：只有表里有的才算一个设置。`test/test_fpv.sh` 会检查表里的每一项在本文的中英文两个版本里都有。

## 怎么改

```sh
fpv get                       # 所有当前值
fpv get bitrate mcs_index     # 只看其中几项
fpv set bitrate 4000 gop 60   # 先检查全部值，再写入，能实时生效的立即生效
fpv restart                   # 让需要重启的改动生效（只重启需要的部分）
fpv schema                    # 板上自带的这张表
fpv reset                     # 恢复出厂默认
```

电脑配置工具也是按这张表生成界面的，下面每个分组对应页面上的一块。

设置存在两个文件里：
- **文件**列是 `rkfpv` 的：`/etc/rkfpv.conf`（`key = value`）。
- **文件**列是 `wfb` 的：`/etc/wfb.yaml`，在所写的段下面（`key: value`）。

### 什么时候生效

| 生效方式 | 何时生效 | 怎么做到 |
|---|---|---|
| `venc` | 立即 | rkfpv 重读配置（SIGHUP） |
| `radio` | 立即 | `wfb_tx_cmd set_radio` |
| `fec` | 立即 | `wfb_tx_cmd set_fec` |
| `txpower` | 立即 | `iw set txpower` |
| `video` | `fpv restart video` 之后 | 重启 rkfpv（画面中断约 2 秒） |
| `link` | `fpv restart link` 之后 | 重启 wfb-ng；**地面站要设成同样的值** |
| `boot` | 下次开机 | |

默认值写 `auto` 的，表示这一项不写进文件（在文件里是注释掉的），用驱动或网卡的默认值。

## `stable` 里已有的设置

### 视频（Video）

| 设置项 | 默认 | 取值 | 文件 | 生效 | 说明 |
|---|---|---|---|---|---|
| `size` | 1280x720 | 1280x720、1920x1080、2304x1296 | rkfpv | video | 采集分辨率 |
| `fps` | 30 | 1–60 | rkfpv | video | 帧率 |
| `rc` | cbr | cbr、vbr | rkfpv | venc | 码率控制：cbr 恒定码率，vbr 可变码率 |
| `bitrate` | 3500 | 100–50000 | rkfpv | venc | 目标码率，kbit/s |
| `max_bitrate` | 0 | 0–50000 | rkfpv | venc | VBR 的峰值码率，kbit/s（0 = 编码器默认） |
| `gop` | 30 | 1–600 | rkfpv | venc | 关键帧间隔，帧数（等于 fps 就是每秒一个关键帧） |
| `stat_time` | 1 | 1–60 | rkfpv | venc | 码率统计窗口，秒（越短峰值越小） |
| `slice_bytes` | 0 | 0–65536 | rkfpv | video | 每个 slice 的字节数（0 = 每帧一个 slice；1300 = 每个包一个 slice） |
| `intra_refresh` | 0 | 0–64 | rkfpv | video | 滚动帧内刷新，每帧刷新的行数（0 = 关，每 `gop` 帧一个 IDR） |
| `pkt_size` | 1400 | 200–4000 | rkfpv | video | RTP 包大小，字节 |
| `pt` | 97 | 96–127 | rkfpv | video | RTP payload type（97 = OpenIPC 地面站用的 H.265） |

### 摄像头（Camera）

| 设置项 | 默认 | 取值 | 文件 | 生效 | 说明 |
|---|---|---|---|---|---|
| `mirror` | 1 | 0、1 | rkfpv | video | 左右镜像 |
| `flip` | 1 | 0、1 | rkfpv | video | 上下翻转（镜像 + 翻转 = 旋转 180°） |

### 无线（Radio）

| 设置项 | 默认 | 取值 | 文件 | 生效 | 说明 |
|---|---|---|---|---|---|
| `channel` | 161 | 1–196 | wfb `wireless` | link | WiFi 信道（地面站要一致） |
| `width` | 20 | 20、40 | wfb `wireless` | link | 信道带宽，MHz |
| `txpower_8812au` | auto（20） | 1–63 | wfb `wireless` | txpower | RTL8812AU 发射功率：功率索引，每级约 0.5 dB，63 最大 |
| `txpower_8812cu` | auto（20 = 10 dBm） | 1–63 | wfb `wireless` | txpower | RTL8812CU / 8822CU 发射功率：数值 / 2 = dBm |
| `txpower_8812eu` | auto（40 = 20 dBm） | 1–63 | wfb `wireless` | txpower | RTL8812EU 发射功率：数值 / 2 = dBm；功率高时电流很大 |
| `mcs_index` | 2 | 0–7 | wfb `broadcast` | radio | 调制方式：0 最慢、距离最远 … 7 最快 |
| `stbc` | 0 | 0、1 | wfb `broadcast` | radio | STBC（需要 2 根发射天线） |
| `ldpc` | 0 | 0、1 | wfb `broadcast` | radio | LDPC 编码 |
| `fec_k` | 8 | 1–32 | wfb `broadcast` | fec | 每个 FEC 块里的数据包数 |
| `fec_n` | 12 | 2–32 | wfb `broadcast` | fec | 每个 FEC 块的总包数，含恢复包（必须大于 `fec_k`） |
| `link_id` | 7669206 | 0–16777215 | wfb `broadcast` | link | 链路 ID（地面站要一致） |

三个 `txpower_*` 里只有检测到的那张网卡对应的一项生效，另外两项不起作用。

### 遥测（Telemetry）

| 设置项 | 默认 | 取值 | 文件 | 生效 | 说明 |
|---|---|---|---|---|---|
| `telemetry_enabled` | true | true、false | wfb `telemetry` | link | 通过 wfb-ng 传 MAVLink 遥测 |
| `baud` | 115200 | 57600、115200、230400、460800、921600 | wfb `telemetry` | link | 飞控串口波特率 |

## 还在 PR 里的设置（尚未合并到 `stable`）

这些设置在合并之前还可能改动。哪个 PR 合并了，就把它的几行挪到上面一节。

### #5 DVR 和图像页

默认值写 `iq` 的，表示沿用 IQ（传感器调校）文件里的值，不设置就不改变任何东西。`off` 表示关闭。

| 设置项 | 默认 | 取值 | 文件 | 生效 | 分组 | 说明 |
|---|---|---|---|---|---|---|
| `isp_brightness` | iq | 0–255 | rkfpv | venc | 画面 | 亮度，128 = 不调整 |
| `isp_contrast` | iq | 0–255 | rkfpv | venc | 画面 | 对比度，128 = 不调整 |
| `isp_saturation` | iq | 0–255 | rkfpv | venc | 画面 | 饱和度，128 = 不调整（0 = 黑白） |
| `isp_hue` | iq | 0–255 | rkfpv | venc | 画面 | 色调，128 = 不调整 |
| `isp_sharpness` | iq | 0–100 | rkfpv | venc | 画面 | 锐度（数值高时噪点也会被锐化） |
| `isp_wb` | auto | auto、daylight、cloudy、shade、twilight、fluorescent、incandescent | rkfpv | venc | 画面 | 白平衡 |
| `isp_exp_max` | iq（30） | 1–33 | rkfpv | venc | 曝光 | 最长曝光时间，ms。越短运动模糊越少，但暗光下更暗、噪点更多；FPV 常用 2–8。低于 10 ms 时抗闪烁自动关闭 |
| `isp_gain_max` | iq（512） | 1–512 | rkfpv | venc | 曝光 | 最大增益，倍。越低夜间噪点越少，画面越暗 |
| `isp_antiflicker` | iq | iq、off、50、60 | rkfpv | venc | 曝光 | 抗灯光闪烁，Hz（室内用）；曝光时间需按 10 / 8.3 ms 取整 |
| `isp_ae_delay` | iq（2） | 0–4 | rkfpv | venc | 曝光 | 自动曝光过几帧才响应。0 最快，在闪烁灯光下可能来回跳 |
| `isp_ae_damp` | iq（45） | 0–100 | rkfpv | venc | 曝光 | 从亮到暗时的自动曝光阻尼，× 0.01 |
| `isp_blc` | off | off、1–100 | rkfpv | venc | 曝光 | 背光补偿：在明亮天空下把主体提亮（不要和 `isp_hlc` 同时开） |
| `isp_hlc` | off | off、1–100 | rkfpv | venc | 曝光 | 强光抑制：压暗天空等高亮区域（不要和 `isp_blc` 同时开） |
| `isp_nr_spatial` | off | iq、off、1–100 | rkfpv | venc | 降噪与细节 | 空域降噪。off 最清晰，暗光下噪点多 |
| `isp_nr_temporal` | off | iq、off、1–100 | rkfpv | venc | 降噪与细节 | 时域降噪（3DNR）。快速运动时会拖影 |
| `isp_dehaze` | iq | iq、off、1–100 | rkfpv | venc | 降噪与细节 | 去雾强度（改回 iq 需要重启视频） |
| `iq_dir` | /oem/usr/share/iqfiles | /oem/usr/share/iqfiles、/userdata/iq | rkfpv | video | IQ | IQ 文件：原厂的，或放在 /userdata/iq 里自己改过的。自定义文件加载失败时退回原厂 |
| `dvr` | 0 | 0、1 | rkfpv | boot | DVR | DVR 开关。关（默认）时不占内存；开时从进入 FPV 模式起就录 |
| `dvr_segment` | 600 | 10–3600 | rkfpv | venc | DVR | 每个文件的秒数；到时间后在下一个关键帧换新文件 |
| `dvr_min_free` | 300 | 10–100000 | rkfpv | venc | DVR | 卡上至少保留的空间，MB |
| `dvr_overwrite` | 1 | 0、1 | rkfpv | venc | DVR | 卡满时：1 = 删掉最旧的录像，0 = 停止录像 |
| `dvr_size` | link | link、1920x1080、1280x720、2304x1296 | rkfpv | video | DVR | 录像分辨率。link = 录无线传输的那路码流（不额外占内存）；写分辨率 = 再开一个编码器（要多占很多内存） |
| `dvr_bitrate` | 8000 | 1000–20000 | rkfpv | venc | DVR | 录像码率，kbit/s |
| `dvr_rc` | vbr | vbr、cbr | rkfpv | venc | DVR | 录像的码率控制 |
| `dvr_gop` | 30 | 1–120 | rkfpv | venc | DVR | 录像的关键帧间隔，帧数。MP4 要在内存里缓存一个 GOP，保持 1 秒左右 |
| `dvr_format` | mp4 | mp4、ts | rkfpv | venc | DVR | mp4 = 分段 MP4（断电后也能播放），ts = MPEG-TS；从下一个文件开始生效 |

### #12 测光

| 设置项 | 默认 | 取值 | 文件 | 生效 | 分组 | 说明 |
|---|---|---|---|---|---|---|
| `isp_metering` | iq | iq、average、bottom、bottom_strong | rkfpv | venc | 曝光 | 自动曝光以画面哪一部分为准。iq = 按 IQ 文件（中央重点）；bottom / bottom_strong = 重视地面而不是天空，逆光时地面不会黑掉 |

### #6 热点和 OSD（实验性）

| 设置项 | 默认 | 取值 | 文件 | 生效 | 分组 | 说明 |
|---|---|---|---|---|---|---|
| `hotspot_ssid` | rkfpv-XXXX（首次开机生成） | 1–32 个字符 | wfb `hotspot` | hotspot | 热点 | 热点名称 |
| `hotspot_pass` | 随机（首次开机生成） | 8–63 个字符 | wfb `hotspot` | hotspot | 热点 | WPA2 密码 |
| `hotspot_channel` | 36 | 36、40、44、48、149、153、157、161、1、6、11 | wfb `hotspot` | hotspot | 热点 | 36–161 = 5 GHz（快），1 / 6 / 11 = 2.4 GHz（5 GHz 起不来时用） |
| `hotspot_country` | US | 两个字母 | wfb `hotspot` | hotspot | 热点 | 国家代码，如 US、DE、CN，决定哪些 5 GHz 信道可以开热点 |
| `osd` | 0 | 0、1 | rkfpv | video | 视频 | 把 OSD 叠加进画面（飞行模式、电池、链路质量）。上板测试前默认关闭 |

### #9 发射功率改用 dBm

用一个 `txpower` 代替 `txpower_8812au` / `txpower_8812cu` / `txpower_8812eu` 三项。

| 设置项 | 默认 | 取值 | 文件 | 生效 | 分组 | 说明 |
|---|---|---|---|---|---|---|
| `txpower` | auto（8812AU / 8812CU 为 10，8812EU 为 20） | 1–30 | wfb `wireless` | txpower | 无线 | 发射功率，dBm，按网卡分别换算。8812AU 是近似值。功率高时电流很大 |

### 自适应链路（`adaptive-link` 分支，还没开 PR）

| 设置项 | 默认 | 取值 | 文件 | 生效 | 分组 | 说明 |
|---|---|---|---|---|---|---|
| `link_control` | none | none、alink | wfb `wireless` | link | 无线 | alink = MCS、FEC 和码率跟随地面站发来的链路分数（PixelPilot、OpenIPC alink_gs）。地面站不用改 |
| `alink_max_bitrate` | 0 | 0–50000 | rkfpv | venc | 无线 | 自适应时允许的最高码率，kbit/s（0 = 用 `bitrate`） |
| `alink_fallback_ms` | 1000 | 200–10000 | rkfpv | venc | 无线 | 超过这么久（ms）收不到地面站的消息，就切到最稳的一档 |
| `alink_keyframe` | 1 | 0、1 | rkfpv | venc | 无线 | 地面站请求时发一个关键帧 |

各档位（分数范围 → MCS、FEC、码率）写在 `/etc/txprofiles.conf` 里，格式和 OpenIPC 相同。
