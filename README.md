# rkfpv

RV1103 / RV1106（Luckfox Pico Mini、Pico Pro）上替代 majestic 的推流程序：

```
sensor → AIQ(ISP) → VI → VENC(H.265) → RTP/UDP → wfb_tx -u 5600 → 无线
```

地面端不用改，照常接收 UDP 5600 上的 H.265 RTP。

## 编译

```sh
make            # 交叉编译，产物 build/rkfpv（默认 SDK=../luckfox-pico，可用 make SDK=... 指定）
make test       # 在电脑上跑 RTP 封包单元测试
```

依赖 Luckfox SDK 里预编译好的 rockit / rkaiq / mpp / rga 和 uclibc 工具链，不需要先完整编一遍 SDK。

支持的板子：Pico Mini（RV1103，TF 卡启动，录像在卡上的 dvr 分区）和 Pico Pro（RV1106G2，SPI NAND 启动，录像用插在卡槽里的 TF 卡）。两块板用同一套程序，Pro 的编译和刷机见 `docs/build-steps.md` 第 16 节。

## 阶段 A：不接网卡，直接推到电脑

用原厂 Luckfox 固件即可，不用改内核。电脑通过 USB（RNDIS，板子默认地址 172.32.0.93）连到板子。

```sh
# 电脑上
scp build/rkfpv root@172.32.0.93:/usr/bin/       # 或 adb push build/rkfpv /usr/bin/
cd ..; ./play_video.sh h265                       # 先开播放端，监听 UDP 5600

# 板子上（adb shell / ssh）
killall rkipc                                      # rkipc 占着摄像头
rkfpv -s dst=172.32.0.100 -s port=5600             # dst 改成电脑在 RNDIS 网段的地址
```

每 5 秒会打印一行 `stats:`，内容是帧率、码率、最大帧和包速率。

**要检查的项：**
- 颜色正常。不正常就确认 `iq_dir` 下有你这颗 sensor 的 IQ 文件。
- 720p30 稳定，码率在设定值附近。
- 横移时看画面有没有错位。这时没有经过无线链路，如果仍然错位，问题就在相机或编码端。

## MAVLink 遥测（mavbridge）

飞控串口接 **UART3（/dev/ttyS3）**，用的是 UART3_M1 这组引脚：

| 板子引脚 | 功能 | 接飞控 |
|---|---|---|
| GPIO1_D0 | UART3_TX | 飞控 RX |
| GPIO1_D1 | UART3_RX | 飞控 TX |
| GND | 地 | GND |

两边都是 3.3V 电平，飞控那边的串口要设成 MAVLink、115200。

**阶段 A 测遥测：** 原厂固件里 UART3 默认是关的，先用 `luckfox-config` 把 UART3 打开（重启生效）。电脑上开 QGroundControl 监听 UDP 14550，然后在板子上运行：

```sh
mavbridge -d /dev/ttyS3 -b 115200 -o <电脑IP>:14550 -s 5
```

QGC 能连上飞控、看到姿态，就说明串口和转发都通了。`-s 5` 会每 5 秒打印一次统计；junk 字节数持续增长，说明波特率或协议没对上。

**接上 wfb 以后**，由 `wfb.sh` 自动启动，无线端口和 OpenIPC 一致，地面端不用改：

```
UART3 → mavbridge → udp:14551 → wfb_tx -p 16  ──无线──▶ 地面
UART3 ← mavbridge ← udp:14550 ← wfb_rx -p 144 ◀──无线── 地面
```

`mavbridge` 按 MAVLink 帧边界切分，把最多 15 帧、不超过 1000 字节打成一个 UDP 包，最多攒 30 ms 就发。这样丢一个无线包只会丢掉几条完整消息，不会出现半帧数据。遥测的 FEC 用 1/2（每包发两份），不用等攒满一组。

## 改设置：`fpv` 命令和电脑上的配置工具

板子上的 `fpv` 命令统一管理 `/etc/rkfpv.conf` 和 `/etc/wfb.yaml`。通过 `adb shell` 和 `ssh` 用法完全一样：

