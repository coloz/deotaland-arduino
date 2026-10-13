# 中文入门指南

适用库版本：**0.9.3**；核对日期：2026-10-13。内容对应当前源码和示例。文中的 Core 版本是已验证的编译基线，不表示只有该版本可用，也不表示它是当前最新 Core。

先看到 READY 和一条模拟读数，再接入真实传感器。所有内置连接与媒体示例均使用 BLE。

## 1. 准备与安装

准备可传数据的 USB 线和 Arduino IDE、有 BLE 控制器的 ESP32 及兼容 Agent Link 的 App。本文的编译基线是 ESP32-S3 + Arduino-ESP32 3.3.12，随 Core 提供的 SDK 为 ESP-IDF 5.5.5。库的便携核心还能用于 STM32 自定义传输，但那需要自行实现桥接，不适合作为第一课。

在开发板管理器安装 Espressif 的 `esp32` Core，再安装本库。0.9.3 内置固定版本的 NimBLE-Arduino 源码，默认启用两个 CoC 通道，不需要另装 NimBLE-Arduino、修改 Core 配置或重编译 SDK。一个固件由本库管理 BLE 栈；不要再初始化 BLEDevice 或另一个 NimBLEDevice 栈。

用“Sketch → Include Library → Add .ZIP Library”选择安装包。手动安装时目录应是 `Sketchbook/libraries/deotaland-arduino/src/Deotaland.h`，而不是多套一层同名目录。重开 IDE，从 File → Examples 找到本库。[官方安装说明](https://support.arduino.cc/hc/en-us/articles/5145457742236-Install-libraries-in-the-Arduino-IDE)

安装后确认 `library.properties` 中的 `version=0.9.3`。若找不到 `DeotalandImageUpload`、`updateFloat`，查看 IDE 详细编译输出采用的库路径，排除旧副本。`config.firmware_rev` 是你自己的设备固件版本，不必跟随库版本填写。

选择与你的实际芯片一致的板型、端口、Flash 大小和分区。完整 BLE 与媒体固件需要足够的应用分区；本次 S3 编译使用 `min_spiffs`，应用槽 1,966,080 字节并保留双槽。按实际 Flash 选择，不能为了通过编译虚填容量。[Espressif 工具菜单](https://docs.espressif.com/projects/arduino-esp32/en/latest/guides/tools_menu.html)

若用 S3 的原生 USB 口看 `Serial`，按板卡配置启用 `USB CDC On Boot`；若用 CH340/CP210x 等转串口芯片，应选对应端口，不能套用原生 USB 设置。打开监视器选 115200。原生 USB 首次烧录后可能需要复位并重新选择端口。[Espressif USB CDC 指南](https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/cdc_dfu_flash.html)

## 2. 第一课：BasicBLE

上传 [BasicBLE](../examples/BasicBLE/BasicBLE.ino)。无需连接其他硬件；在 App 内搜索 `Deotaland-ESP32` 并连接。串口会周期性显示状态，发送 `?` 也可主动查询。

| 看到的状态 | 含义 | 下一步 |
| --- | --- | --- |
| begin 报错 | 驱动尚未启动成功 | 查错误名和排查表，不继续上报 |
| DISCONNECTED | 尚未建立当前可用会话 | 在 App 内扫描、连接 |
| CONNECTED | 链路建立，但事件通道未就绪 | App 需订阅事件特征 0xFFC4 |
| READY | 控制/事件通道就绪 | 继续传感器练习 |

期望日志形如 `连接状态: READY`。UUID 每台设备不同；首次持久化后才会可用。模拟电量固定为 80%，并非每次采样都发包：库会去重，重连后会重新同步。普通扫描工具订阅只够做 GATT 调试，不会自动完成 Agent 业务交互。

## 3. 第二课：CachedSensor

上传 [CachedSensor](../examples/CachedSensor/CachedSensor.ino)，连接 `Deotaland-Sensor`。程序每秒生成 20.0～20.9°C 的循环模拟值，每 5 秒打印一次日志；App 中的端点 ID 是 `temp0`。

一次传感器接入包含四步：

1. 全局创建 `agent_link_io_desc_t`，填写短且唯一的 id、IN 方向、语义 kind、数据类型和单位。
2. `setup()` 中先 `registerIo`，检查返回值，再 `begin`。
3. `loop()` 每轮调用 `poll()`，用 `millis()` 决定何时采样。
4. 调用 `updateFloat("temp0", 实际温度)`，库会复制、缓存并稍后发送。

`updateFloat` 要配 F32，整数用 `updateInt32` 或 `updateUInt16`，开关用 `updateBool`，文本用 `updateText`；完整类型对应见 [API 概览](REFERENCE.zh-CN.md#api-overview)。不要通过强制转换去“消除”类型不匹配；先检查端点声明。`double` 应确认精度与范围后转换为 `float`，数组和其他类型仍用原始接口。

`updateXXX` 返回 `ESP_OK` 表示已接受到本地缓存，不代表 App 已收到。文本按 UTF-8 字节计数，默认最多 125 字节，不含结尾 NUL；`updateText(id, "")` 可清空文本，`nullptr` 会被拒绝。

先断开 App，等待数秒后重连：应同步最新温度，不会把离线期间的每个旧样本逐个补发。要保存每次采样或报警事件，需要应用自己的记录/确认机制；最新值缓存不替代日志。

接真实硬件时，只需先替换 `readTemperatureC()` 并初始化对应驱动。采样失败时不要上报假的 0；检测驱动错误，保留上次有效读数，并通过独立状态端点说明故障。NaN/Inf 会被库拒绝，缓存不被污染。传感器慢速转换应拆成“发起测量”和“读取结果”，让 loop 继续运行。

## 4. 第三课：SensorActuator

上传 [SensorActuator](../examples/SensorActuator/SensorActuator.ino)，连接 `Deotaland-IO`。将 `led0` 设为 true/false，串口输出 ON/OFF，`enabled0` 回传软件状态，`uptime0` 每秒更新运行时间。

默认不操作任何 GPIO。确认普通 LED 的引脚与有效电平后，修改 `kLedPin` 和 `kLedActiveLow`；可编程 RGB 地址灯须使用自己的驱动。`led0` 在此例代表逻辑开关，不代表已测得真实灯亮。应用若需要证明硬件动作完成，应增加实际状态采样/故障读数。

回调里只接收目标值；`loop()` 应用它。命令用“设为开启”，不要用“切换一次”：重发相同命令仍会得到相同状态。BOOL 参数是一个 0/1 字节，STR 是带长度的 UTF-8，不能未经复制和补 NUL 就当作 C 字符串使用。

描述符、描述字符串和 ctx 需要持续有效；用全局/static 对象。`begin` 会复制配置字符串及输出回调表，但不会复制 I/O 描述符，也不会延长 ctx 或自定义传输对象的寿命。

## 5. 第四课：SerialPrompt

上传 [SerialPrompt](../examples/SerialPrompt/SerialPrompt.ino)，在 App 连接 `Deotaland-Prompt` 后，串口发送 `p`。程序发送“你好”，可附加换行；`?` 显示连接状态和允许的提示词字节数。

提示词按 UTF-8 字节计数。默认 BLE MTU 下通常最多 14 字节，中文通常每字 3 字节。`maxPromptBytes()` 反映当前协商容量；长句需要先协商更大 MTU 或缩短文本，不能把一个提示词拆成多次 pushPrompt 假定平台会自动合并。

示例每 100ms 尝试一次，只在拥塞时重试；断线或等待超过 5 秒取消。它不会在 `while` 中反复发送，也不会把旧提示词留到新连接。发送成功意味着本地接受，实际 Agent 回复需要 App 和平台业务链路正常。

## 6. 语音和图片什么时候开始

语音/图片示例放在控制链路练习之后。VOICE 使用 GATT，但测试程序产生的是合成音；AUDIO、IMAGE、TTS 和 OTA 使用内置 CoC 栈，仍要求 App 打开相应通道。VoicePTT、RealtimeASR、ImageStream、BufferedPlayback 会在启动后检查所需能力，每 10 秒或输入 `?` 可重新查看诊断。库不会替你选择 I2S、摄像头或扬声器引脚。每个进阶示例的替换点见 [索引](../examples/README.zh-CN.md)。

先用 [StoredJpegUpload](../examples/StoredJpegUpload/StoredJpegUpload.ino) 验证无需相机的真实 JPEG 收图；[CoCDiagnostics](../examples/CoCDiagnostics/CoCDiagnostics.ino) 查看能力和通道状态。真实相机使用 [CameraSnapshot](../examples/CameraSnapshot/CameraSnapshot.ino)：核对引脚并启用 PSRAM。连接 App 后用串口 `c`、实体按钮或 App 的 `camera0=true` 拍照上传；后台负责采集/编码，主循环分块发送，完整步骤见[图片上传指南](IMAGE-UPLOAD.zh-CN.md)。[ImageSequence](../examples/ImageSequence/ImageSequence.ino) 演示有限连续图片、进度和取消，ImageStream 保留为 2×2 测试图练习。配置与协议见 [CoC 说明](BLE-COC.zh-CN.md)。

## 7. 不容易出错的 loop

持续 `poll()`，用无符号时间差 `uint32_t(now - lastTime)` 安排采样。失败后的重试必须留到后续 loop，不能阻塞等待 READY；大量串口打印也会影响通信。

所有业务接口在同一 loop 线程调用，不在中断里调用。回调不能执行生命周期操作；需要停止或重启时，回调只设置标志，loop 看到标志后调用 `end()`，等 `canBegin()` 为 true 再 `begin()`。端点只注册一次：end 保留它们，reset 才清除。

[ResponsiveLoop](../examples/ResponsiveLoop/ResponsiveLoop.ino) 演示限时执行动作和低压力发送：拥塞重试至少间隔 100ms，成功或永久错误后等待下一个上报周期。`isRetryable()` 仅识别 NO_MEM/TIMEOUT，不代表初始化内存不足也应无限重试。

<a id="troubleshooting"></a>
## 8. 常见问题与当前边界

| 现象 | 先检查与处理 |
| --- | --- |
| 找不到头文件或新方法 | 检查库目录结构、实际编译路径及 0.9.3 版本 |
| Sketch too big | 按真实 Flash 容量选择足够大的应用分区，参考第 1 节 |
| 上传后串口空白 | 检查板卡对应的 USB/CDC、端口、115200；打开串口后复位或等待周期诊断 |
| begin / streamStatus 返回 NOT_SUPPORTED | 检查芯片、Core、所选传输与 CoC 支持；不能靠重试补齐能力 |
| BLE 一直 CONNECTED | App 需订阅事件特征 0xFFC4；READY 后媒体仍可能等待各自通道 |
| updateXXX 成功但没有读数 | 检查端点 ID/方向/类型、持续 poll、App 的上报策略和当前包容量 |
| 提示词/读数过长 | 按 UTF-8 字节检查 maxPromptBytes 与 MTU；读数还占用 ID 字节数加 2 字节 |
| 相机图片无法上传 | 看 CameraSnapshot 诊断，确认 PSRAM/接线、IMAGE 能力、App 的 CoC 0x0082 通道及上传返回值 |
| 没声音或识别为噪声 | VoicePTT/RealtimeASR 默认是合成测试音；BufferedPlayback 无扬声器驱动，需按真实硬件接入 |
| 重连后旧 stream 句柄报错 / duplicate id | 重新打开当前会话的流；I/O 端点不要每次连接重复注册，变更配置时才 reset |

先检查本次 API 返回值，再看 `state()/isReady()` 及 `status()`。`stats().last_error` 是最近一次记录的历史错误，成功不会自动清零；它不代表当前连接必然失败。不要在日志中输出完整私密提示词。

ESP32-S3 AIOT-Basic 与 RoRoLee Android 3.4.2 已完成一次 BLE 拍照请求、JPEG 收图和画面描述验收，见[硬件实测记录](HARDWARE-VALIDATION.zh-CN.md)。该结果不代替其他板卡、RAW 或持续图片上传的验收。API 的预算、内存、媒体流、OTA 和自定义传输约定见 [进阶参考](REFERENCE.zh-CN.md)。
