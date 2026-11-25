# 改设置：`fpv` 命令、fpvd 和配置工具

全部设置项见 [settings.zh.md](settings.zh.md)（英文版：[settings.en.md](settings.en.md)）。

## `fpv` 命令

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

加 `--json` 输出机器可读格式。新增参数只要在 `files/fpv` 的参数表里加一行，再在两份设置文档里各加一行（`make test` 会检查）。

## fpvd（UDP 命令服务）

板子开机就在 UDP 7000 端口运行 `fpvd`（`src/fpvd.c`），只执行 `fpv` 的子命令（get/set/schema/status/log/restart/mode/reset），不给 shell，参数只允许安全字符。现在可以走 USB RNDIS（板子 172.32.0.93，电脑网卡手动设成 172.32.0.x），以后走 wfb 隧道（见 issue #1），不用 UART 也不占 USB 口。

协议很简单，按无线丢包设计：请求 `FPV1 <id> <命令> [参数…]`；回复按 1200 字节分片，每片格式 `FPV1 <id> <序号>/<总数>\n<JSON 片段>`。丢包时客户端用**同一个 id** 重发，fpvd 直接返回缓存的结果，不会重复执行（比如 restart 不会跑两次）。

## 电脑上的配置工具

界面风格类似 Betaflight Configurator。板子接电脑开机进入调试模式后，运行

```sh
python3 configurator/fpvconf.py
```

浏览器里会打开界面，详见 [configurator/README.md](../configurator/README.md)。

## 直接改文件

```sh
vi /etc/rkfpv.conf
killall -HUP rkfpv
```

bitrate、max_bitrate、gop、rc、stat_time 不用重启就能生效（同时会请求一个 IDR）。size、fps、dst、pkt_size 改了要重启。

命令行用 `-s key=value` 覆盖配置文件，可以重复多次。
