# 扩展接口

rkfpv 留了几个接口，让自己写的程序（目标识别、AprilTag、控制、第三方 OSD、地面站工具……）作为**单独的进程**接进来，不用改 rkfpv 的代码，也不用跟着 rkfpv 一起维护分支：

- 扩展程序崩溃、卡住或内存不够被杀，图传照常工作；
- 升级 rkfpv 时扩展程序不用合并代码；
- 扩展程序可以用任何语言写，许可证也随你（rkfpv 是 MIT）。

所有接口默认关闭，关闭时 rkfpv 的行为和没有这些接口时完全一样。

## 插件目录：跟着视频启停

`/etc/fpv/plugins.d/` 里每个可执行的 `S<NN>名字` 脚本就是一个插件，写法和 `/etc/init.d` 的脚本一样，收一个参数：

| 参数 | 什么时候 |
|---|---|
| `start` | 视频启动之后（开机进入 FPV、`fpv restart video`、电脑预览、热点预览） |
| `stop` | 视频停止之前 |
| `status` | `fpv plugins`，输出的第一行显示出来 |

- `start` 按文件名顺序，`stop` 倒序。
- 每次调用限时 5 秒，超时会被杀掉。长期运行的程序要放到后台再返回，比如 `start-stop-daemon -S -b -m -p /var/run/xxx.pid -x /usr/bin/xxx`。
- 失败只记日志（`fpv log fpv`），不影响别的插件和图传。
- 环境变量 `FPV_MODE` 是当前模式：`fpv`、`debug`、`hotspot`。比如只在真正飞行时才启动控制程序：`[ "$FPV_MODE" = fpv ] || exit 0`。
- 插件和它启动的程序 `oom_score_adj` 是 500：内存不够时内核先杀插件，图传进程是 -900。Mini 只有 64 MB 内存，FPV 模式下剩不到几 MB，用 `fpv status` 看 `memory`。
- `start` 时 rkfpv 可能还在初始化，插件要能等它（重试或者轮询）。

例子 `/etc/fpv/plugins.d/S50hello`：

```sh
#!/bin/sh
PID=/var/run/hello.pid
case "$1" in
start)  [ "$FPV_MODE" = fpv ] || exit 0
        start-stop-daemon -S -b -m -p $PID -x /usr/bin/hello ;;
stop)   start-stop-daemon -K -q -p $PID ;;
status) pidof hello > /dev/null && echo running || echo stopped ;;
esac
```

查看：

```
# fpv plugins
S50hello: running
# fpv status | grep plugins
plugins:   1/1 started
```

## 把自己的文件打进镜像：`EXTRA_OVERLAY`

扩展程序、它的插件脚本和配置放在一个目录里，按板子上的路径摆好，比如：

```
my-addon/
├── usr/bin/hello
└── etc/fpv/plugins.d/S50hello
```

出镜像时传给 `mkoverlay.sh`，多个目录用 `:` 隔开：

```sh
DRONE_KEY=... BOARD=mini EXTRA_OVERLAY=/path/my-addon luckfox/mkoverlay.sh
```

- 在 rkfpv 自己的文件装好之后再合并，所以也能替换 rkfpv 的文件（比如自带一份 `etc/rkfpv.conf`），替换的每个文件都会打印出来。
- 目录不存在时直接报错退出，不改 SDK 里的任何东西。
- 文件权限照原样保留，程序和脚本记得 `chmod 755`。
- 不出镜像、直接更新正在运行的板子：`EXTRA_OVERLAY=... luckfox/push-update.sh`，用 ADB 推过去，和 rkfpv 的程序一起替换。

## NPU：`KEEP_NPU=1`

为了省内存，镜像默认不加载 NPU 驱动 `rknpu.ko`（`mkoverlay.sh` 的 `TRIM_MODULES`）。要用 NPU 的扩展程序，出镜像时加 `KEEP_NPU=1`：

```sh
DRONE_KEY=... BOARD=pro KEEP_NPU=1 EXTRA_OVERLAY=/path/my-addon luckfox/mkoverlay.sh
```

- 只是开机时多加载 `rknpu.ko`，驱动文件本来就在 `/oem/usr/ko/`；RKNN 运行库 `librknnmrt.so` 也已经在 SDK 的 `/oem/usr/lib/` 里，扩展程序直接链接即可。
- 模型（`.rknn`）和推理程序用 `EXTRA_OVERLAY` 放进镜像。
- 内存：NPU 的输入输出缓冲和视频共用 CMA。Mini 只有 64 MB，FPV 模式下可用内存只剩几 MB，基本跑不了模型；Pro（128 MB）可以试，启用后用 `fpv status` 看 `memory`，确认图传帧率不变。
- 检查驱动是否加载：`ls /dev/rknpu* /sys/kernel/debug/rknpu 2>/dev/null` 或 `dmesg | grep -i rknpu`。

## MAVLink：和飞控对话

`mav_local_port` 打开板子上的本地 MAVLink 端口（只监听 127.0.0.1，外面连不进来），默认 0 关闭：

```sh
fpv set mav_local_port 14570     # 生效：fpv restart link
```

用法和 mavlink-router 的 UDP server 一样：