```sh
fpv get                         # 所有设置
fpv set bitrate 4000 mcs_index 3   # 先全部校验再写入；FPV 在跑时能热生效的会立即生效
fpv schema                      # 每个参数的范围、默认值、生效方式
fpv status                      # 模式、帧率/码率、无线参数、内存、温度
fpv log [rkfpv|rkfpv-prev|fpv|kernel]   # 日志存在 /userdata/log，断电重启后还在
fpv restart [video|link|all]    # 让需要重启的设置生效
fpv mode [auto|debug|fpv]       # 开机模式（fpv = 总是 FPV，ADB 不再出现，慎用）
fpv reset                       # 恢复出厂设置
```

加 `--json` 输出机器可读格式。新增参数只要在 `files/fpv` 的参数表里加一行。

**fpvd（UDP 命令服务）**：板子开机就在 UDP 7000 端口运行 `fpvd`（`src/fpvd.c`），只执行 `fpv` 的子命令（get/set/schema/status/log/restart/mode/reset/dvr/preview/hotspot/iq/blackbox，列表在 `src/fpvcmd.c`，和 dvrhttpd 共用），不给 shell，参数只允许安全字符。现在可以走 USB RNDIS（板子 172.32.0.93，电脑网卡手动设成 172.32.0.x），以后走 wfb 隧道（见 issue #1），不用 UART 也不占 USB 口。

协议很简单，按无线丢包设计：请求 `FPV1 <id> <命令> [参数…]`；回复按 1200 字节分片，每片格式 `FPV1 <id> <序号>/<总数>\n<JSON 片段>`。丢包时客户端用**同一个 id** 重发，fpvd 直接返回缓存的结果，不会重复执行（比如 restart 不会跑两次）。

**电脑上的配置工具**（类似 Betaflight Configurator）：板子接电脑开机进入调试模式后，运行

```sh
python3 configurator/rkfpvconf.py
```

浏览器里会打开界面，详见 `configurator/README.md`。

**网页版（手机不用装 App）**：同一套界面发布在 https://kkbin505.github.io/rkfpv/ 。手机连上板子的 WiFi 热点（见下面），用 Chrome 打开就能改设置、看实时画面、下载录像，竖屏布局。详见 `configurator/README.md` 的“网页版”。

直接改文件也可以：

```sh
vi /etc/rkfpv.conf
killall -HUP rkfpv
```

bitrate、max_bitrate、gop、rc、stat_time 不用重启就能生效（同时会请求一个 IDR）。size、fps、dst、pkt_size 改了要重启。

命令行用 `-s key=value` 覆盖配置文件，可以重复多次。

## 阶段 B：接 RTL8812AU + wfb-ng

需要：
1. 内核打开 cfg80211；`rv1103g-luckfox-pico-mini.dts` 里把 `dr_mode` 改成 `"otg"`（开机后由 S99fpv 切 host），`&uart3` 改成 `status = "okay"`
2. 网卡驱动（`luckfox/build-deps.sh` 编译，装到 `/oem/usr/ko/`，wfb.sh 按 USB ID 自动选）：`88XXau.ko`（8812AU）、`88x2cu.ko`（8812CU/8822CU）、`8812eu.ko`（8812EU）
3. 交叉编译 wfb-ng（`wfb_tx`、`wfb_tx_cmd`）

然后安装：

```
build/rkfpv      -> /usr/bin/rkfpv
build/mavbridge  -> /usr/bin/mavbridge
files/wfb.sh     -> /usr/bin/wfb.sh
files/wfb.yaml   -> /etc/wfb.yaml        （信道、link_id 要和地面端一致）
files/rkfpv.conf -> /etc/rkfpv.conf
files/S99fpv     -> /etc/init.d/S99fpv
drone.key        -> /etc/drone.key      （不进仓库，mkoverlay.sh 用 DRONE_KEY=... 指定）
```

### USB 角色与恢复

S99fpv 开机时判断一次：USB 插着电脑（gadget 被枚举）就保持 device 模式（ADB/RNDIS），
否则写 `host` 到 `/sys/class/usb_role/*/role`（dwc3 软件角色切换）启动 FPV。
**切成 host 后是强制的，之后再插电脑不会自动回到 ADB。** 恢复办法：

