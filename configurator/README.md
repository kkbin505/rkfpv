# rkfpv configurator

电脑上运行的配置界面，风格参考 Betaflight Configurator。界面上的每个操作都对应板子上的一条 `fpv … --json` 命令，所以界面能做的事，`adb shell` 里也都能做。

## 运行

需要 Python 3.8+（只用标准库），`adb` 要在 PATH 里。Linux 和 Windows 都能用。

```sh
python3 configurator/fpvconf.py            # 自动打开 http://127.0.0.1:8765/
python3 configurator/fpvconf.py --no-browser --port 9000
```

1. 不插 WiFi 网卡，板子 USB 直接接电脑，上电。约 10 秒后进入调试模式（开机找不到网卡就切回 USB 设备模式），ADB 出现。
2. 选设备，点 **Connect**。如果板子上还没有 `fpv`，界面会提示 **Install it now**。也可以在 Status 页点 **Install / update on board**，会把仓库里的 `files/fpv`、`files/S99fpv` 和 `files/wfb.sh` 推到板子上。
3. 在 Video / Camera / Radio / Telemetry 页修改参数，改过的项会标成橙色，然后点 **Save**。

URL 加上 `?connect&tab=Radio`，打开页面时会自动连接第一个设备并跳到 Radio 页。

## 页面

| 页面 | 内容 |
|---|---|
| Status | 模式（FPV / 调试）、码流 fps/码率、无线 MCS/FEC、温度、内存；开机模式切换；安装脚本 |
| Video / Camera / Radio / Telemetry | 根据 `fpv schema` 自动生成的表单 |
| OSD | 像 Betaflight 一样摆放 OSD：每个元素前面打勾才显示，在画面上拖动位置；有预览时背景就是实时画面 |
| Image | 画质设置（曝光、降噪、颜色等，FPV 运行时立即生效）；IQ 文件的下载、上传、切换出厂/修改版、历史版本回滚 |
| DVR | 录像状态、开始/停止按钮、卡上剩余空间；录像文件列表，可以下载（ADB）和删除；DVR 设置 |
| Presets & Backup | 把所有设置备份成 PC 上的 JSON 文件，或从文件恢复；套用预设（Factory defaults / Bench / Long range / Low latency） |
| Logs | rkfpv 日志（本次 / 上次开机）、开机日志、内核日志 |
| CLI | 直接在板子上执行命令，例如 `fpv help`，也可以是任意 shell 命令 |

参数后面的标签表示改完后怎么生效：

- **live**：FPV 正在运行时立即生效（编码器 SIGHUP，或 `wfb_tx_cmd`）
- **restart**：要重启视频才生效，保存后会出现 *Restart* 按钮
- **link**：信道、带宽、link_id、遥测。**地面站也要改成一样**，否则会断链

## 有线预览（Preview）

板子用 USB 接在电脑上（调试模式）时，点右上角 **▶ Preview**，页面右侧会出现实时画面，切换页面时一直显示，可以边调参边看。

- 画面就是图传那一路码流本身：同一个编码器、分辨率、码率、GOP，OSD 也是烧在码流里的。看到的效果和无线传出去的一样，只是没有无线丢包和 MCS 的影响。
- Image 页的画质参数、码率这类 **live** 设置改完马上就能看到；**restart** 类设置会重启视频，预览会自动重连。
- 点 Preview 时，板子执行 `fpv preview on`，在调试模式下启动 rkfpv（有飞控时也会启动 mavbridge，OSD 能显示飞控数据）。点 ✕ 或断开连接时执行 `fpv preview off`。这是临时状态，重启后恢复原样。
- 只走 ADB：rkfpv 只在板子的 `127.0.0.1:5602` 上提供预览（`rkfpv.conf` 里的 `preview_port`，0 = 关闭），由 `adb forward` 转到电脑上，无线那边看不到这个端口。
- 解码用浏览器的 WebCodecs（显卡硬件解码 H.265）。要用 **Chrome 或 Edge**，并且显卡支持 H.265 解码。不支持时面板里会提示，可以点 **Open in ffplay instead**，前提是电脑上装了 FFmpeg、`ffplay` 在 PATH 里。
- 只能有一个观看端：在 ffplay 里打开时，浏览器里的画面会停。

