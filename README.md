# rkfpv

一个基于 Luckfox Pico Mini（RV1103）的无线高清图传天空端项目。

## 优点

- **高性价比**：一百多元一套。
- **体积小**：主板只有 28 × 21 mm。
- **免焊接，即刷即用**：刷好固件就能用。
- **开源**：全部代码和所有参数都公开。
- **可视化调参**：有独立的调参软件，支持 USB 调参；WiFi 调参（热点模式）正在开发。
- **稳定**：在 Luckfox 官方 SDK 基础上修改，改动小。
- **兼容性好**：兼容使用广泛的 OpenIPC 地面端（PixelPilot、Radxa 等），也有自己的 Linux 地面端。

## 缺点

- **延迟高**：端到端延迟大于 60 ms。
- **画质一般**：廉价摄像头和镜头的画质有限，软件没法弥补。

## 组成

```
摄像头 → RV1103（ISP + H.265 编码）→ wfb-ng → RTL8812 网卡 ──无线──▶ 地面端
飞控串口 ⇄ mavbridge ⇄ wfb-ng（MAVLink 遥测）
```

- 天空端：Luckfox Pico Mini、SC3336 摄像头、RTL8812AU / 8812CU / 8812EU 网卡。
- 地面端：OpenIPC 地面站（PixelPilot 安卓、Radxa Zero3 等），或者 Linux 电脑加 RTL8812AU 网卡。

## 快速开始

1. 编译固件并烧到 TF 卡：[docs/build-steps.md](docs/build-steps.md)
2. 先不接网卡，在桌面上确认画面：[docs/bench-test.md](docs/bench-test.md)
3. 接网卡和飞控：[docs/install.md](docs/install.md)、[docs/telemetry.md](docs/telemetry.md)
4. 调参：电脑上运行 `python3 configurator/fpvconf.py`，或者在板子上用 `fpv` 命令：[docs/fpv-command.md](docs/fpv-command.md)

## 文档

| 文档 | 内容 |
|---|---|
| [build-steps.md](docs/build-steps.md) | 从零开始：主机环境、SDK、驱动、打包、烧 TF 卡（英文） |
| [bench-test.md](docs/bench-test.md) | 不接网卡推流到电脑；Linux 电脑当地面站 |
| [install.md](docs/install.md) | 天空端要改什么、文件装到哪、USB 角色与恢复、供电 |
| [telemetry.md](docs/telemetry.md) | 飞控接线和 MAVLink 遥测 |
| [fpv-command.md](docs/fpv-command.md) | `fpv` 命令、fpvd 命令服务、配置工具、直接改文件 |
| [settings.zh.md](docs/settings.zh.md) | 全部设置项一览（英文版：[settings.en.md](docs/settings.en.md)） |
| [openipc-params.md](docs/openipc-params.md) | OpenIPC 参数和 rkfpv 的对应关系（英文） |
| [技术路线.md](docs/技术路线.md) | 为什么这样设计 |
| [development.md](docs/development.md) | 编译、测试、代码结构 |
| [configurator/README.md](configurator/README.md) | 调参软件 |

## 致谢

这个项目站在很多开源项目的肩膀上，感谢：

- **[OpenIPC](https://github.com/OpenIPC)**：让 IP 摄像头芯片能做数字图传的思路，以及 `wifibroadcast` 脚本、网卡驱动的整合与补丁、TX 功率刻度、各种参数经验。`files/wfb.sh` 就是从 OpenIPC 的脚本精简来的。
- **[wfb-ng](https://github.com/svpcom/wfb-ng)**（svpcom）：整个无线链路，包括广播、FEC、加密和遥测通道。
- **[Luckfox](https://github.com/LuckfoxTECH/luckfox-pico)** 和 **Rockchip**：Luckfox Pico SDK、RKMPI / rkaiq 媒体库和示例代码。
- **Realtek 网卡驱动的维护者**：[openipc/realtek-wlan](https://github.com/openipc/realtek-wlan)（RTL8812AU FPV 分支）、[libc0607](https://github.com/libc0607) 的 RTL8812CU/8822CU 和 RTL8812EU 驱动、[svpcom/rtl8812au](https://github.com/svpcom/rtl8812au)。
- **PixelPilot** 等 OpenIPC 地面站项目：本项目的视频格式和它们保持兼容。
- 界面风格参考了 **[Betaflight Configurator](https://github.com/betaflight/betaflight-configurator)**。

本项目与 Rockchip、Luckfox 没有关联。

## 许可证

本仓库自己的代码使用 [MIT](LICENSE) 许可证。从第三方项目改编的文件、第三方补丁、构建时下载的组件，以及各自的许可证，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

如果要分发编译好的固件镜像，注意镜像里包含 GPL 软件（内核、网卡驱动、wfb-ng），必须同时提供对应的源码，详见 THIRD_PARTY_NOTICES.md。