- 串口终端（UART2 调试口）：`echo device > /sys/class/usb_role/*/role`
- 或者 `touch /userdata/fpv.off` 后重启，插电脑开机
- 或者拔 TF 卡在电脑上删掉 `/etc/init.d/S99fpv`

### 供电

host 模式下 RTL8812AU 的 5V 取自板子 USB-C 的 VBUS，板上没有开关/限流。
8812AU 大功率发射电流较大，建议网卡由 BEC 单独供 5V（与板子共地），不要靠板子转供。

## 文件

| 文件 | 作用 |
|---|---|
| `src/main.c` | VI / VENC 初始化、主循环、SIGHUP 热重载 |
| `src/isp.c` | AIQ 启动，固定帧率，镜像和翻转 |
| `src/rtp_h265.c` | H.265 RTP 封包（RFC 7798，单 NAL 包 + FU 分片） |
| `src/config.c` | key=value 配置解析 |
| `src/mavbridge.c` | 串口 ↔ UDP 的 MAVLink 转发，按帧打包，不依赖其他库 |
| `src/fpvd.c` | UDP 命令服务，只执行 `fpv` 子命令 |
| `files/fpv` | 板子上的设置命令（get / set / status …） |
| `configurator/` | 电脑上的配置界面 |
| `test/` | 主机端单元测试；`send_file` 用来和 GStreamer 做端到端测试 |

## WiFi 热点模式（不插 USB 下载录像、改设置）

用板子上的 **BOOT 按键**，不用另外接开关。开机完成后：

- **按住 BOOT 约 2 秒**：视频和 wfb 停止，WiFi 网卡变成 WPA2 热点（`rkfpv-XXXX`，开机时自动生成名称和随机密码，调试模式下用 `fpv hotspot` 就能看到），板子地址是 10.0.0.1。
  - 手机或电脑连上热点后，用 Chrome 打开网页版配置工具（https://kkbin505.github.io/rkfpv/ ，先在有网时安装到主屏幕）：所有设置都能改，Live 页有实时画面（预览这次不录像），DVR 页可以下载、删除录像。
  - 电脑上的 `rkfpvconf.py` 选 **WiFi hotspot** 也一样能改设置、下载录像。
  - 浏览器直接打开 `http://10.0.0.1/`，是简单的录像下载页。
- **再按住 BOOT 约 2 秒**：回到 FPV（wfb 图传 + 录像）。
- 每次按住只切换一次，松开后才算下一次。开机过程中按键不起作用；上电时按住 BOOT 会进入烧录（maskrom）模式，不要这样做。板子接在电脑上（调试模式）时按键也不起作用。

`fpv hotspot status` 查看按键状态、热点名称、密码和已连接的设备。`hotspot_ssid` / `hotspot_pass` / `hotspot_channel` / `hotspot_country` 改的是下次开热点时的设置。5 GHz 热点能不能起来取决于网卡驱动（8812CU 目前不行，见 issue #16）。**只选天线支持的频段**：天线只适合 5.8 GHz 时不要用 1/6/11 信道（2.4 GHz），频段不对可能损坏网卡。热点起不来时会回到 FPV，不会自动换信道。

热点上的 HTTP 服务（`dvrhttpd`）只提供 `rec_*.mp4|ts|bbl` 文件（支持断点续传和拖动播放）、给网页版用的 `/api/fpv`（只执行白名单里的 `fpv` 子命令，不经过 shell）和 `/live`（实时画面）。改东西的请求必须带确认请求头，而且只对网页版的来源回应 CORS，所以别的网页没法借你的浏览器改设置或删卡上的文件。

需要的 SDK 改动（见 docs/build-steps.md）：buildroot 加 `hostapd` 和 `dnsmasq`。BOOT 按键接在 SARADC 通道 0 上，原厂 DTS 已经打开 ADC，不用改。

## 画质（ISP）和 IQ 文件

rkfpv 直接调用 rkaiq 的运行时接口改画质。majestic 在 Rockchip 上并没有调用这些接口。和其他设置一样，每一项都是 `fpv` 参数表里的一行，可以用 `fpv set` 修改，FPV 运行时立即生效：

