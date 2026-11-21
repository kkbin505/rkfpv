# MAVLink 遥测（mavbridge）

## 接线

飞控串口接 **UART3（/dev/ttyS3）**，用的是 UART3_M1 这组引脚：

| 板子引脚 | 功能 | 接飞控 |
|---|---|---|
| GPIO1_D0 | UART3_TX | 飞控 RX |
| GPIO1_D1 | UART3_RX | 飞控 TX |
| GND | 地 | GND |

两边都是 3.3V 电平，飞控那边的串口要设成 MAVLink、115200。

## 不接网卡时测试

原厂固件里 UART3 默认是关的，先用 `luckfox-config` 把 UART3 打开（重启生效）。电脑上开 QGroundControl 监听 UDP 14550，然后在板子上运行：

```sh
mavbridge -d /dev/ttyS3 -b 115200 -o <电脑IP>:14550 -s 5
```

QGC 能连上飞控、看到姿态，就说明串口和转发都通了。`-s 5` 会每 5 秒打印一次统计；junk 字节数持续增长，说明波特率或协议没对上。

## 经过 wfb-ng

接上网卡以后由 `wfb.sh` 自动启动，无线端口和 OpenIPC 一致，地面端不用改：

```
UART3 → mavbridge → udp:14551 → wfb_tx -p 16  ──无线──▶ 地面
UART3 ← mavbridge ← udp:14550 ← wfb_rx -p 144 ◀──无线── 地面
```

`mavbridge` 按 MAVLink 帧边界切分，把最多 15 帧、不超过 1000 字节打成一个 UDP 包，最多攒 30 ms 就发。这样丢一个无线包只会丢掉几条完整消息，不会出现半帧数据。遥测的 FEC 用 1/2（每包发两份），不用等攒满一组。