## OSD 布局

**OSD** 页左边是元素列表：飞行模式、ARMED、REC、计时器、CPU 温度、电池、mAh、分辨率/帧率、UL、LQ、准星。勾选才显示，不勾就不显示。右边是画面，直接拖动元素换位置，会吸附到字符网格上。

- **背景**：开着 Preview 时，右边就是实时画面（比侧边预览大），上面的 OSD 是板子自己画的，虚线框是拖动用的把手，拖完大约一秒画面上的 OSD 就跟过去。没开预览时背景是一张天空和地面的壁纸，元素显示示例数值。
- **保存**：每次勾选或拖完都立刻写进板子（`fpv set osd_layout ...`）。FPV 或预览在跑时马上生效，不用重启视频；没在跑时下次启动用。
- **对齐**：每个元素是一个框，宽度按它最长的文字算。框在画面左三分之一时文字左对齐，中间三分之一居中，右三分之一右对齐，所以放在右边的元素文字变长时也贴着右边。
- **分辨率**：位置按画面的千分比存，换分辨率（720p、1152x648、1080p 录像）不用重新摆。
- **Default layout** 按钮恢复默认摆法，所有元素都打开。
- 板子上 `osd = 0` 时页面顶部会提示，可以一键打开（要重启视频）。
- 存在 `rkfpv.conf` 的 `osd_layout` 里，格式是 `名字:显示:x:y,...`，x、y 是 0–1000。备份/恢复会一起带上。

## DVR 录像下载

下载录像需要 ADB（USB）。文件默认保存在 `~/Videos/rkfpv-dvr`，Windows 上是 `%USERPROFILE%\Videos\rkfpv-dvr`，可以用 `--dvr-dir` 改位置。**Open folder** 会打开这个文件夹。正在录的文件不能下载，也不能删除。

调试模式（板子接在电脑上）时 FPV 不运行，所以开始/停止按钮是灰的，但是可以下载和删除卡上已有的录像。

## 备份、恢复和预设

- **Save backup to PC**：把当前所有设置（`fpv get`）存成 `rkfpv-backup-日期-时间.json`。ADB 和 fpvd 两种连接都能用。
- **Restore from file…** 和预设都会先打开对比表：列出板子上的当前值 → 新值，每行一个勾选框。只有勾选的项会写入，而且是一条 `fpv set`，`fpv` 会先校验全部参数再写。
  - **link 类设置**（信道、带宽、link_id、遥测）默认不勾选。如果只改板子、不改地面站，会断链。
  - 板子上没有的参数（比如新版本备份里多出来的）、超出范围的值，都会列出来并跳过。
- **预设**就是 `configurator/presets/` 下的 JSON 文件：`{"name", "description", "values": {...}}`。可以直接加新文件；`make test` 会检查每个预设都能被 `fpv` 接受。预设里永远不放 link 类设置。

## 连接方式

| 方式 | 地址 | 能做什么 |
|---|---|---|
| ADB (USB) | 自动列出设备 | 全部功能，包括安装脚本和任意 shell 命令 |
| fpvd (UDP) | `172.32.0.93`（USB RNDIS），以后是 `10.5.0.10`（wfb 隧道） | 只能执行 `fpv` 命令；不能安装文件，也不能执行 shell 命令 |
| SSH | `10.5.0.10` | 预留，只支持密钥登录 |

走 USB RNDIS 时，要把电脑上新出现的那个网卡手动设成 172.32.0.x（例如 172.32.0.100/24）。

**Install / update on board** 会额外推送 `build/fpvd`，前提是这份代码里已经编译出了它（`make`）。

## 限制

调试模式下 USB 口接的是电脑，WiFi 网卡没接，无线链路不运行。这时改的设置会保存，下次以 FPV 模式开机时生效；画面相关的设置可以用上面的有线预览直接看效果。

要在飞行中实时调参，需要通过 wfb 隧道走 SSH（还没做）。界面已经预留了 SSH 选项，只支持密钥登录。

## 安全

服务只监听 127.0.0.1。每次启动生成一个随机 token，只嵌在服务自己返回的页面里，所有 API 请求都要带上它。这样别的网站不能借你的浏览器向板子发命令。
