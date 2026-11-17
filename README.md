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

**fpvd（UDP 命令服务）**：板子开机就在 UDP 7000 端口运行 `fpvd`（`src/fpvd.c`），只执行 `fpv` 的子命令（get/set/schema/status/log/restart/mode/reset），不给 shell，参数只允许安全字符。现在可以走 USB RNDIS（板子 172.32.0.93，电脑网卡手动设成 172.32.0.x），以后走 wfb 隧道（见 issue #1），不用 UART 也不占 USB 口。

协议很简单，按无线丢包设计：请求 `FPV1 <id> <命令> [参数…]`；回复按 1200 字节分片，每片格式 `FPV1 <id> <序号>/<总数>\n<JSON 片段>`。丢包时客户端用**同一个 id** 重发，fpvd 直接返回缓存的结果，不会重复执行（比如 restart 不会跑两次）。

**电脑上的配置工具**（类似 Betaflight Configurator）：板子接电脑开机进入调试模式后，运行

```sh
python3 configurator/fpvconf.py
```

浏览器里会打开界面，详见 `configurator/README.md`。

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
