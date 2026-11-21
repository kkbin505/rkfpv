# 桌面测试：不接网卡，直接推流到电脑

不接 WiFi 网卡，先确认相机、ISP 和编码都正常。用原厂 Luckfox 固件即可，不用改内核。电脑通过 USB（RNDIS，板子默认地址 172.32.0.93）连到板子。

```sh
# 电脑上
scp build/rkfpv root@172.32.0.93:/usr/bin/       # 或 adb push build/rkfpv /usr/bin/
gs/view.sh                                         # 先开播放端，监听 UDP 5600

# 板子上（adb shell / ssh）
killall rkipc                                      # rkipc 占着摄像头
rkfpv -s dst=172.32.0.100 -s port=5600             # dst 改成电脑在 RNDIS 网段的地址
```

每 5 秒会打印一行 `stats:`，内容是帧率、码率、最大帧和包速率。

**要检查的项：**
- 颜色正常。不正常就确认 `iq_dir` 下有你这颗 sensor 的 IQ 文件。
- 720p30 稳定，码率在设定值附近。
- 横移时看画面有没有错位。这时没有经过无线链路，如果仍然错位，问题就在相机或编码端。

## Linux 电脑当地面站（RTL8812AU）

电脑上的内核 6.12 用 OpenIPC 的 `rtl8812au_fpv` 驱动会卡死 nl80211，所以地面端改用 svpcom 的 `rtl8812au` v5.2.20（模块名 `88XXau_wfb`）。

```sh
gs/build.sh                          # 只需一次：编译驱动和电脑上的 wfb_rx，放到 gs/build/
sudo gs/gs_start.sh [信道]           # 加载驱动、切 monitor 模式（会确认并重试）、启动 wfb_rx
gs/stats.sh                          # 实时统计：包数、丢包、FEC 恢复、坏包、Mbps、每根天线的 RSSI/SNR
gs/view.sh                           # GStreamer 播放，用普通用户运行（不要 root）
sudo pkill wfb_rx; sudo rmmod 88XXau_wfb   # 停止
```

`gs/latency.html` 可以用来测端到端延迟。
