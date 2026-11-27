# rkfpv 安卓 App

用一根 Type-C 线把天空端直接接到手机上，或者让手机连上天空端的 WiFi 热点，就能调参、实时预览画面、下载和播放 TF 卡上的录像。有线时还能开始和停止录像。不需要电脑。

两种连接方式：

- **USB（ADB）**：App 在手机上直接实现了 ADB 协议（通过安卓的 USB Host API），所以板子这边不用装任何新东西，手机也不需要 root。
- **WiFi（天空端热点）**：按住板子的 BOOT 键 2 秒，WiFi 网卡变成热点，App 通过 HTTP 访问板子上的 `dvrhttpd`（`http://10.0.0.1`）。用的是网页配置器在热点模式下用的同一套接口。不占 USB 口，网卡可以一直插着。见下面的 [WiFi](#wifi天空端热点)。

两种方式执行的命令和 PC 配置器（`configurator/`）完全一样，都是 `fpv … --json`。所以 App 能做的事，`adb shell` 里也都能做。

## 功能

| 页面 | 内容 |
|---|---|
| 状态 | 模式、视频帧率和码率、网卡和发射功率、内存、温度、录像状态；每 3 秒刷新一次 |
| 预览 | **视频** 开关（`fpv preview on/off`），实时画面（H.265 硬解），分辨率 / 帧率 / 丢帧，录像控制 |
| 参数 | 按 `fpv schema` 自动生成，分组和 PC 配置器一样（Video / Camera / Radio / Picture …）。改过的项会高亮，点 **保存** 后作为一条 `fpv set` 发给板子。板子会先校验所有参数再写入。每一项都标明生效方式：即时生效、需重启、链路、下次启动。打开 **显示预览** 可以边调边看 |
| 录像 | **开始录像 / 停止**（当前文件、大小、丢帧）；板子 TF 卡上的录像列表（剩余空间、文件大小）；可以下载到手机、播放、分享、删除 |

还不支持：IQ 文件编辑、预设和备份、日志。这些 PC 配置器里都有。

## WiFi（天空端热点）

**两代板子固件都能用**：

- 带热点网页接口的固件（PR #25，`web-configurator-hotspot` 分支）：调参走 `dvrhttpd` 的 `POST /api/fpv`，预览走 `GET /live`。
- 更早的热点固件：`dvrhttpd` 只管录像，`/api/fpv` 返回 404。App 自动改用板子上的 `fpvd`（UDP 7000，和 PC 配置器的 fpvd 连接方式同一个协议）来调参，录像照样走 HTTP。**这种固件上没有预览**，状态页地址后面会注明。

1. 天空端插着 WiFi 网卡，正常上电进入 FPV。按住 **BOOT 键 2 秒**，网卡变成热点 `rkfpv-XXXX`（信道、国家码见 **参数 → Hotspot**）。
2. 在安卓的 WiFi 设置里连上这个热点。系统提示"此网络无法访问互联网"时，选**保持连接**。手机可能会弹出录像页面（captive portal），关掉就行。
3. 打开 App，在 **状态** 页的"通过天空端 WiFi 热点"里点 **连接**。地址默认 `10.0.0.1`。
4. 之后再打开 App，如果上次是用 WiFi 连的，并且手机还在 WiFi 上，App 会自动重连。点 **断开** 后就不再自动连接。

**第一次的密码**：`hotspot_pass` 是板子第一次开机时随机生成的。先用 USB 连一次，在 **参数 → Hotspot** 里查看或修改；也可以在 `adb shell` 里运行 `fpv hotspot status`。

**和 USB 的区别**：

- 热点模式下网卡在当热点，所以**图传不运行，也不录像**（`S99fpv` 用 `dvr=0` 启动 rkfpv）。录像按钮是灰的；已有的录像可以下载、播放、删除。
- 预览（只有带热点网页接口的固件）：打开 **视频** 开关，App 执行 `fpv preview on`，画面从 `/live` 读取，延迟取决于 WiFi。
- 保存的参数在下次 FPV 启动时生效（再按住 BOOT 2 秒回到 FPV）。改 `hotspot_*` 不会断开当前连接，下次开热点时才用新值。
- `dvrhttpd` 只接受由字母、数字和 `_ . , : -` 组成、不超过 64 个字符的参数值（`src/fpvcmd.c`，那里没有 shell）。带其他字符的值，比如含 `/` 的路径 `iq_dir`，或含特殊字符的密码，App 会直接提示改用 USB，不会发给板子。
- 下载走 HTTP，不支持断点续传；断了就重新下载。
- 手机的移动数据开着也没关系：热点没有互联网，安卓会继续把移动数据当默认网络，但 App 的请求显式走 WiFi 那个网络（`Network.openConnection`），不需要关掉移动数据。

## 预览和录像控制

调试模式下图传不运行，录像是 `rkfpv` 做的，所以要先打开 **视频** 开关。App 执行 `fpv preview on`，板子在调试模式下启动 `rkfpv`，但不开无线。

- **需要板子固件支持有线预览**（PR #18，目前在 `hotspot-osd-v2` 分支上，`main` 还没有）。老固件上打开视频会提示 `unknown command 'preview'`；调参和下载录像不受影响。
- **预览**：App 通过 ADB 直接打开板子上的 `tcp:5602`（和 `adb forward` 用的是同一个服务，手机上不需要转发端口）。码流格式见 `src/preview.c`：每帧是 4 字节大端长度加 Annex-B 的 H.265。App 用 `MediaCodec` 硬解，每帧解出来马上显示；解码跟不上时丢帧，直到下一个关键帧。改了"需重启"的参数后视频会重启，预览会自动重连。
- **录像**：开始和停止对应 `fpv dvr start/stop`，只有视频在运行时才能用。固件里 `dvr` 默认是关闭的（不占内存）：在 **参数 → DVR** 里把 `dvr` 打开，然后把视频关掉再打开。`dvr` 打开时，视频一启动就会自动开始录像。
- **供电**：视频运行时摄像头、编码器都在工作，录像时还要写 TF 卡，比纯调试模式耗电多。用手机供电时注意手机的 OTG 输出能力。

## 使用（USB）

1. 天空端**不要插 WiFi 网卡**，用 Type-C 对 Type-C 线（或 OTG 转接头）接到手机上。手机会给板子供电。
2. 板子上电约 10 秒后，发现 USB 上有主机，进入调试模式（ADB）。这一步手机和电脑一样。
3. 第一次连接时，安卓会询问是否允许 rkfpv 访问 USB 设备，选允许。勾选"默认打开"之后，插上板子会直接打开 App。
4. 在 **参数** 页修改后点 **保存**。

调试模式下图传不运行，所以保存的参数会在**下次 FPV 启动**时生效：拔掉手机，插上网卡，重新上电即可。

标为 **链路** 的参数（信道、带宽、link_id、遥测）必须在地面站上改成同样的值，否则会断链。保存前 App 会再确认一次。

## 录像

- 下载的文件保存在 App 自己的目录：`Android/data/io.github.kkbin505.rkfpv/files/Movies/rkfpv-dvr/`。卸载 App 时会一起删除。想长期保存的话，可以用 **分享** 存到相册或网盘。
- 播放使用手机自带的播放器。录像默认是 H.265 的 fMP4，近几年的手机基本都能硬解。
- 正在录制的文件不能下载，也不能删除（调试模式下不会录像，所以接手机时不会遇到）。
- 下载速度取决于板子的 USB 2.0 和 TF 卡，应该和电脑上 `adb pull` 差不多。

## 编译

需要 JDK 17 或更新的版本，以及 Android SDK（platform 34）。Android Studio 可以直接打开 `android/` 目录。命令行编译：

```sh
cd android
./gradlew assembleDebug          # → app/build/outputs/apk/debug/app-debug.apk
./gradlew testDebugUnitTest      # JVM 单元测试，不需要手机
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

如果 SDK 不在默认位置，设置 `ANDROID_HOME`，或者在 `android/local.properties` 里写 `sdk.dir=…`。

本仓库不带发布用的签名密钥，release 构建也用 debug 密钥签名。要正式分发，请用自己的密钥签名。

## 代码结构

| 文件 | 作用 |
|---|---|
| `adb/AdbMessage.kt` | ADB 消息头（24 字节，小端） |
| `adb/AdbConnection.kt` | CNXN / AUTH 握手，多路复用的流（OPEN / OKAY / WRTE / CLSE），一个读线程 |
| `adb/AdbSync.kt` | `sync:` 服务：STAT、RECV（也就是 `adb pull`） |
| `adb/AdbKey.kt` | App 自己的 RSA 密钥、AUTH 签名、adbd 用的公钥格式 |
| `adb/UsbAdbChannel.kt` | USB bulk 端点（接口 class 0xff / subclass 0x42 / protocol 1） |
| `board/Board.kt` | 连接方式的接口：`fpv` 命令、预览码流、下载录像三件事 |
| `board/AdbBoard.kt` | USB：`shell:` 执行 fpv，`tcp:5602` 读预览，`sync:` 下载 |
| `board/HttpBoard.kt` | WiFi：`dvrhttpd` 的 `/api/fpv`、`/live`、`/dvr/<名字>`；请求绑定到热点的 `Network` |
| `board/FpvdClient.kt` | WiFi、老固件：`fpvd` 的 UDP 协议（同一个 id 重发，按 1200 字节分片收回复） |
| `fpv/Fpv.kt` | `fpv` 命令的客户端，解析 JSON；参数校验规则和 `files/fpv` 一致 |
| `video/H265.kt` | 预览码流的分帧（长度 + Annex-B），找关键帧和 VPS / SPS / PPS |
| `video/PreviewPlayer.kt` | 通过 ADB 读 `tcp:5602`，`MediaCodec` 解码到 SurfaceView，断开后自动重连 |
| `ui/` | Jetpack Compose 界面和 ViewModel |

测试：

- `AdbConnectionTest` 让 App 的协议代码和一个假的 adbd 通过本机 socket 通信。假 adbd 严格按流控收发，最大负载设得很小，让 sync 头跨包，并且用公钥验证 AUTH 签名。
- `AdbKeyTest` 用 `adb keygen` 生成的密钥对（`src/test/resources/adb/`，只用于测试），检查公钥编码和 PC 上的 adb 完全一致。
- `FpvClientTest` 用的 JSON 是在电脑上用 `FPV_ROOT` 运行 `files/fpv` 录下来的（preview 和 dvr status 来自 `hotspot-osd-v2` 的 `fpv`；`dvr_status_recording.json` 是按同样格式手写的，因为录像时的状态在电脑上跑不出来）。`validate_cases.txt` 里每一条都和 `fpv set` 的结果对过。
- `H265Test` 检查分帧在任意切包方式下都正确，以及关键帧和参数集的识别。`MediaCodec` 解码只能在手机上测。
- `HttpBoardTest` 让 `HttpBoard` 和一个假的 `dvrhttpd`（本机 socket）通信：确认头、参数不加引号、JSON 错误、503 表示预览没开、预览码流任意切包、下载、不合法的参数值不发送；还有老固件：`/api/fpv` 404 时改走假的 `fpvd`（第一个包故意丢掉、两个分片倒序到达）。
- `HttpBoardLiveTest` 连真的 `dvrhttpd`，默认跳过。在电脑上用 PR #25 的 `src/dvrhttpd.c` 和 `files/fpv`（`FPV_ROOT` 指向一个临时目录，`tmp/mode.status` 写 `hotspot`）就能跑，接真板子也可以：
  ```sh
  RKFPV_TEST_BOARD=127.0.0.1:28080 ./gradlew testDebugUnitTest --tests '*HttpBoardLiveTest*'
  ```
  会改一次 `bitrate` 再改回去；需要板子上有 `rec_0003_01.mp4`，预览端口有画面。

## 已知限制和待验证

- **只有一个 USB 口**：接手机时就不能接 WiFi 网卡，所以有线模式只用在地面调参和回看。不想拔网卡时用 WiFi 热点。
- **WiFi 在三星 S22 和一块老热点固件的板子上测过**：连接、状态、参数、录像列表、下载都正常（设置走 fpvd）。还没测的：
  - 带热点网页接口的新固件上的 `/live` 预览，以及它在 WiFi 上的延迟和丢帧（只在电脑上对真的 `dvrhttpd` 测过转发）。
  - 移动数据开着时请求是否仍走 WiFi（测试时手机只有 WiFi 一个网络）。
- **还没在真实板子上测过**的几点：
  - 手机能否通过 C 对 C 线当主机并给板子供电，取决于板子 Type-C 口的 CC 电阻。不行的话换 OTG 转接头。
  - 板子 adbd 的版本和是否开了认证。两种情况代码都处理了：老版本 adbd 最大负载 4 KB、要求校验和；开了认证时会发送公钥。
  - 插上板子自动打开 App 靠 `res/xml/usb_device_filter.xml` 里的 Rockchip USB 厂商 ID 0x2207。不匹配的话也不影响使用，手动打开 App 就行。
