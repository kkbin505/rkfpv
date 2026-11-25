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