| 设置 | 作用 |
|---|---|
| `isp_exp_max` | 最长曝光（毫秒），对运动模糊影响最大；IQ 文件里是 30 ms，FPV 常用 2～8 ms。低于 10 ms 时自动关闭防闪烁 |
| `isp_nr_temporal` / `isp_nr_spatial` | 时域 / 空域降噪，**默认关闭**：快速运动时不拖影，代价是低照度下噪点更多 |
| `isp_gain_max` | 最大增益（IQ 文件里是 512×） |
| `isp_ae_delay` / `isp_ae_damp` | 自动曝光的反应延迟和阻尼（从亮到暗） |
| `isp_blc` / `isp_hlc` | 逆光补偿 / 强光抑制 |
| `isp_brightness` `contrast` `saturation` `hue` `sharpness` `wb` `dehaze` `antiflicker` | 颜色、锐度、白平衡等 |

设成 `iq` 的项用 IQ 文件里的值。rkfpv 启动时会读出 IQ 文件的原值，所以把某项改回 `iq` 能立即恢复。

**IQ 文件**：出厂文件（`/oem/usr/share/iqfiles`）永远只读。修改的是 `/userdata/iq` 里的副本：
```sh
fpv iq status                      # 当前用的是哪个、是否回退
fpv iq copy-stock                  # 复制出厂文件作为修改起点（不会覆盖已改过的文件）
fpv set iq_dir /userdata/iq        # 使用修改版（需要 fpv restart video）
fpv iq history / rollback <文件> [版本]
```
配置工具的 **Image** 页可以下载、上传 IQ 文件。上传会先检查：JSON 格式、rkaiq 必需的段、文件名和出厂文件一致；然后按原样逐字节写入（保留 CRLF），在板子上校验 md5，没问题才替换，旧版本自动保留最近 5 个。修改版启动失败时，或者连续两次没有出图时，rkfpv 自动改用出厂 IQ，保证一定有图像。

## DVR：录像到 TF 卡

> **默认关闭（`dvr = 0`）。** 在板子上实测：64 MB 内存里 24 MB 是视频专用的 CMA，内核占约 8 MB，FPV 跑起来以后普通内存只剩约 2 MB。开着 1080p 第二路编码，板子会因为内存耗尽卡死。`dvr = 0` 时 rkfpv 不创建任何录像相关的东西（第二路编码器、缓冲区、复用器都不分配）。打开时（`fpv set dvr 1`，重启 FPV 生效）默认 `dvr_size = link`，只录图传那一路，不额外编码。

ISP 有两个硬件缩放输出：主通路（main path）可以出 1080p，用单独的编码器、更高码率录到 TF 卡；副通路（self path）出 720p，给图传用。`dvr_size = 1920x1080` 时就是下面这样，**需要的内存比现在板子上空出来的多，目前不能用**：

```
sensor → ISP ─ 主通路 VI 0 (1920x1080) → VENC 1 (H.265 VBR 8 Mbit/s, GOP 1 s) → DVR
            └─ 副通路 VI 1 (1280x720)  → VENC 0 (H.265 CBR 3.5 Mbit/s)       → RTP → wfb-ng
```

设置项：`dvr_size`、`dvr_bitrate`、`dvr_rc`、`dvr_gop`。设成 `dvr_size = link` 时，直接录图传那路码流，不额外编码。第二路创建失败（比如 CMA 不够）时，会自动退回录图传那路。

**内存**：高码率录像容易把 64MB 的板子撑爆（majestic 上就遇到过）。这里录像占用的内存基本固定在 2～3 MB，不随码率增长：
- fMP4 每个分段会在内存里攒一个 GOP，所以录像码流单独设 1 秒 GOP，8 Mbit/s 时约 1 MB。TS 格式不攒分段，内存最省。
- 每秒执行一次 `syncfs`，并把已经写完的数据从页缓存里清掉。`S99fpv` 还把内核允许的待写数据上限降到 4 MB（默认是内存的 20%）。
- 编码器和摄像头的缓冲区在 24 MB 的 CMA 预留区里，和普通内存分开（单路 720p 实测用掉约 11 MB CMA）。但普通内存才是瓶颈，见上面。
- `fpv dvr status` 和配置工具的 DVR 页会显示剩余内存。

