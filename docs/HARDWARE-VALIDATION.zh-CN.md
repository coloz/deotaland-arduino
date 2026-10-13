# AIOT-Basic 自动实测记录

实测日期：2026-10-12；库版本：**0.8.2 → 0.9.0 → 0.9.1 → 0.9.2 → 0.9.3**；Arduino-ESP32：**3.3.12 / ESP-IDF 5.5.5**。测试对象为用户连接在 **COM7** 的 ESP32-S3 AIOT-Basic，使用真实驱动和完整 [AiotBasicAgent](../examples/AiotBasicAgent/README.md) 示例。下文保留各轮历史记录；其中 WiFi、云平台和 MQTT 仅记录移除前的版本，不代表当前源码支持。当前库仅保留 BLE 内置连接，相关 WiFi 代码、配置和示例已移除。电脑覆盖的测试与现场观察、手机 App 验收分开记录。

## 2026-10-13：移除 Wi-Fi 后的回归

本轮移除 Wi-Fi 联网、配网、云端激活、MQTT 桥接及四个相关示例，整板示例固定使用 BLE。以下检查针对修改后的生产源码，未烧录开发板或进行新的 App 端到端验收；后续章节保留移除前的实测记录。

| 检查 | 结果 |
| --- | --- |
| Arduino-ESP32 3.3.12 / S3 | 全部 19 个示例编译通过，零警告；16M Flash / OPI PSRAM / min_spiffs / USB CDC |
| C3 / H2 | BasicBLE 分别编译通过，零警告；C3 无 PSRAM，H2 无 Wi-Fi 硬件 |
| BLE 接收与播放顺序 | 实际核心、队列、音频和 BLE 接收入口的 260 项主机断言通过 |
| 核心存储与生命周期 | ESP32 分配边界替身、实际核心 C++11 的 57 项主机断言通过 |
| 公共 C API | C11、Wall / Wextra / Werror 编译通过 |
| 源码与固件检查 | 业务源码和示例无 Wi-Fi/MQTT 功能引用；C3 固件无 Wi-Fi、HTTP/MQTT 客户端初始化符号 |

构建与回归日志位于工作区 `.build/ble-only/`，不随安装包分发。现有 BLE UUID 保留于 `agent_link/dev_uuid`；Wi-Fi 配置、状态字段、云平台配置和旧身份迁移代码已移除。

## 历史硬件与烧录设置

esptool 确认 ESP32-S3 rev 0.2、16 MiB Flash、内置 8 MiB OPI PSRAM。USB 为 ESP32 USB-Serial/JTAG。BLE 地址为 `10:B4:1D:C4:86:19`，设备 UUID 在重新烧录后保持 `42062738-920e-477b-8b5c-249fe03a1bfa`。

测试前已完整备份 16 MiB Flash，SHA-256 为 `5fbe1a433c2e87cedfc6c86babde4be6498ae7f33dd2e8e18b44dcf5e22f5697`。使用原分区表：`ota_0` / `ota_1` 各 3 MiB，保留原 NVS、SPIFFS、model 和 coredump 区域；上传采用分段写入，未执行全片擦除。

编译使用 ESP32-S3、16 MB Flash、OPI PSRAM、USB CDC On Boot、Hardware CDC。临时 sketch 的 `partitions.csv` 来自板上原分区，其生成的分区表与备份相应字节一致。其他板子必须按实际容量与引脚选择设置，不能照搬此次确认的容量。

## 已发现并修正的问题

### 内部 RAM 不足

完整示例原先静态 RAM 占用 124544 字节，初始化外设和 BLE 后内部 heap 只剩 4156 字节，最大空闲块 3060 字节。OTA 预热返回 `ESP_ERR_NO_MEM`，BLE 出现 `Malloc failed`，I/O 清单传输超时。

将仅由任务访问的 manifest、I/O registry 和 BLE 控制发送队列改为启动时优先分配 PSRAM，失败回退内部 RAM，双失败明确返回错误。保留原容量、FIFO、锁及停止语义；ISR、RX critical section、DMA、NimBLE MSYS、Flash 写缓冲和任务栈仍使用内部 RAM。

首次内存迁移后静态 RAM 降为 **98864 字节**，释放 **25680 字节**。OTA 预热成功后，断开 BLE 时内部 heap **21008 字节**，最大块 **12276 字节**；连接并执行控制命令时仍有约 **20 KiB**，该阶段观测最低水位 **19060 字节**。该阶段完整 BLE 固件大小为 **1485559 字节**，小于原 3 MiB OTA 槽。后续发送调度与诊断优化会略改变最终大小，该余量仅对应本配置，增加业务仍须检查内存。

### 麦克风协议配置