- 往 `127.0.0.1:14570` 发的 MAVLink 消息，`mavbridge` 原样写进飞控串口，和地面站的上行并在一起。两边的消息都是按数据报整块写，不会在中途交错。
- 谁发过数据，飞控的下行就也发给谁，回到它发送时用的地址和端口。10 秒没再发东西就不再发给它。MAVLink 程序本来就每秒发一次 HEARTBEAT，所以不用另外处理；只想收数据的程序发一个空数据报也算订阅。
- 地面站的链路不受影响：下行照常走 wfb_tx，rkfpv 的 OSD 照常收它那份。
- 用 pymavlink 测试：

  ```python
  from pymavlink import mavutil
  m = mavutil.mavlink_connection("udpout:127.0.0.1:14570", source_system=1, source_component=191)
  m.mav.heartbeat_send(18, 8, 0, 0, 0)          # 先发一个，才开始收
  print(m.recv_match(type="ATTITUDE", blocking=True, timeout=5))
  ```

- 发控制命令之前要想清楚飞控在什么模式、遥控器能不能随时接管。`mavbridge` 只是转发，不检查内容。

## OSD：在画面上画框和文字

`osd_ext_port` 让板子上的程序往 rkfpv 的 OSD 里画东西，比如识别到的目标框、AprilTag 编号和距离（只监听 127.0.0.1）。需要先打开 OSD：

```sh
fpv set osd 1 osd_ext_port 14580     # 生效：fpv restart video
```

协议是纯文本 UDP，**一个数据报就是当前要显示的全部图形**，替换上一个。每行一条命令：

| 命令 | 画什么 |
|---|---|
| `box X Y W H` | 空心框，左上角 X Y，宽 W 高 H |
| `text X Y 文字` | 文字，左上角 X Y，行尾之前都是文字（最多 47 字节，只显示 ASCII） |
| `cross X Y SIZE` | 十字，中心 X Y |
| `clear` | 清空（发一个空数据报也一样） |

- 坐标是 0–1000 的网格（横竖各 1000，SIZE 按高度算），rkfpv 按画布大小缩放，所以图传和录像分辨率不同也能对上。可以略微超出 0–1000，超出画面的部分会被裁掉。
- 一个数据报最多 64 个图形，写错的行跳过。
- `osd_ext_ttl_ms`（默认 500）内没收到新数据，图形就消失：发送程序崩了也不会在画面上留下旧框。要一直显示就定时重发。
- 收到新数据后最快 40 ms 内重画（平时每 250 ms 重画一次）。图形画在图传上；录像有自己的 OSD 时（`dvr_osd = 1`）也画在录像上。

试一下：

```sh
printf 'box 400 300 200 300\ntext 400 260 TAG 7  1.2m\n' | nc -u -w1 127.0.0.1 14580
```

Python：

```python
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b"box 400 300 200 300\ntext 400 260 TAG 7", ("127.0.0.1", 14580))
```

## 画面：帧接口（共享内存）

`frame_tap = 1` 时，rkfpv 把图传画面的灰度图（YUV 的 Y 平面）按设定的帧率复制到共享内存 `/dev/shm/rkfpv-frame`，给识别、AprilTag、光流之类的程序读：

```sh
fpv set frame_tap 1 frame_tap_fps 10 frame_tap_scale 2     # 生效：fpv restart video
ftdump -n 30 -o /tmp/f.pgm                                 # 看帧率和延迟，存一张图
```

| 设置 | 默认 | 说明 |
|---|---|---|
| `frame_tap` | 0 | 1 打开 |
| `frame_tap_fps` | 10 | 每秒复制几帧（1–30） |
| `frame_tap_scale` | 2 | 缩小倍数：1 原尺寸，2 一半（720p → 640×360），4 四分之一（320×180） |

- **读取**：只需要 [`src/frametap.h`](../src/frametap.h) 这一个头文件。`open` + `mmap` 只读映射，循环调用 `frametap_read()`，它只在有新的一帧时复制出来，返回帧号、相机时间戳和写入时间。完整的小例子见 [`src/ftdump.c`](../src/ftdump.c)。
- **不会读到写了一半的帧**：2 个缓冲槽轮流写，每个槽带序号（seqlock），`frametap_read()` 复制完核对序号，变了就重读。读的程序再慢也不会拖住 rkfpv。
- **缩小**：每隔 scale 个像素取一个（不做平均），CPU 开销小。需要更平滑的图可以自己再做模糊。
- **来源**：图传那一路画面，也就是地面看到的那一路（双路录像时是 ISP 的 self path）。分辨率和视场跟图传一样。
- **开销**：
  - 相机这一路多用一个缓冲区，720p 大约多占 1.4 MB 的 CMA（视频专用内存），这样编码器不用等帧接口；
  - 共享内存 2 帧，scale 2 约 460 KB，scale 4 约 115 KB；
  - 复制线程优先级低于图传。Mini 内存紧，建议 `frame_tap_scale = 4`，打开后用 `fpv status` 看 `memory`，并确认图传帧率不变。
- **状态**：`fpv log` 的 stats 行会多一项 `tap N fps`。rkfpv 重启时会重新创建这个文件，读的程序发现 2 秒没有新帧就重新 `open` + `mmap`（`ftdump` 的做法）。
- **时间戳**：`pts_us` 是相机驱动给的采集时间，`write_us` 是复制完成的时间（`CLOCK_MONOTONIC`，微秒），可以用来算延迟、和遥测对齐。