- **分区**：`luckfox/mkoverlay.sh` 把分区表改成 `…,256M(userdata),512M(rootfs),-(dvr)`，卡上剩余空间全部给 DVR。首次开机 `S99fpv` 会把它格式化成 ext4，并挂载到 `/mnt/dvr`。只会格式化内核报告名字为 `dvr` 的分区。**改了分区表，需要重新烧卡。**
- **格式**：默认 fMP4（分段 MP4），断电后文件仍能播放，最多丢最后 1～2 秒；也可以选 TS。复用 SDK 的 `librkmuxer`，rkipc 录像用的也是它。
- **切文件**：默认每 10 分钟一个文件，到时间后在下一个关键帧切换。文件名是 `rec_会话_序号.mp4`，例如 `rec_0003_01.mp4`。板子没有实时时钟，所以每次开始录像算一个新会话，会话号递增。
- **空间**：卡上剩余空间少于 `dvr_min_free` 时，自动删除最旧的录像（`dvr_overwrite = 1`），或者停止录像（`= 0`）。
- **控制**：`dvr = 1` 时开机自动录像；`fpv dvr start|stop`，或者配置工具 DVR 页的按钮，随时开始或停止。
- **取文件不用拔卡**：板子接电脑开机进入调试模式，DVR 分区同样会挂载。在配置工具的 DVR 页点 **Download**，或者手动执行 `adb pull /mnt/dvr/rec_0003_01.mp4`。

```sh
fpv dvr status            # 状态、当前文件、剩余空间
fpv dvr list              # 卡上的录像
fpv dvr delete rec_0003_01.mp4
fpv dvr delete-all        # 正在录的那个文件不会删
```

H.265 的 MP4 用 VLC 能直接播放；Windows 自带播放器需要安装 "HEVC 视频扩展"。

### Betaflight 黑盒（给 Gyroflow 增稳）

飞控的 blackbox 设成串口输出（`blackbox_device = SERIAL`），TX 接板子的 UART4 RX。`bblog` 把每次解锁的日志存成和录像同名的 `rec_SSSS_NN.bbl`，和录像一起下载、一起按旧到新清理。`dvr_osd = 0` 让录像不烧 OSD。

不接飞控也能先验证串口：把 UART4 的 TX 和 RX 短接（Mini 排针 10–11，Pro 排针 6–7），运行 `fpv blackbox selftest`。接线和 BF 设置见 [docs/blackbox.md](docs/blackbox.md)。

## 致谢

这个项目站在很多开源项目的肩膀上，感谢：

- **[OpenIPC](https://github.com/OpenIPC)**：让 IP 摄像头芯片能做数字图传的思路，以及 `wifibroadcast` 脚本、网卡驱动的整合与补丁、TX 功率刻度、各种参数经验。`files/wfb.sh` 就是从 OpenIPC 的脚本精简来的。
- **[wfb-ng](https://github.com/svpcom/wfb-ng)**（svpcom）：整个无线链路，包括广播、FEC、加密和遥测通道。
- **[Luckfox](https://github.com/LuckfoxTECH/luckfox-pico)** 和 **Rockchip**：Luckfox Pico SDK、RKMPI / rkaiq 媒体库和示例代码。
- **Realtek 网卡驱动的维护者**：[openipc/realtek-wlan](https://github.com/openipc/realtek-wlan)（RTL8812AU FPV 分支）、[libc0607](https://github.com/libc0607) 的 RTL8812CU/8822CU 和 RTL8812EU 驱动、[svpcom/rtl8812au](https://github.com/svpcom/rtl8812au)。
- **PixelPilot** 等 OpenIPC 地面站项目：本项目的视频格式和它们保持兼容。
- 界面风格参考了 **[Betaflight Configurator](https://github.com/betaflight/betaflight-configurator)**。

## 许可证

本仓库自己的代码使用 [MIT](LICENSE) 许可证。从第三方项目改编的文件、第三方补丁、构建时下载的组件，以及各自的许可证，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

如果要分发编译好的固件镜像，注意镜像里包含 GPL 软件（内核、网卡驱动、wfb-ng），必须同时提供对应的源码，详见 THIRD_PARTY_NOTICES.md。
