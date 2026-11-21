# 开发：编译、测试和代码结构

## 编译

```sh
make            # 交叉编译，产物 build/rkfpv（默认 SDK=../luckfox-pico，可用 make SDK=... 指定）
make test       # 在电脑上跑单元测试
```

依赖 Luckfox SDK 里预编译好的 rockit / rkaiq / mpp / rga 和 uclibc 工具链，不需要先完整编一遍 SDK。打包整个固件见 [build-steps.md](build-steps.md)。

## 视频链路

```
sensor → AIQ(ISP) → VI → VENC(H.265) → RTP/UDP → wfb_tx -u 5600 → 无线
```

地面端照常接收 UDP 5600 上的 H.265 RTP，和 OpenIPC 地面站一样。设计上的考虑见 [技术路线.md](技术路线.md)。

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
| `files/wfb.sh` | 加载网卡驱动、启动 wfb-ng 和 mavbridge |
| `files/S99fpv` | 开机判断调试模式还是 FPV 模式 |
| `configurator/` | 电脑上的配置界面 |
| `luckfox/` | 编译网卡驱动和 wfb-ng、拼固件 overlay、SDK 补丁 |
| `gs/` | Linux 电脑当地面站的脚本 |
| `test/` | 主机端单元测试；`send_file` 用来和 GStreamer 做端到端测试 |
