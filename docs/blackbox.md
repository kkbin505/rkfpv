# Betaflight 黑盒 → Gyroflow

飞控把 Betaflight blackbox 从一个串口发出来，天空端的 `bblog` 原样存成 `.bbl` 文件，放在 TF 卡上的录像旁边，文件名和录像配对。Gyroflow 用这份陀螺仪数据给 DVR 录像做后期增稳。

```
飞控 UART TX ──► 板子 UART4 RX ──► bblog ──► /mnt/dvr/rec_0012_03.bbl
                                              /mnt/dvr/rec_0012_03.mp4（同一段录像）
```

## 文件怎么命名
- **一次解锁一个日志。** 日志从 BF 的第一行头 `H Product:Blackbox flight data recorder` 开始，到日志结束标记 `End of log` 结束。下一个日志头出现，或者超过 2 秒没有数据（断电、拔线），也算结束。日志头之前的杂字节丢掉。
- **和录像配对。** 日志开始时正在录哪一段（rkfpv 的 `/tmp/dvr.status`），就叫什么名字。比如正在录 `rec_0012_03.mp4`，日志就存为 `rec_0012_03.bbl`。
- **同一段里解锁两次，追加到同一个文件。** 一个 `.bbl` 文件本来就可以包含多个日志，Blackbox Explorer 和 Gyroflow 都能读。
- **没在录像时（没插卡、DVR 已停止），日志不保存。** `fpv blackbox status` 会显示 `not-saved`。
- 每 2 秒和每个日志结束时 `fsync` 一次，突然断电最多丢约 2 秒。
- **卡满时**，`.bbl` 和录像一起按时间从旧到新删除，正在录的那一段的 `.bbl` 不会删。
- **下载**：`.bbl` 和录像一样，可以在配置工具 DVR 页、热点页面下载，也能用 `fpv dvr list/delete` 管理。

## 接线
只接一根信号线加地线：飞控空闲 UART 的 TX 接板子的 UART4 RX。

| | UART4 TX（GPIO1_C4） | UART4 RX（GPIO1_C5） | GND |
|---|---|---|---|
| Pico Mini | 排针 10 | 排针 **11** | 2 / 21 |
| Pico Pro | 排针 7 | 排针 **6** | 3 / 8 |

UART4 需要设备树里 `&uart4 { status = "okay"; }`（见 build-steps.md 2.1 节和 Pro 一节）。这一项改的是内核设备树，必须重新出镜像、刷机，用 `push-update.sh` 推不进去。

## 第一步：回环自测（不接飞控）
在 UART4 的 TX 和 RX 之间接一根杜邦线：Mini 接排针 10–11，Pro 接排针 6–7。两个脚相邻，一根跳线帽就行。然后：

```sh
fpv blackbox selftest              # 依次测 1000000 / 1500000 / 2000000
fpv blackbox selftest 1500000      # 只测一个波特率
fpv blackbox selftest -r 0         # 不限速，按串口最高速度灌数据
```

- 板子上的 `bbsend` 扮演飞控：在 TX 上发两段 BF 格式的日志（日志头、数据帧、`End of log`），每段前面带一些杂字节。
- `bblog` 从 RX 收下来，然后和发出的内容逐字节比较。
- 默认按 60 KB/s 发，相当于 BF 2 kHz 记录时的数据量。
- 每个波特率都显示 `pass` 就说明没问题；如果高波特率失败，`blackbox_baud` 就用通过的最高那一档。
- 测试时如果 bblog 正在运行，会先暂停，测完再恢复；测试文件放在 `/tmp`，不写卡。

测完记得拔掉跳线。

## 不接飞控、不接线：HIL 测试

`test/bb_hil.sh` 在板子上模拟飞控，检查从“飞控发出”到“卡上的 .bbl 能正确解析”这一整条链路：

```
bbsim（模拟 BF 飞控）──► 伪终端（代替 UART4）──► bblog ──► /mnt/dvr/bbhil/rec_XXXX_YY.bbl
                                                         （名字跟正在录的那段录像配对）
电脑：取回 .bbl ──► 和发出的字节逐字节比较 ──► test/bbdecode.py 解码，每一帧的每个值和模拟器发的比较
```

- `bbsim` 发的是真正能解码的 Betaflight 日志：BF 的日志头、按 BF 编码方式（预测器 + 变长编码）编码的 I 帧和 P 帧、日志结束事件。飞行数据是已知的函数（陀螺仪、电机的正弦波，加一点循环时间抖动），所以解码结果可以逐值核对。
- 三种情况：两次解锁各 20 秒（2 kHz，日志前带杂字节）；一次 15 秒 8 kHz（约为正常飞控数据量的 4 倍）；一次 5 秒没有结束标记（模拟拔线 / 断电）。
- 板子要在录像：`fpv set dvr 1`，再开视频（电脑预览或 FPV 模式）。不需要 UART4，也不需要重新出镜像：测试用的 bblog 从 `/tmp` 运行，不动板子自己的 bblog。
- 测试文件放在卡上单独的 `/mnt/dvr/bbhil/` 目录，不会追加到真实飞行的 `.bbl` 里。

```sh
make build/bblog build/bbsim
test/bb_hil.sh
```

不带板子的同一条链路在 `make test` 里（`test/test_bbsim.sh`）。

## 第二步：接真实飞控
1. **Betaflight 设置：**
   - Ports 页：把一个空闲 UART 设成 **Blackbox logging**，波特率和 `blackbox_baud` 相同；
   - Blackbox 页：Logging device 选 **Serial port**，也就是 `blackbox_device = SERIAL`；
   - 采样率设成 1–2 kHz，比如 8k 陀螺仪用 `blackbox_sample_rate = 1/4`；
   - BF 4.3 以后默认会记未滤波的陀螺仪数据（gyroUnfilt），Gyroflow 用的就是它。
2. **rkfpv 设置：**
   ```sh
   fpv set blackbox 1 blackbox_baud 1500000
   fpv set dvr 1                       # 要在录像才会保存
   fpv set dvr_osd 0                   # 录像不烧 OSD（Gyroflow 需要干净画面）
   fpv restart video
   ```
   `dvr_osd = 0` 只有录像用单独的编码器时（`dvr_size` 不是 `link`）才有效；录的如果是图传那一路，画面里一定有 OSD。
3. **检查：** 解锁后 `fpv blackbox status` 显示 `logging`；上锁后卡上出现和录像同名的 `.bbl`，可以先在 Blackbox Explorer 里打开看看。
4. **Gyroflow：** 把 `.bbl` 和同名的 mp4 一起拖进去，用光流自动对齐时间。Gyroflow 还需要这颗镜头的标定文件，可以用 DVR 录一段棋盘格，在 Gyroflow 自带的标定工具里生成。

## 命令
```sh
fpv blackbox status        # 状态、当前文件、日志数、丢弃字节、错误
cat /userdata/log/bblog.log # bblog 自己的日志：每个日志的开始、结束、文件名
```
