# 天空端安装：网卡、wfb-ng、开机逻辑和供电

完整的从零编译步骤（主机环境、SDK、驱动、打包、烧 TF 卡）见 [build-steps.md](build-steps.md)。这里说明要改什么、文件装到哪里。

## 需要的改动

1. 内核打开 cfg80211；`rv1103g-luckfox-pico-mini.dts` 里把 `dr_mode` 改成 `"otg"`（开机后由 S99fpv 切 host），`&uart3` 改成 `status = "okay"`
2. 网卡驱动（`luckfox/build-deps.sh` 编译，装到 `/oem/usr/ko/`，wfb.sh 按 USB ID 自动选）：`88XXau.ko`（8812AU）、`88x2cu.ko`（8812CU/8822CU）、`8812eu.ko`（8812EU）
3. 交叉编译 wfb-ng（`wfb_tx`、`wfb_tx_cmd`）

## 文件装到哪里

`luckfox/mkoverlay.sh` 会把这些放进固件：

```
build/rkfpv      -> /usr/bin/rkfpv
build/mavbridge  -> /usr/bin/mavbridge
files/wfb.sh     -> /usr/bin/wfb.sh
files/wfb.yaml   -> /etc/wfb.yaml        （信道、link_id 要和地面端一致）
files/rkfpv.conf -> /etc/rkfpv.conf
files/S99fpv     -> /etc/init.d/S99fpv
drone.key        -> /etc/drone.key      （不进仓库，mkoverlay.sh 用 DRONE_KEY=... 指定）
```

## USB 角色与恢复

S99fpv 开机时判断一次：USB 插着电脑（gadget 被枚举）就保持 device 模式（ADB/RNDIS），
否则写 `host` 到 `/sys/class/usb_role/*/role`（dwc3 软件角色切换）启动 FPV。
**切成 host 后是强制的，之后再插电脑不会自动回到 ADB。** 恢复办法：

- 串口终端（UART2 调试口）：`echo device > /sys/class/usb_role/*/role`
- 或者 `touch /userdata/fpv.off` 后重启，插电脑开机
- 或者拔 TF 卡在电脑上删掉 `/etc/init.d/S99fpv`

## 供电

host 模式下 RTL8812AU 的 5V 取自板子 USB-C 的 VBUS，板上没有开关/限流。
8812AU 大功率发射电流较大，建议网卡由 BEC 单独供 5V（与板子共地），不要靠板子转供。
