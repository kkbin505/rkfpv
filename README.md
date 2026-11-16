# rkfpv

RV1103 (Luckfox Pico) 上替代 majestic 的推流程序：

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

## 运行时调参

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
否则写 `host` 到 `/sys/devices/platform/ff3e0000.usb2-phy/otg_mode` 启动 FPV。
**切成 host 后是强制的，之后再插电脑不会自动回到 ADB。** 恢复办法：

- 串口终端（UART2 调试口）：`echo otg > /sys/devices/platform/ff3e0000.usb2-phy/otg_mode`
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
| `test/` | 主机端单元测试；`send_file` 用来和 GStreamer 做端到端测试 |