依据板厂 [录音教程](https://arduino.me/a/3107) 和教程所列型号的 [I2S 麦克风数据手册](https://files.waveshare.com/upload/0/01/MSM261S4030H0R.pdf)，默认接法修正为标准 Philips I2S：BCLK41、WS42、DIN2；32 位槽，取高 16 位输出 16 kHz 单声道 PCM。保留确认两线 PDM 变体时的 `AIOT_MIC_USE_PDM=1` 及 RIGHT 选择。

**此次板上的实际麦克风输入仍不正常。** 两声道标准 I2S 分别三次读取 32000 样本，原始数据均为 `0xFFFFFFFF`，PCM 为 -1，变化数及 AC RMS 为零；完整驱动也复现恒值。旧 PDM 设置产生可重复的异常直流数据，同样不能证明有效收音。DMA 能读出数据只证明数字读通路运行。

示例的 `?` 增加样本数、极值、DC、AC RMS、变化数、削波和堆水位；足够样本仍无变化时显示异常提示。未以滤波、虚构音频或软件改值掩盖问题。尚需现场核对麦克风实物型号、WS42、供电及使能/焊接；当前不能认定麦克风损坏，也不能将收音标为通过。

### USB 串口背压与语音发送

串口窗口关闭后，HWCDC 的连接标志仍可能为真；原先多行诊断打印会因 TX 背压阻塞主循环，导致 BLE ACK 超时及采集丢块。现在诊断先生成固定大小快照，优先使用 PSRAM，每轮按可写容量发送最多 64 字节；保留未完成快照和新的 `?` 请求。短日志也检查容量，CDC TX timeout 设为零，容量变化不会导致业务等待。诊断内存不足按有界间隔重试，不阻止 Agent 工作。真实 `.ino` 的平台边界回归覆盖满 TX、部分写入、分配失败及后续恢复。

初始连续 VOICE 测试还出现控制通知被音频抢占，触发原有 3 秒故障断连。现在控制 FIFO 及已取出、正在重试的控制包保留发送优先权；VOICE 满包立即发送，短尾最多等待 20 ms 合并新 PCM，END 立即刷新。没有更改采样率、线协议或虚构音频。

设备请求 15–30 ms 间隔；此次 Windows 曾接受 15 ms 后又改回 60 ms。已观察到 DLE 251 字节及 2M PHY，不能据此保证所有接收端的持续吞吐。PC 测试没有强设 ThroughputOptimized。

Windows/Bleak 的对象关闭并不等于物理 GAP 断开。旧测试器关闭客户端时未清 CCCD，重开同一 ACL 后不会产生新的 READY 边沿，清单可通过协议 `0x34` 重新获取；这不是完整清单不存在。后续测试器在退出时显式清除四项 CCCD，再验证重新订阅后的完整清单与控制查询；必须将订阅周期与有实证的物理重连区分。

## 已完成的自动检查

| 检查 | 结果与边界 |
| --- | --- |
| 外设与完整示例启动 | Audio、LCD、Camera、Agent 初始化及 OTA 预热均 `ESP_OK`；并不代表实际声音与屏幕观感验收 |
| 相机连续采集 | 独立真实驱动 100/100 帧成功，平均 118.1 ms、最大 139 ms；更正麦克风后追加 20/20 帧，平均 115 ms、最大 155 ms；未观察到重启 |
| 图像内容 | 串口导出真实 240×240 JPEG，3973 字节，校验及 JPEG 首尾正确；电脑 Pillow 完整解码通过，并保存解码 PNG |
| Android App 拍照 | RoRoLee 3.4.2 调用 `camera0`，4350 字节 JPEG 使用正确 wire format=0；用户确认收到照片和画面描述，单张 JPEG 端到端通过 |
| LCD | SPI 驱动、提交及预览路径返回成功；未现场确认颜色、方向、背光与中文字形 |
| 扬声器数字通路 | 1 kHz 测试音，32000 字节全部交付 I2S DMA，排队归零，约 1081 ms 完成；未现场确认声音与音量 |
| 麦克风 | I2S DMA 运行、无该次超时/丢块；实际 PCM 恒值，**有效收音未通过** |
| SD 保护 | 正确返回 `ESP_ERR_NOT_SUPPORTED`，在初始化 SPI 前拒绝 GPIO35/OPI 冲突；未驱动冲突引脚，未挂载或创建录音文件 |

SD MISO 接 GPIO35，而此模组的 OPI PSRAM 占用该引脚。此板无法按当前 SD 接线完成录音、列表或回放测试；不得将真实 OPI 菜单强改 QSPI 来绕过检查。有关保留引脚见 [Espressif GPIO 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/peripherals/gpio.html)。

## 最终 BLE GATT 控制实测

最终默认构建 **未开启 BLE 诊断宏，也未强设 PC 的连接参数**。全程 **34.886 秒没有打开 COM7 串口读者**，**177 项通过、零失败**，另有 1 项因 SD 不可用跳过。覆盖设备信息、DIS/BAS、能力、完整 I/O manifest、读数策略、LED/音量/文本执行器、合法与非法 UTF-8、重复 VoiceReply START/END ACK，以及 5 次 CCCD 再订阅恢复。

每次恢复均自动收到完整清单，设备 UUID 保持不变；共 100 次压力查询、134 次命令响应全部收到，中位延迟 **59.735 ms**、P95 **60.747 ms**、最大 **63.343 ms**。没有使用 bootstrap 超时后的手动清单恢复来冒充本轮自动恢复；测试退出的 24 次 CCCD 清零均成功，清理错误为零。这里验证的是订阅周期，不能将 WinRT 对象关闭次数记作物理 GAP 断连次数。

最终 S3 固件 **1486023 字节**，静态 RAM **98912 字节**，比原完整示例少 **25632 字节**；使用原 3 MiB 双 OTA 槽，上传写入校验通过。OTA 预热仍为 `ESP_OK`。

## 最终连续 VOICE 实测

最终短尾合并及非阻塞串口版本，PTT 按下到松开约 **16 秒**：设备读出 **255360 个 PCM16 样本**，接受 **510720 字节**，电脑实际接收 **510720 字节**，三者一致。**16 项传输检查通过、零失败**；共 **2504 帧**，序号 0–2503 无缺口，满包 204 字节，最后一包 108 字节。通知跨度 **15.9367 秒**，接收平均 **32046.76 字节/秒**。

设备 RX(queue+ISR)、pendingTimeout、pendingReplacement、cancelPending 均为 **0**；本地 END 为正常 `ESP_OK`，READY 保持，真实中文 Prompt“你好”到达。内部 heap 恢复为 **20728 字节**，本轮最低水位 **17628 字节**，最大内部空闲块 **12276 字节**；未观察到重启或异常断连。测试退出清除 4 项 CCCD，清理错误为零。

此前未合并短尾时，375552 字节用了 2217 通知，平均载荷约 169.4 字节，记录 10 项混合丢块并以 incomplete END 结束。最终平均载荷约 203.96 字节，达到业务 PCM 速率且上述计数为零；没有扩大音频 ring 或降低采样率。

**传输通过不等于收音通过。** 保存的 WAV 仍为恒值 -1，AC RMS 为零、仅一个不同样本值，麦克风有效信号独立判为失败。旧 VOICE 协议没有可通知对端的 END 完整性标志，本轮正常结束依据设备日志与接受/接收计数，未虚构线上 END 事件。

## WiFi 配网实测

使用完整示例的 `AIOT_USE_WIFI=1` 模式，电脑实际连接开放热点 **`Deotaland-AIOT-Basic-8618`**，访问 **`192.168.4.1`**。**17/17 项通过**：7 项 HTTP 页面/状态/缓存扫描/图标/Android、Apple 及未知地址重定向，DNS A 指向板上门户及 AAAA 空响应，8 项无效表单均返回 400 且配网状态不变。

WiFi 初始化成功，门户活动；该次内部 heap 为 66988 字节、最低水位 63148 字节。测试后确认恢复电脑原 WiFi profile，并删除本次临时 profile 和 XML；没有读取保存的密码、提交有效 WiFi 凭证或执行忘记绑定。首次隐藏 PowerShell 的字符解码失败发生在切网前，属于主机辅助程序问题，修正后真实测试通过。

没有有效凭证，因此未验证板子加入路由器、NVS 保存新凭证或云平台绑定。`wifi_ip` 是 STA 地址，在仅门户活动时为空；这不代表 `192.168.4.1` 门户不存在。该阶段的 0.8.2 尚无 WiFi 控制/媒体数据通道。0.9.0 新增 MQTT 控制，仍不提供 WiFi 媒体、不进入 BLE 的 `READY`，验证边界见末节。

## RoRoLee Android CoC 实测与修复

用户随后准备了 RoRoLee Android App，并确认已连接设备、收到硬件发送的“你好”，但没有收到照片，且没有主动断开蓝牙。此阶段使用相同硬件和分区，开启 `AGENT_LINK_BLE_LINK_DIAGNOSTICS=1`，电脑持续读取 COM7；与前述关闭串口读者的默认构建测试区分。

首张失败照片共 **4103 字节**，API 已全部接受，但 NimBLE 仅提交 **4096 字节**；最后 7 字节发送返回 `BLE_HS_ENOMEM`。此时对端 MPS 为 251、剩余发送信用为 65511，随后板端主动终止连接并自动重连。因此“JPEG + END accepted locally”不能用来判断手机已收到图片。

发送工厂使用 MSYS 小池，而小池耗尽不会转用仍有余量的大池。现在连短尾 SDU 也从大池申请；在 NimBLE host 线程按真实工厂、小池余量及对端 MPS 计算发送预算，保留 6 块控制余量，连续两段后让出 CPU。资源不足时仅在 SDK 接受 SDU 前等待并重试；SDK 的 `ENOMEM` 仍可能发生在部分数据发送后，因此保留终止处理和 `ESP_ERR_NO_MEM`，禁止重放该 SDU。

首轮修复诊断构建的 Flash 为 **1490355 字节**、静态 RAM **98960 字节**，上传校验通过。连续四张真实 240×240 JPEG 的板端提交检查通过：**3962、3931、3999、4004 字节**均与接受量和声明总长度一致，成功发送结束消息，`rejected=0`、`truncated=0`、`last=ESP_OK`，连接保持 READY。短暂小池不足在发送前等待后恢复，未发生 SDK 内存不足或断连。

随后 AUDIO 持续采集约 **9.6 秒**，接受和提交均为 **308480 字节**，队列归零，状态 COMPLETE，拒绝写入及采集丢块计数均为零；再拍照正常完成。麦克风 PCM 仍为恒值 -1，故本项仅证明板端采集/提交过程，不能认定有效收音或 ASR 通过。上述 CoC 提交量不能代替手机实际接收/解码验收。

用户确认 **RoRoLee 3.4.2 仍未显示这些照片，IMAGE 端到端验收尚未通过**。在 App 输入“用语音说你好”后出现文字回复“你好”、没有音声，板端 TTS PCM 接受量和播放 DMA 均为零；尚无证据表明 App 已生成并向设备发送 TTS，因此不能直接判为硬件播放失败。继续检查控制帧、CoC 收发与 App 行为，OTA 仍未进行 App 端到端验收。首轮修复的原始日志和指纹保存于 `app-fixed-live.log`、`app-first-fix-result.json`，失败对照为 `app-diag-live.log`、`app-image-before-fix.json`。

另补低 MPS 的预算适配：如果整个小池也容纳不了当前段，在 SDK 接受之前缩短段并沿原偏移重新切片；仅暂时余量不足或短尾不足不会永久降低段长。生产策略 **145 项**和真实 host/vendor factory 边界 **23 项**通过，包含 MPS 23、4103 字节奇数尾完整提交、瞬时短尾背压、会话失效及消费后的内存不足不可重播。它们是主机边界检查，未代替手机端接收验收。

随后烧录增加控制帧头、IMAGE 通知元数据、订阅和 CoC 接收计数的诊断构建：Flash **1492035 字节**、静态 RAM **99016 字节**，上传校验通过，App 自动重连、ATT MTU 255、CoC81 已连接。此构建不打印聊天、凭证或 JPEG 正文。关闭诊断的默认构建也编译通过（Flash **1487323 字节**、静态 RAM **98936 字节**），目前尚未替换上述诊断固件。相同修复的无 PSRAM C3 BasicBLE 编译零警告（Flash **1464335 字节**、静态 RAM **68192 字节**）。最新诊断日志为 `app-wire-live.log`，构建指纹为 `build-app-coc-fix-diag.json`。

该诊断构建实际发送一张 3999 字节 JPEG，`0x54` 的 19 字节及 `0x55` 的 15 字节与原版布局一致，均在事件 handle 27 提交成功；仍不能代替 App 接收证据。用户从 App 请求 `camera0` 后显示“设备操作失败，camera0”；本阶段完整写入记录仅有两条 `0x04` 文件列表请求，没有预期的 `0x33` 拍照命令，也没有 `0x05` TTS。

为对齐原版，将完整示例及 CameraSnapshot、ImageSequence 的 `camera0` 恢复为 `display_name="Camera Snapshot"` 和缺省 `args_schema`，保留 BOOL、回调及线协议。重新烧录完整示例（诊断构建 Flash **1492055 字节**、静态 RAM **99016 字节**，校验通过；默认构建 **1487343/98936 字节**也编译通过）。用户在 App 真正断开并重连后，串口确认 FFC1、FFC4、FFA1、FFB1 均已订阅、两条 CoC 均连接，初始化中收到 `0x34` 清单请求。

此后 **80.712 秒收到 App 的 `0x33`（payload 9 字节、seq 176）**，自动触发拍照，**4026 字节全部提交、结束状态成功**，连接保持。此阶段串口命令文件为空，因此这张照片由 App 调用触发；日志为 `app-camera-live.log`，指纹为 `build-app-coc-fix-diag-camera-metadata.json`。这次恢复元数据与刷新连接一起进行，不能认定其中单一因素就是前次调用失败的根因。用户随后确认 App 回复“设备没有返回照片，只返回了运行时间信息”，故图片端到端仍未通过。用户未找到播放选项，尚未收到 TTS PCM；不修改音频数据来掩盖麦克风异常。

### 拍照响应与运行时间对照

追加受诊断宏控制的真实 `0x33` 响应和最多三条 `uptime0` Reading 元数据记录，排查响应串包。示例增加 `AIOT_UPTIME_REPORT_INTERVAL_MS`：默认 1000 保持原行为，0 停止自动采样/推送，但保留 `uptime0` 端点、可见性、清单和拍照协议。0 与 1000 两种诊断构建均零警告；关闭诊断的默认构建也零警告。

遥测开启阶段，**220.088 秒收到 App 的 `0x33 seq251`**；220.092 秒向 FFC1（handle 24）实际提交正确 10B 成功响应：`01 02 33 FB 04 00 33 00 00 00`，`rc=0`，没有运行时间 Extra。随后一张 **4191 字节 JPEG** 全部提交，成功的 `0x55` 在 220.395 秒送至 FFC4（handle 27）；220.416 秒开始，三条独立的 `0x19` 事件（`seq=0`、`id=uptime0`、int32/4 字节）送至 FFC4。用户确认 App 仍只返回运行时间信息。因此已排除这次固件将运行时间放进拍照 ACK 的情况；尚不能仅凭板端发送记录确认客户端怎样接收、解码或整理图片。

此阶段诊断构建 Flash **1492511 字节**、静态 RAM **99016 字节**，日志 `app-uptime-on.log`、指纹 `build-app-coc-fix-diag-ack-uptime-on.json`。随后仅把运行时间间隔设为 0 的对照构建已烧录并校验，Flash **1491899 字节**、静态 RAM **99016 字节**，日志 `app-uptime-off.log`、指纹 `build-app-coc-fix-diag-ack-uptime-off.json`。

停推阶段，**527.556 秒收到 App 的 `0x33 seq94`**，527.561 秒提交成功响应 `01 02 33 5E 04 00 33 00 00 00`（handle 24、`rc=0`）。一张 **4306 字节 JPEG** 的接受量、SDK 提交量与声明长度一致，527.888 秒提交成功的 IMAGE END（handle 27、`rc=0`）；此后没有运行时间通知。用户反馈变为“设备已接受拍照请求，照片处理中；目前没有收到图像，所以暂时无法描述画面内容”。这说明停推后 App 识别了拍照请求，但图片端到端仍未通过，不能用板端提交成功代替手机接收验收。手机可能保有历史读数，不应把后续提及旧运行时间直接当成板端仍在推送的新数据。对照固件与默认发行配置分开记录，不能把停推运行时间视为照片接收通过。

### 图片结束通知时序对照

为区分 GATT 结束通知与 Android 独立 CoC 接收线程的时序，另外复制临时实验库，在完整 JPEG 提交后、IMAGE END 前增加一次可取消的 **500 ms** 等待；运行时间推送仍关闭。等待后重新检查连接、通道 epoch、取消状态和字节数，不改变 START、JPEG 分段、MTU 或发送计数。该等待仅在 `.build/hardware-aiot/image-end-grace` 的临时库中，**没有加入发行源码或安装包**，也不能保证客户端接收。

实验构建零编译警告，Flash **1492787 字节**、静态 RAM **99016 字节**，COM7 上传校验通过，App 已重连。临时 transport SHA-256 为 `fc0349f5a3e6aca00a25fcd937eb2d32b2b8e790038ade8f13796692382bb3b6`，二进制 SHA-256 为 `ca7850092d8ac9ca7b2c093c6d5b0d3feb027fe848f4d96070a438b07d22e636`。日志为 `app-end-grace.log`，构建指纹为 `build-app-coc-fix-diag-image-end-grace.json`。

**90.554 秒收到 App 的 `0x33 seq87`**，90.559 秒提交成功 ACK；90.716 秒提交 IMAGE START，声明 **4336 字节**。90.907 秒进入等待，91.407 秒结束等待并记录接受量、提交量和总长度均为 4336，`status=0`、`rejected=0`、`truncated=0`；91.417 秒 IMAGE END 提交 `rc=0`。实际执行了 500 ms 对照等待，仍没有运行时间通知；用户确认“设备已接受拍照请求，但目前仍未收到照片，因此暂时无法描述画面内容”。延迟对照未解决收图，未将等待加入发行实现。随后重新逐项比较原版，发现下面的编码编号差异。

### IMAGE 编码编号修正

原版 `Agent_link-main/firmware/components/agent_link/src/transport_ble.cpp` 为图片定义 `kImgWireJpeg=0`、`kImgWireRgb565=1`，在 `ble_stream_start()` 将 API `AGENT_ENC_JPEG=1` 转为 wire 0、`AGENT_ENC_RAW=0` 转为 wire 1。Arduino 后端原先直接使用 `meta[0]`，导致前述 JPEG 的 START 均携带 `format=1`，被声明为 RGB565。此前“布局一致”的检查仅确认字段长度与顺序，遗漏了编码的数值语义；完整提交字节数和延长 END 均不能修正这一错误。

现在后端按原版显式映射 JPEG/RGB565，其他编码返回 `ESP_ERR_NOT_SUPPORTED`，保留公开 API 枚举、JPEG 数据、PSM、通知字段布局和正常 END 时序。修正后的诊断构建停止运行时间推送，Flash **1491911 字节**、静态 RAM **99016 字节**，零编译警告，COM7 上传校验通过。

**146.560 秒收到 App 的 `0x33 seq253`**，146.565 秒提交正确 ACK。146.707 秒发送 `0x54`：`01 03 54 00 0D 00 01 00 00 00 00 F0 00 F0 00 FE 10 00 00`，JPEG 的 `format` 已为 **0**，尺寸 240×240、声明 4350 字节。146.895 秒接受量、提交量和总长度均为 **4350**，`status=0`、`rejected=0`、`truncated=0`；146.910 秒 END 提交成功。用户确认 **“现在有照片和描述了”**，本次 Android 图片端到端验收通过。正常 END 时序下成功，无需实验延迟。日志 `app-image-format.log`、指纹 `build-app-coc-fix-diag-image-wire-format.json`；transport SHA-256 `d2326ea88bf1f923f9e370e3fb224a2b01592f07532ad2ca30c0ef963587f44a`，固件 SHA-256 `a4ad691f3feb90474df0e39ae7d0ff9cfdfb3bae92c7887dfaf82f03d8b82b59`。

生产与原版实际 `ble_stream_start()`、`ImgSendStreamStart()` 的源码抽取检查共 **23 项通过**：JPEG/RAW 的完整 19B 通知逐字节一致，并另用固定协议值验证 JPEG=0、RGB565=1、尺寸/长度的小端序、非法 meta 和不支持的编码。该检查使用通知边界捕获，不是手机测试；实机成功由上述 App 反馈单独确认。真实 SDK transport 对象的诊断开/关编译均通过。

另将完整示例的 `uptime0` 设为 `AGENT_AUD_USER`，清单携带 `"audience":"user"`，保留 App/用户诊断读数，并让遵循该标记的 App 向 AI 隐藏运行时间端点；不删除端点、查询、订阅或上报。正常配置恢复默认 1000 ms 更新，此优化与图片编号修复分别记录。

收尾烧录关闭 BLE 诊断、使用默认运行时间周期的正常构建，Flash **1487367 字节**、静态 RAM **98936 字节**，零编译警告，上传校验通过。App 自动重连，串口检查 Audio/LCD/Camera/Agent 初始化均成功、状态 READY、ATT MTU 255、CoC81 已连接，身份 UUID 保持不变；检查结束后关闭串口读者。日志 `app-image-final.log`、指纹 `build-app-coc-fix-final-image-final-user-uptime.json`；最终固件 SHA-256 `14fa1c9eddcf01e20c571143a6e306ba28a84562e96e2813a39e524c9b4c3e8b`。图片成功验收对应前述诊断构建，收尾构建另确认启动和重连，不把它记为额外一次手机收图测试。

## 验证边界

Windows 的 Bleak/WinRT 用于实际 GATT 检查，未提供此次测试所需的 BLE CoC 通道。最初 PC 阶段没有配套手机 App；随后新增的 RoRoLee Android 阶段见上节，已确认一次拍照调用、照片接收与画面描述。尚未完成手机实际音频接收/ASR、TTS PCM 播放，以及 OTA 固件接收、重启、回滚验收；OTA 成功预热仅验证本地资源准备。RAW、持续图片上传与其他手机/板卡未随单张 JPEG 验收一并判定通过。

没有平台地址、产品与账号配置，因此未进行实际云激活/绑定或心跳验收。未现场观察 RGB、LCD 或扬声器效果。上述项须保留为未验收，不能从编译成功、ACK 或驱动成功推断通过。

## 回归与产物

临时 sketch、主机边界替身、原始日志、BLE WAV、JPEG、Flash 备份均保存在源码工作区的 `.build/hardware-aiot`，不随 Arduino 安装包分发。库中未恢复 `tests` 或 `extras`。

修改后运行真实生产核心的 C++11 回归 139 项、实际 ESP32 核心存储生命周期 57 项、控制队列与存储 ESP 分支 116 项/便携分支 109 项、BLE 调度与短尾策略 94 项、STD/PDM 音频接收各 152 项，以及完整示例状态机/串口背压 20 场景、140 断言。上述检查通过；边界替身仅代替 heap/时钟/驱动等平台设施，不能替代上述真实硬件结果。

真实 Arduino Core 编译覆盖 S3 完整 BLE、内存修复阶段的 S3 完整 WiFi、PDM 兼容分支及无 PSRAM 的 C3 BasicBLE，零编译器警告。C3 默认 1.25 MiB 应用槽不足，改用实际容量支持的 `min_spiffs` 约 1.9 MiB 双 OTA 槽后通过。无 PSRAM 时使用内部 RAM 的实现已在平台分配边界回归中验证；未连接第二块 C3 做实物测试。

PC 阶段结束后清理电脑 BLE 会话并复位开发板，保留当时默认 BLE `AiotBasicAgent` 固件。复位后 Audio/LCD/Camera/Agent 与 OTA 预热均成功，UUID 保持不变，状态为等待连接；内部 heap 为 20960 字节，最大空闲块 12276 字节。之后为 App 阶段重新烧录并打开串口诊断，见上节。没有写入新的路由器凭证或云平台配置。

## 0.9.0 全功能审计与电脑复测

此次范围由用户确认：RoRoLee 与原有 BLE/WiFi；不包含独立 Muse 后端。对照当前原项目实际协议、核心、网络、OTA 及 Korvo 录音源码，而非只对照旧 README。完整功能矩阵和协议边界见[审计对照](UPSTREAM-COMPARISON.zh-CN.md)。

### 已修正的问题

- TTS：已到达的 CoC SDU 现在先占同一接收 FIFO 的位置，`poll()` 按背压消费后才执行其后 GATT END/下一 START，修复尾部音频丢失；保留真正 END 后迟到数据的过滤和 OTA 共用通道所有权。
- WiFi：补齐原版 MQTT token、时钟与 JSON 命名/raw 控制应答。控制回调在 `poll()` 执行，worker 处理网络；HTTP 云在线保持 CONNECTED，MQTT 另行连接，不向 WiFi 发布主动事件或媒体。
- 录音目录：动态缓存支持超过 128 个文件，内存不足、`readdir`/`stat` I/O 错误明确失败，恢复刷新后可继续使用；可编码文件名与 uint16 分页边界沿用原协议。
- BLE：在连接过程结束后请求 2M PHY；协商失败不妨碍 READY，实际参数由中央设备决定。VOICE 缓冲仍为 16 KiB，采样率仍为 16 kHz。

### COM7 最新实测

先使用之前的默认固件建立对照：GATT 129 项通过，但 16 秒采集结束时记录 1 个 pendingTimeout 与 incomplete END。电脑接收的 505600 字节与板端已接受量一致，不能把重试次数算作丢失包数；该轮有效麦克风信号仍失败。

随后烧录 0.9.0 诊断构建，原分区与 UUID 保持一致，写入校验通过。Flash **1537243 字节**，静态 RAM **99624 字节**；BLE/核心源码在测试期间冻结。日志为 `.build/hardware-aiot/all-functions-final-serial.log`，原始通知和结果为 `.build/hardware-aiot/all-functions-final/`，构建命令、全运行源码 SHA 和固件 SHA 为 `build-app-coc-fix-diag-all-functions-audit.json`。

| 检查 | 最新结果 |
| --- | --- |
| GATT 控制 | **129 项通过、0 项失败、1 项 SD 跳过**；设备身份/DIS/BAS、完整清单、I/O/策略、LED/音量/文本命令、UTF-8 异常、重复 VoiceReply ACK、3 轮 CCCD 重新订阅恢复 |
| 控制响应 | 92 次响应；中位 **59.791 ms**、P95 **63.175 ms**、最大 **90.510 ms**。旧对照中位 119.717 ms；两轮实测不代表其他手机的保证值 |
| 连续 VOICE | 采集 **255040 样本**；接受、提交、电脑接收均 **510080 字节**；2501 帧，序号 0～2500 无缺口，15.936147 秒，平均 **32007.74 B/s** |
| 板端完成状态 | 正常 END `ESP_OK`；队列归零；RX(queue+ISR)、pendingTimeout、pendingReplacement、cancelPending 均为 **0**；169 次写入拒绝后重试不是 169 个丢失包 |
| 实际链路 | ATT MTU 255，DLE 251，观察到 30 ms 间隔、双向 2M PHY；仅确认本次连接 |
| 内部 RAM | 测试后 heap **19836 字节**、最低水位 **16908 字节**、最大内部块 **11764 字节**；未观察到重启或意外断开 |
| 麦克风 | PCM 仍恒为 -1，AC RMS 0、distinct=1；**有效收音独立判为失败**，VOICE 传输通过不改变此结论 |
| SD | OPI/GPIO35 保护继续生效，未挂载，未把无卡/冲突情况下的列表、录音、回放标为通过 |

测试器退出清除四项 CCCD，电脑会话已结束；WinRT 对象关闭不能算作真实 GAP 重连。本轮手机尚未重新验收 TTS PCM、完整 AUDIO/ASR 或 OTA 收包/重启/回滚；单张 JPEG 的手机成功结论仍对应前述 RoRoLee 图片验收，不能扩大为这些功能均正常。

录音 worker 的独立生产源码回归为 PCM 45、ADPCM 45、OPI 防护 1，共 **91 项通过**：每个目录 257 个新增文件完整分页、扩容 OOM 与恢复、扫描 EIO 与恢复，以及 WAV/ADPCM 编解码和读写生命周期。真实 Core 3.3.12 的 PCM/QSPI、ADPCM/QSPI、OPI 对象编译均零警告。这些文件系统/调度边界检查保存在 `.build/audit-all-recording/`，不能替代实际 SD 卡验收。

### 新固件 RoRoLee 复测与最终构建

电脑测试结束后，用户用 RoRoLee 重新连接并再次调用 `camera0`。串口 **311.746 秒**收到 `0x33 seq188`，正确 ACK 后上传 **11683 字节、240×240 JPEG**，wire format=0；312.144 秒 accepted=submitted=total，`status=0`、`rejected=0`、`truncated=0`，312.153 秒 END 成功。MSYS 短暂不足在 SDK 提交前背压重试，最终 sdk_rc=0，连接保持 READY。用户确认 **“已连接，照片和描述正常”**，本次新 BLE/TTS 顺序修复构建的图片端到端复测通过。日志为 `.build/hardware-aiot/all-functions-app.log`。

用户未找到语音回复或固件升级入口；此次板端 TTS 接受量和播放 DMA 仍为零，故未对手机 TTS 或 OTA 作通过/失败推断。实际麦克风与 SD 问题继续保留。用户不需要为了正常收图寻找这些入口。

独立 WiFi 复核发现 JSON 桥可能在新清单请求执行之前混入旧主动清单尾片，已改为等待本次成功 ACK 再收集。实际 JSON/session/MQTT/API/worker/core 整合共 **1033 项断言通过**，另有独立旧清单竞态 12 项通过，含 ACK 背压后恢复；记录在 `.build/audit-all-wifi/` 和 `.build/audit-network-wifi-review/`。平台账号、有效 broker 和网络边界使用替身，实际云端仍未验收。

| 最终 SDK 构建 | Flash / 静态 RAM | 结果 |
| --- | --- | --- |
| S3 OPI/16M 完整 AiotBasicAgent，BLE 诊断关闭 | 1532095 / 99536 字节 | Core 3.3.12，零警告，COM7 上传校验成功 |
| C3 无 PSRAM BasicBLE，min_spiffs | 1516599 / 68832 字节 | Core 3.3.12，零警告；未连接 C3 实物 |
| S3 OPI/16M WiFiMqttControl | 1363682 / 77664 字节 | 全 185 个库/示例编译单元 PRODUCT_ID=1，真实 MQTT 链接，零警告 |
| C3 无 PSRAM WiFiMqttControl | 1512893 / 68156 字节 | 全 185 个库/示例编译单元 PRODUCT_ID=1，真实 MQTT 链接，零警告 |

COM7 最终正常固件 SHA-256：`b05ee533662010f0fa5d2f3e1bc522e18ce5795b051e7d1071131584bb9d4e82`。命令和全运行源码指纹保存于 `build-app-coc-fix-final-all-functions-release.json`；正常构建与实测诊断构建的 BLE/核心源码一致，最后新增的 WiFi ACK 关联修复也已编入。图片与 VOICE 的实测结果对应上述诊断构建，不能把最终烧录另算一次收图或 VOICE 验收。

最终启动检查 Audio/LCD/Camera/Agent 均为 `ESP_OK`，UUID 保持不变，恢复 READY、ATT MTU 255、CoC81 连接；内部 heap 19968 字节、最大块 11764 字节。日志 `all-functions-release-boot.log`；检查后关闭 COM7 读者，保留可供 App 使用的正常固件。

## 0.9.1 WiFi 连接实测

用户随后提供 vivoX300 的 2.4 GHz WPA2 热点。使用同一 COM7、OPI/16M 与原分区表，临时切换 `AIOT_USE_WIFI=1`，平台配置保持为空。测试钩子仅放在 `.build/hardware-aiot/wifi-connection/`：记录状态变化、NVS 凭据 blob 是否存在及字节数，调用 SDK 主动断开和软件重启；未向固件源码、日志或安装包写入用户密码。

| 实物检查 | 结果 |
| --- | --- |
| 无凭据首次启动 | 配网热点 `Deotaland-AIOT-Basic-8618` 正常开启；没有 STA IP，NVS 尚无凭据 blob |
| 2.4 GHz 扫描 | 配网页能找到 vivoX300；电脑同时确认 WPA2、信道 11。手机热点重新开启后的重启检查为信道 1 |
| 非法表单 | 原始 NUL 出现在 SSID 后、密码中，以及 `%00` 编码，均 HTTP 400 且状态不变 |
| 错误密码 | 三次认证/握手失败后 `/status` 为 `failed/badpass`，没有 IP；再次查询仍无 NVS 凭据，页面可继续使用 |
| 正确密码与 DHCP | HTTP 正常接受，约 2.54 秒内网页显示 `connected`。IP `10.243.117.67`，掩码 `255.255.255.0`，网关/DNS `10.243.117.150` |
| 成功后保存 | 仅连接并成功提交 NVS 后，页面报告 connected；实际 NVS blob 为 102 字节，之后刷新和重启仍存在 |
| 撤除配网热点 | 连接快照 150.618 秒为 connected=1/portal=1，154.619 秒为 connected=1/portal=0，间隔 **4.001 秒** |
| 主动断开后的恢复 | SDK disconnect 返回 ESP_OK，状态清空 IP；约 **5.6 秒**后自动获得 IP，无需重新提交密码 |
| 真正关闭/开启热点 | 用户关闭约 15 秒再开启；串口观察到掉线、找不到 AP 的重试及再次获得 IP。观测的总断网窗口 **34.603 秒**，包含人工关闭时间及退避，不是精确的热点开启后耗时 |
| 重启自动连接 | 无预设 SSID/密码的固件软件重启，读取 NVS 后在启动约 **2.868 秒**已 connected=1/portal=0，无需配网 |
| 启动网络不存在 | 仅临时草图配置不存在的 SSID，验证预设优先于保存网络；启动约 **168.1 秒**后按八次失败策略回到门户，原 NVS 凭据仍为 102 字节 |
| 回退门户可用 | HTTP、扫描、状态、重定向、DNS 和非法表单共 **17/17 项通过**；回退后的配网页可以实际访问 |

错误密码的最终网页序列 **8/8**、正确密码序列 **4/4** 均通过。首轮探测曾遇到一次 2 秒首页读取超时，且该轮最后断开原因是 notfound；加入 SDK 原因记录、使用 8 秒读取超时复测后，实际观察到 reason 204/15/15、badpass 和正常页面响应。保留首轮失败原始记录，不把它合并成零失败。期间电脑每次都恢复原 WiFi profile，删除本次临时 profile/XML；没有读取电脑保存的密码。

源码审计并修复配网页原始 NUL 字节被 C 字符串解析器截断的问题。现在在停止扫描、改变状态或提交凭据之前返回 HTTP 400。生产源码与实际 IDF 查询解析函数的 **163 项主机检查通过**；Core 3.3.12 S3/C3 的 HTTP 编译单元均零警告。证据在 `.build/audit-wifi-connection/`，源码 SHA 为 `901e40035cbf5f38a2886aa2416c6c76de3ba61b04e2d8e349f6a6e9a00f5d50`。

测试的是网络连接层：DHCP、网关及 DNS 地址配置已实测；这些结果不代替 DNS 外网解析、互联网 HTTPS、平台激活或 MQTT broker 验收。无平台配置时状态为 NETWORK_READY，`isReady()` 保持 false，不能用 BLE READY 或云 CONNECTED 来判断是否已连接 WiFi。退避为 4/8/16/30 秒封顶；启动未成功时按失败次数回门户，曾成功联网的运行中设备持续重连，不自动回门户；没有新增 DHCP 总等待超时。

测试结束恢复 **0.9.1 正常 BLE AiotBasicAgent**，不包含测试钩子或不存在的 SSID。S3 完整构建 **1532131 字节 Flash / 99536 字节静态 RAM**，无 PSRAM C3 BasicBLE **1516649 / 68832 字节**，均零警告。COM7 上传校验成功，固件 SHA `d39afd2f089e7e8de732540c791c0fe54997e726befe16a84327ea28a9907e6c`；Audio/LCD/Camera/Agent 初始化成功，UUID 不变。WiFi 凭据保留于 NVS；后续使用无预设凭据的 WiFi 模式可以自动连接此热点。

最终结果及各轮原始日志位于 `.build/hardware-aiot/wifi-connection/`。恢复后电脑只扫描广播，确认 `Deotaland-AIOT-Basic`、目标 BLE 地址及产品 UUID，没有占用蓝牙连接；串口读者已关闭，设备等待 App 连接。测试工具与日志不进入 Arduino 安装包，也未恢复 `tests` 或 `extras`。

## 0.9.2 WiFi 云端复核与待完成的实机检查

原项目 `firmware/boards/korvo-cloud/config.h` 的实际配置为 `https://bot.zhaojun.work`、`product_id=1`、`chip_type="esp32-s3"`。注释说明产品 ID 是创建 Korvo Cloud 产品之前暂用的现有 ID；这不是 AIOT-Basic 产品或当前账号可成功绑定的保证。用户同意沿用原平台并先核对配置。

电脑经当前网络使用正常 CA 和主机名校验，公开 GET 平台首页及 `/api/device-gateway/claim-code` 均收到 HTTP 403、Cloudflare `error code: 1010`。这里没有发送 POST、认证密钥或绑定请求；GET 不验证设备网关的 POST 协议，也不能断言开发板经手机热点会得到相同响应。证据为 `.build/hardware-aiot/wifi-online/pc-preflight.json`。

COM7 已烧录独立 WiFi 诊断构建，但新一轮扫描后电脑和开发板均未找到 vivoX300；开发板未取得 IP，之后按启动失败策略回到门户。NVS 的 WiFi 凭据 blob 仍为 102 字节，生产 `al_dev/auth_key` 不存在。缓存读数 `updateInt32("uptime0",42)` 返回 `ESP_OK`，同端点错误类型返回 `ESP_ERR_INVALID_ARG`。无 IP 时发起的 DNS/HTTPS 探测不计为联网验收；在这个热点不可见阶段，未验证开发板的外网 DNS、SNTP、HTTPS、真实平台激活、心跳、token 或 MQTT 控制。当时已请求用户保持 2.4 GHz 热点开启后继续；恢复后的最新联网结果见下节。日志保留在 `.build/hardware-aiot/wifi-online/`。

复核实际生产源码补出并修复两处边界问题：有限寿命 token 的 uint32 续期截止时间恰好回绕为零时，不再与“未给寿命”的状态混淆；MQTT 首次创建 mutex 失败时，立即释放尚未发布的邮箱存储，避免停止后仍残留大块缓冲。未删除已成功创建的 mutex，保留迟到读者的同步保护。生产整合回归 **1041 个断言通过**；独立 token 边界 **21 场景、124 检查**，独立分配失败/停止/重启 **5/5**，均绑定源码 SHA，保留修复前失败记录。主机替身验证不能代替实际云端收发。

Core 3.3.12 的 0.9.2 完整 S3 BLE 构建为 **1531571 字节 Flash / 99536 字节静态 RAM**，关闭自动运行时间推送；无 PSRAM C3 BasicBLE 为 **1516691 / 68832 字节**，均零警告。S3 WiFi 诊断构建同样零警告并完成上传校验，其测试钩子、输出缓冲和临时任务使其体积不同于正常示例。

仅在 `.build/wifi-platform-lab/` 准备了隔离的 HTTPS/MQTTS 测试平台和观测工具，使用临时 CA bundle、合成设备 UUID、单独的测试凭据 namespace；生产库的 TLS 校验和持久化 namespace 不变。离线 fixture 19 项检查通过，尚未生成证书、启动监听或进行开发板 MQTT 测试，不能把该工具的就绪计作真实平台验收。后续验收必须核对 SUBACK、业务 `reply_to`、响应码和设备读回，MQTT PUBACK 或 HTTP 认证成功本身不证明执行器已执行。

等待热点恢复时已恢复 COM7 的 **0.9.2 正常 BLE AiotBasicAgent**，上传校验通过，固件 SHA `cc46764e644adbb241289c2b199aada86842420dd86c490f97eff84539d3dfbe`。Audio/LCD/Camera/Agent 初始化及媒体缓冲预热成功，UUID 保持不变；这些启动检查不扩大为新的图片、语音或 OTA 收发验收。自动运行时间推送关闭，测试钩子不包含在该固件中。电脑仍使用原 `702-5G` profile，没有创建新 WLAN profile 或改变系统 CA；串口检查结束后释放 COM7。后续热点恢复后需再次切换 WiFi 测试构建。

### 热点恢复后的联网实测

用户恢复 2.4 GHz 热点后，再次切换 WiFi 诊断构建。COM7 读取已保存凭据并获得 IP，随后在开发板上完成外网探测；此结果更新前述热点不可见阶段的阻断状态。原始记录为 `.build/hardware-aiot/wifi-online/hotspot-network-repeat.log`，此前电脑 HTTP 403 和热点不可见的记录继续保留，不与这轮合并。

| 开发板实物检查 | 结果 |
| --- | --- |
| 外网 DNS | `www.espressif.com`、`bot.zhaojun.work`、`pool.ntp.org` 均 `rc=0`，各返回 1 个 IPv4 地址 |
| SNTP 时钟 | `valid=1`，记录 Unix 毫秒 `1791807094155` |
| 公共 CA 与正确主机名的 HTTPS | SDK 公共 CA bundle 与主机名校验开启；Espressif `robots.txt` 返回 HTTP 302，未跟随跳转；平台根路径 GET 及 `/api/device-gateway/claim-code` GET 均返回 HTTP 200 |
| 公网错误名称探针 | 故意设置错误 `common_name` 后，返回 `ESP_ERR_HTTP_CONNECT`、`tls_error=30592`，连接被拒绝；该结果也可能来自服务端 SNI 拒绝，不能单独证明客户端名称校验。受控客户端负例见下节 |
| 轮询响应 | 网络探测后台任务共 15078 ms，期间主循环最大间隔 4 ms；HTTP/TLS 等待未阻塞主循环 |
| 实际激活码 POST | 使用真实 `cloud_api_claim_code()` 向原平台发送 POST，`rc=ESP_OK`、`api_code=0`、`success=1`，返回 6 位激活码及 `ttl=600` 秒；文档与安装包不记录该激活码 |

在此网络探测阶段，已验证开发板外网 DNS、时钟、正确名称的 HTTPS、公网错误名称连接拒绝、HTTP 连通和真实激活码申请。当时平台账号绑定、真实 `bind-status`、认证、心跳、MQTT token/broker 和控制收发仍待验收，隔离本地 fixture 也尚待实机执行；后续结果见下节，媒体与 OTA 的既有验收边界保持。

## 0.9.3 原平台与 WiFi 控制实测

用户确认屏幕上的激活码显示正常并完成绑定。以下结果更新前述待验收状态。沿用原项目配置 `https://bot.zhaojun.work`、产品 1、`esp32-s3`；手机热点仍使用之前保存的凭据。真实平台测试使用生产库和完整示例，仅在 `.build` 临时草图加入数值观测钩子，未输出密钥、MQTT 密码或请求正文。

### 原平台

| 实物检查 | 结果 |
| --- | --- |
| 屏幕激活码 | 六位码大字显示，状态刷新不覆盖正文；PAIRING 时暂停自动相机预览；用户确认显示正常并成功绑定 |
| 绑定与持久化 | `bind-status` 返回 bound；`al_dev/auth_key` 存在、占 37 字节，仅查询存在性和长度；重新烧录后不再次 claim/bind，直接认证 |
| HTTPS 认证与 MQTT token | `rc=ESP_OK`、平台 `code=0`、`success=1` |
| MQTT WSS | CONNECTED 一次，两个命令主题收到 SUBACK；连续观测期间 `conn=1/sub=2/dis=0/err=0`，客户端未销毁 |
| HTTPS 心跳与 MQTT 共存 | 连续两轮心跳 `start=2/done=2/ok=2`；每轮 `rc=0/code=0/success=1`，期间 MQTT 保持连接 |
| 原平台命令入口 | 用户没有找到控制入口，因此未宣称完成原平台账号端的执行器收发验收；自动业务测试使用下述独立 TLS 平台 |

证据为 `.build/hardware-aiot/wifi-online/original-platform-screen.log`、`original-platform-memory-tuned.log`、`original-platform-stacks-psram.log`、`original-platform-camera-dma.log`。前两次内存优化尚不足的失败记录保留；最终两轮心跳通过对应最后一个日志及 `build-original-platform.json`，不把较早阶段的 MQTT 连接成功当作心跳成功。

### 内部 RAM 修复

原完整 WiFi 固件首次 MQTT WSS 握手出现 `mbedtls_ssl_setup -0x7F00`。释放闲置 HTTP TLS 连接、在本板 SD 被 OPI 引脚冲突保护停用时不保留麦克风 RX，已使 MQTT 成功连接；但保留 MQTT 后的新 HTTPS 心跳仍因内部 RAM 不足失败。

0.9.3 将相机 8192 字节、LCD 6144 字节的永久任务栈在支持的 S3 WiFi Core 上优先放入 PSRAM，失败回退普通任务创建；BLE 默认路径保持内部栈。LCD IRQ 上下文和音频/LCD DMA 数据显式保留内部 RAM。再使用相机驱动的运行时直接 PSRAM DMA API；驱动缺少 API 时沿用普通模式，重配失败时显式恢复普通初始化。传感器调整在重配后执行。

仅启用相机直接 DMA 的对照，启动可用内部 heap 从 116624 增至 139512 字节，**净增加 22888 字节**。这是本轮 240×240 RGB565/GC2145 的实际增量，不能把驱动配置的 32768 上限当作释放量。最终 MQTT 与心跳共存时，内部 heap 72432 字节、最小记录 18548 字节、最大块 31732 字节；双 TLS 实测通过，未修改 Arduino Core、TLS 全局分配器或证书验证设置。

实际源码主机回归：WiFi 核心整合 1041 条断言；HTTP/MQTT 释放时序专项 14 场景、161 条；可选音频采集 STD/PDM 合计 1378 条；任务栈守卫/回退 60 条；相机初始化与恢复 39 条。各报告绑定源码 SHA，均零警告。专项与整合测试存在重叠，不将这些数相加为独立覆盖总数；它们也不代替实物音频或网络验收。

### 独立 TLS 平台的实物业务检查

测试平台只监听电脑在同一热点上的私有地址，HTTPS 8443、MQTTS 8883，使用临时 CA、合成设备 UUID 和独立 `al_lab_09` 凭据区。与生产库源码唯一差异为凭据 namespace；真实 `al_dev`、保存的 WiFi 凭据和设备 UUID 保留。没有将临时 CA 加入操作系统，也没有关闭客户端证书或名称验证。

| 检查 | 结果 |
| --- | --- |
| 完整 JSON 控制 | **83/83** 检查通过：ping、设备信息、电源、I/O 清单、执行器写入、类型化读回、上报策略、raw，以及未知动作、未知端点和非法参数 |
| 实际执行次数 | 软件执行器 42、43 两次有效请求各执行一次；负例不增加回调数；读取返回 43。响应严格关联 `reply_to`，MQTT PUBACK 不作为执行证明 |
| 整板音量端点 | 默认 `args.value` 的 U16 50 返回成功且串口记录 `I2S volume: ESP_OK`；65536 超出 U16，返回 1004。未据此宣称扬声器实际有声 |
| 令牌续期 | 测试 token 寿命 90 秒，约 81.8 秒取得新 token、重新 CONNECT/SUBACK；续期后的控制/读回 **33/33** 通过 |
| WiFi 断开后恢复 | 调用 SDK disconnect，随后恢复 MQTT；控制/读回 **33/33** 通过，无需重新配网 |
| 开发板重启 | 保存的测试密钥直接认证，无新的 claim/bind；控制/读回 **33/33** 通过 |
| 测试服务重启 | 整个本地 HTTPS/MQTT 服务重启，旧 RAM token 被拒绝，开发板取得新 token 后恢复；服务进程启动后约 15.3 秒完成 MQTT CONNECT，控制/读回 **33/33** 通过。此项不是只重启 broker 的单独测试 |
| 相机与 LCD | 重启后的连续观测获得至少 129 张 240×240 JPEG、零采集错误、直接 PSRAM DMA 开启；LCD 仍 `ready=1/error=ESP_OK`，检查的是本地 JPEG 采集/解码/驱动，不是 WiFi 图片上传 |

业务报告位于 `.build/wifi-platform-lab/online-full-check.json`、`online-volume-check.json`、`after-token-renewal.json`、`after-wifi-reconnect.json`、`after-device-reboot.json` 和 `after-service-restart.json`。各轮 33 条是重复验证同组控制能力，不作独立功能数相加。一次额外探针误将描述量程 100 当作协议自动校验，101 返回协议接受、应用回调忽略；量程只是描述信息。原探针记录保留在 `volume-range-probe.json`，最终线格式负例使用超出 U16 的 65536。

### 客户端 TLS 正反例

在相同服务器/叶证书/网络下，对 POST 和 GET 分别检查正确 CA+名称、错误名称、错误 CA，最终 **6/6** 通过。正例返回 HTTP 400/501，仅证明已通过 TLS 到达 HTTP，不表示业务接口成功。错误名称返回证书校验错误 `0x2700`；错误 CA 的签名验证失败返回 `0x3000`。服务器没有 SNI 拒绝回调，普通 TCP/DNS、超时和分配失败不能算通过。

此 SDK 在握手拒绝时可能记录 `verify_flags=0`；检查器要求正例先通过，并匹配明确的客户端握手阶段和证书错误码，不能只判断“连接失败”。最终日志 `tls-client-verification-final.log` 为 `stop=0/wifi=0/restore=0/pass=6/fail=0/skip=0/partial=0`。旧的 flags-only 检查器得到 2/6，原日志和判据保留；修正判据的 47 条边界回归与真实 SDK 语法检查通过。

另一次临时固件最初显示 `Agent init failed/ESP_ERR_INVALID_ARG`，原因是测试 CA bundle 生成器使用旧二进制格式。改为 SDK 偏移表格式并四字节对齐后，初始化和上述业务检查通过；证书/私钥未重新生成，生产 CA 配置不受影响。测试结束调用 end 并清理 `al_lab_09`，串口确认 `test_namespace_erased=ESP_OK`。

WiFi 与原项目相同，提供配网、激活、HTTPS 心跳和 MQTT JSON 控制；VOICE/AUDIO/TTS/IMAGE 及固件数据传输仍使用 BLE。RoRoLee 的图片验收保留此前用户确认，未扩大为 WiFi 图片、手机 TTS 或 App OTA 验收。本板麦克风数据异常、SD GPIO35 与 OPI PSRAM 的冲突也仍保留。

### 最终正常固件

COM7 已恢复 **0.9.3 正常 WiFi AiotBasicAgent**，沿用原平台配置，不包含临时 CA、合成 UUID、观测钩子或测试端点。分段上传校验成功，原绑定和热点凭据保留。Core 3.3.12 完整 WiFi 构建为 **1531583 字节 Flash / 99536 字节静态 RAM**，零警告；固件 SHA-256 为 `0eb92e1f008d0a0956705e06119b9ef9720f38c1c9e9eec3225eb7c1078c5525`，命令与全部运行源码/草图 SHA 见 `build-normal-wifi-upload.json`。

正常固件启动后 Audio/LCD/Camera/Agent 均 `ESP_OK`，恢复真实 UUID 与 IP、`CONNECTED`，LCD 仍正常。无 PSRAM C3 BasicBLE 对当前库源码的编译为 **1516785 / 68832 字节**、零警告，未连接 C3 实物；S3 BLE 完整示例另完成编译 **1531799 / 99536 字节**、零警告，但未用该 BLE 构建重做手机媒体验收。后续正常 WiFi 观测日志为 `.build/hardware-aiot/wifi-online/normal-wifi-final-boot.log`，汇总为 `results-final.json`。当前板运行 WiFi 模式；使用 RoRoLee 的图片/音频时需要切回 BLE 模式。
