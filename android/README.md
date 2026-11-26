# rkfpv 安卓 App

用一根 Type-C 线把天空端直接接到手机上，就能调参、实时预览画面、开始和停止录像、下载和播放 TF 卡上的录像。不需要电脑。

App 在手机上直接实现了 ADB 协议（通过安卓的 USB Host API），所以板子这边不用装任何新东西，手机也不需要 root。它执行的命令和 PC 配置器（`configurator/`）完全一样，都是 `fpv … --json`。所以 App 能做的事，`adb shell` 里也都能做。

## 功能

| 页面 | 内容 |
|---|---|
| 状态 | 模式、视频帧率和码率、网卡和发射功率、内存、温度、录像状态；每 3 秒刷新一次 |
| 预览 | **视频** 开关（`fpv preview on/off`），实时画面（H.265 硬解），分辨率 / 帧率 / 丢帧，录像控制 |
| 参数 | 按 `fpv schema` 自动生成，分组和 PC 配置器一样（Video / Camera / Radio / Picture …）。改过的项会高亮，点 **保存** 后作为一条 `fpv set` 发给板子。板子会先校验所有参数再写入。每一项都标明生效方式：即时生效、需重启、链路、下次启动。打开 **显示预览** 可以边调边看 |
| 录像 | **开始录像 / 停止**（当前文件、大小、丢帧）；板子 TF 卡上的录像列表（剩余空间、文件大小）；可以下载到手机、播放、分享、删除 |

还不支持：IQ 文件编辑、预设和备份、日志。这些 PC 配置器里都有。

## 预览和录像控制

调试模式下图传不运行，录像是 `rkfpv` 做的，所以要先打开 **视频** 开关。App 执行 `fpv preview on`，板子在调试模式下启动 `rkfpv`，但不开无线。

- **需要板子固件支持有线预览**（PR #18，目前在 `hotspot-osd-v2` 分支上，`main` 还没有）。老固件上打开视频会提示 `unknown command 'preview'`；调参和下载录像不受影响。
- **预览**：App 通过 ADB 直接打开板子上的 `tcp:5602`（和 `adb forward` 用的是同一个服务，手机上不需要转发端口）。码流格式见 `src/preview.c`：每帧是 4 字节大端长度加 Annex-B 的 H.265。App 用 `MediaCodec` 硬解，每帧解出来马上显示；解码跟不上时丢帧，直到下一个关键帧。改了"需重启"的参数后视频会重启，预览会自动重连。
- **录像**：开始和停止对应 `fpv dvr start/stop`，只有视频在运行时才能用。固件里 `dvr` 默认是关闭的（不占内存）：在 **参数 → DVR** 里把 `dvr` 打开，然后把视频关掉再打开。`dvr` 打开时，视频一启动就会自动开始录像。
- **供电**：视频运行时摄像头、编码器都在工作，录像时还要写 TF 卡，比纯调试模式耗电多。用手机供电时注意手机的 OTG 输出能力。

## 使用

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
| `fpv/Fpv.kt` | `fpv` 命令的客户端，解析 JSON；参数校验规则和 `files/fpv` 一致 |
| `video/H265.kt` | 预览码流的分帧（长度 + Annex-B），找关键帧和 VPS / SPS / PPS |
| `video/PreviewPlayer.kt` | 通过 ADB 读 `tcp:5602`，`MediaCodec` 解码到 SurfaceView，断开后自动重连 |
| `ui/` | Jetpack Compose 界面和 ViewModel |

测试：

- `AdbConnectionTest` 让 App 的协议代码和一个假的 adbd 通过本机 socket 通信。假 adbd 严格按流控收发，最大负载设得很小，让 sync 头跨包，并且用公钥验证 AUTH 签名。
- `AdbKeyTest` 用 `adb keygen` 生成的密钥对（`src/test/resources/adb/`，只用于测试），检查公钥编码和 PC 上的 adb 完全一致。
- `FpvClientTest` 用的 JSON 是在电脑上用 `FPV_ROOT` 运行 `files/fpv` 录下来的（preview 和 dvr status 来自 `hotspot-osd-v2` 的 `fpv`；`dvr_status_recording.json` 是按同样格式手写的，因为录像时的状态在电脑上跑不出来）。`validate_cases.txt` 里每一条都和 `fpv set` 的结果对过。
- `H265Test` 检查分帧在任意切包方式下都正确，以及关键帧和参数集的识别。`MediaCodec` 解码只能在手机上测。

## 已知限制和待验证

- **只有一个 USB 口**：接手机时就不能接 WiFi 网卡，所以有线模式只用在地面调参和回看。
- **还没在真实板子上测过**的几点：
  - 手机能否通过 C 对 C 线当主机并给板子供电，取决于板子 Type-C 口的 CC 电阻。不行的话换 OTG 转接头。
  - 板子 adbd 的版本和是否开了认证。两种情况代码都处理了：老版本 adbd 最大负载 4 KB、要求校验和；开了认证时会发送公钥。
  - 插上板子自动打开 App 靠 `res/xml/usb_device_filter.xml` 里的 Rockchip USB 厂商 ID 0x2207。不匹配的话也不影响使用，手动打开 App 就行。
