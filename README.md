# deotaland-arduino

Arduino Agent Link 通信库，当前版本 **0.9.3**。通过 BLE 上报传感器、接收控制命令、发送提示词和相机照片，并支持语音、音频回复和 BLE OTA。内置 NimBLE-Arduino 2.5.1 源码，默认启用两个 BLE CoC 通道。内置后端面向 ESP32；STM32 需使用自定义传输适配器。

## 快速开始

1. 在 Arduino IDE 中通过“Sketch → Include Library → Add .ZIP Library”安装，或将本目录放入 Sketchbook 的 `libraries`。
2. 安装官方 ESP32 Arduino Core，选择实际板型、端口、Flash 和分区。验证基线为 Arduino-ESP32 3.3.12，其 SDK 基于 ESP-IDF 5.5.5；S3 使用 `PartitionScheme=min_spiffs`，真实相机另需匹配硬件的 PSRAM 设置。
3. 从“文件 → 示例 → deotaland-arduino”打开下面的示例，上传后以 **115200** 查看串口。

无需另装 NimBLE-Arduino、修改 Core 或重编译 SDK。BLE 需要有 BLE 控制器的 ESP32 及兼容 Agent Link 的 App。详细安装、USB 设置和排错见 [中文入门指南](docs/BEGINNER-GUIDE.zh-CN.md)。

## 选择示例

| 目标 | 示例 |
| --- | --- |
| ESP32-S3 AIOT-Basic 整板语音、相机、屏幕、SD 录音和 OTA | [AiotBasicAgent](examples/AiotBasicAgent/README.md) |
| 验证 BLE 连接 | [BasicBLE](examples/BasicBLE/README.md) |
| 上报读数、接收开关控制 | [CachedSensor](examples/CachedSensor/README.md) / [SensorActuator](examples/SensorActuator/README.md) |
| 发送文字提示词 | [SerialPrompt](examples/SerialPrompt/README.md) |
| 真实相机拍照并上传给 App | [CameraSnapshot](examples/CameraSnapshot/README.md)，先读[图片上传指南](docs/IMAGE-UPLOAD.zh-CN.md) |
| 无相机验证 JPEG 上传 | [StoredJpegUpload](examples/StoredJpegUpload/README.md) |
| 连续测试图片、进度和取消 | [ImageSequence](examples/ImageSequence/README.md) |
| 查询 CoC 能力与通道状态 | [CoCDiagnostics](examples/CoCDiagnostics/README.md) |
| 显式启用 BLE OTA | [CoCOtaUpdate](examples/CoCOtaUpdate/README.md) |

读数优先使用 `updateBool / updateInt32 / updateUInt16 / updateFloat / updateText`，库负责类型检查、缓存和后续发送。音频、图片、播放及移植等全部 **19 个示例**见 [示例索引](examples/README.zh-CN.md)。

## 使用要点

- 每轮 `loop()` 调用 `Deotaland.poll()`，回调快速返回；检查启动和发送 API 的返回值。
- BLE 订阅事件特征 `0xFFC4` 后才是 `READY`；AUDIO、IMAGE、TTS 和 OTA 还需 App 打开对应 CoC 通道。两个通道是同时连接的资源上限，并非两个固定业务类型。
- `DeotalandImageUpload` 分块发送 JPEG/RGB565，处理拥塞重试、超时及断线。`CameraSnapshot` 提供 GC2145 相机采集、JPEG 编码和 `camera0` 拍照端点；[CoC 说明](docs/BLE-COC.zh-CN.md)介绍配置及协议。
- `ESP_OK` 表示本地接受，不保证对端收到；缓存读数在离线或拥塞时只保留最新值。
- AiotBasicAgent 接入 AIOT-Basic 的真实麦克风、功放、屏幕、相机、WS2812 和 SD 卡；CameraSnapshot 使用真实相机。其余示例中的模拟数据或测试音需按硬件替换，具体条件见各目录 README。

当前库仅提供 BLE 内置连接：Wi-Fi 联网、配网、云端激活和 MQTT 控制代码及四个相关示例已移除。原 Wi-Fi 配置字段和传输枚举也已移除，旧草图应改用 BLE。整板应用详见 [AiotBasicAgent](examples/AiotBasicAgent/README.md)。

## 文档

[入门指南](docs/BEGINNER-GUIDE.zh-CN.md) · [API 与进阶参考](docs/REFERENCE.zh-CN.md) · [原项目对照与完善记录](docs/UPSTREAM-COMPARISON.zh-CN.md) · [AIOT-Basic 实测与已知问题](docs/HARDWARE-VALIDATION.zh-CN.md) · [常见问题](docs/BEGINNER-GUIDE.zh-CN.md#troubleshooting)

第三方源码及许可证见 [vendor 说明](src/vendor/nimble-arduino/README.deotaland.md)。许可证见 [LICENSE](LICENSE)。编译通过不代表真实无线、音视频或 OTA 已完成硬件验证。

## 生成安装包

维护者可在源码仓库根目录运行以下命令，需要 PowerShell 和 Python：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-library.ps1 -OutputPath ..\dist\deotaland-arduino-0.9.3.zip
python .\tools\check-library-package.py ..\dist\deotaland-arduino-0.9.3.zip
```

安装包包含库源码、示例、使用文档和许可证。`tools` 是仓库维护工具，不随安装 ZIP 分发，安装和使用本库无需运行这些脚本。
