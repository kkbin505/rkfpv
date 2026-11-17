# rkfpv configurator

电脑上运行的配置界面，风格参考 Betaflight Configurator。界面上的每个操作都对应板子上的一条 `fpv … --json` 命令，所以界面能做的事，`adb shell` 里也都能做。

## 运行

需要 Python 3.8+（只用标准库），`adb` 要在 PATH 里。Linux 和 Windows 都能用。

```sh
python3 configurator/fpvconf.py            # 自动打开 http://127.0.0.1:8765/
python3 configurator/fpvconf.py --no-browser --port 9000
```

1. 不插 WiFi 网卡，板子 USB 直接接电脑，上电。约 15 秒后进入调试模式，ADB 出现。
2. 选设备，点 **Connect**。如果板子上还没有 `fpv`，界面会提示 **Install it now**。也可以在 Status 页点 **Install / update on board**，会把仓库里的 `files/fpv`、`files/S99fpv` 和 `files/wfb.sh` 推到板子上。
3. 在 Video / Camera / Radio / Telemetry 页修改参数，改过的项会标黄，然后点 **Save**。

URL 加上 `?connect&tab=Radio`，打开页面时会自动连接第一个设备并跳到 Radio 页。

## 页面

| 页面 | 内容 |
|---|---|
| Status | 模式（FPV / 调试）、码流 fps/码率、无线 MCS/FEC、温度、内存；开机模式切换；安装脚本 |
| Video / Camera / Radio / Telemetry | 根据 `fpv schema` 自动生成的表单 |
| Logs | rkfpv 日志（本次 / 上次开机）、开机日志、内核日志 |
| CLI | 直接在板子上执行命令，例如 `fpv help`，也可以是任意 shell 命令 |

参数后面的标签表示改完后怎么生效：

- **live**：FPV 正在运行时立即生效（编码器 SIGHUP，或 `wfb_tx_cmd`）
- **restart**：要重启视频才生效，保存后会出现 *Restart* 按钮
- **link**：信道、带宽、link_id、遥测。**地面站也要改成一样**，否则会断链

## 限制

调试模式下 USB 口接的是电脑，WiFi 网卡没接，FPV 不运行。这时改的设置会保存，下次以 FPV 模式开机时生效。

要在飞行中实时调参，需要通过 wfb 隧道走 SSH（还没做）。界面已经预留了 SSH 选项，只支持密钥登录。

## 安全

服务只监听 127.0.0.1。每次启动生成一个随机 token，只嵌在服务自己返回的页面里，所有 API 请求都要带上它。这样别的网站不能借你的浏览器向板子发命令。
