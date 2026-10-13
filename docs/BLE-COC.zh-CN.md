# BLE CoC 与 NimBLE 配置

0.8.0 在 Arduino 构建中随库编译固定版本的 **NimBLE-Arduino 2.5.1** 主机源码，默认启用 **两个 L2CAP Connection Oriented Channel（CoC）**。安装官方 ESP32 Arduino Core 和本库即可编译；无需另装 NimBLE-Arduino、修改 Core、重编译 SDK 或向 Core 提交 PR。官方 Arduino-ESP32 3.3.12 是当前已验证的编译基线。

## 安装和使用

1. 在 Arduino 开发板管理器安装 Espressif 的 `esp32`，再安装本库 ZIP。
2. 打开 [BasicBLE](../examples/BasicBLE/BasicBLE.ino) 检查控制连接。
3. 用 [StoredJpegUpload](../examples/StoredJpegUpload/StoredJpegUpload.ino) 测试真实 JPEG 上传，无需相机。
4. App 订阅设备事件 `0xFFC4`，并打开 IMAGE CoC `PSM 0x0082`。READY 表示控制就绪，不能代替图片通道就绪。
5. 接入相机时使用 [CameraSnapshot](../examples/CameraSnapshot/CameraSnapshot.ino)，核对引脚、PSRAM 类型、Flash 容量和应用分区。

一个固件由本库管理 BLE 控制器和主机生命周期。草图包含 `Deotaland.h` 即可；不要同时初始化 `BLEDevice`、另一套 `NimBLEDevice` 或其他 BLE 主机。单独安装 NimBLE-Arduino 不是本库的依赖；vendored 主机属于库的私有实现。

ESP32-S2 等没有 BLE 控制器的芯片仍不支持内置 BLE；STM32 使用自定义传输。其 Arduino 编译会跳过 vendored BLE 源码，定义 `AGENT_LINK_DISABLE_BLE` 的编译同样跳过该主机。

## 协议与通道上限

| 业务 | 传输 |
| --- | --- |
| 命令、设备事件 | GATT `0xFFC1` / `0xFFC4` |
| VOICE 上行 | GATT `0xFFA1` |
| AUDIO 上行、TTS 下行 | CoC `PSM 0x0081` |
| IMAGE 上行 | CoC `PSM 0x0082` |
| 显式启用的 OTA | 与 TTS 共用 CoC `0x0081`，命令/响应为 `0xFFB1` |

两个通道是**同时连接的资源上限**，可连接 IMAGE 与 AUDIO/TTS 两种 PSM。OTA 与 TTS 使用同一通道，升级时应停止音频等大流量业务。CoC 使用信用流控；App 暂停接收时发送队列会积压，应持续 `poll()` 并按 API 返回值重试。

图片载荷保持与 Agent_link-main 一致：连续的 JPEG 或大端 RGB565 字节，不添加应用级分片封装。尺寸、格式及开始/结束通过事件 `0x54` / `0x55` 发送。`DeotalandImageUpload::Completed` 表示本地接受完成，不是 App 收图 ACK。本库没有独立的图片 HTTP 上传接口。

## 库内统一配置

[DeotalandNimbleConfig.h](../src/vendor/nimble-arduino/DeotalandNimbleConfig.h) 在主机所有编译单元中加载同一配置，覆盖 SDK 的主机默认值：

| 配置 | 值 |
| --- | ---: |
| BLE 连接数 | 1 |
| CoC 通道数 | 2 |
| CoC MPS | 512 字节 |
| MSYS1 | 24 块 × 256 字节 |
| MSYS2 | 32 块 × 576 字节 |
| 主机任务栈 | 6144 字节 |
| ATT 首选 MTU | 255 字节，仍需对端协商 |

MSYS2 块为 512 字节数据及 mbuf/协议头留有余量。实际内存还包含主机状态、控制器、任务栈及按需预分配的业务队列；这些数值不是整库总 RAM 占用。CoC 增加代码和 RAM，相机和并行音频也会竞争内部 RAM。大图应分块上传，相机帧和 JPEG 缓冲须按[图片指南](IMAGE-UPLOAD.zh-CN.md)保持有效。

调整配置需要重新编译本库主机源码。仅在 `.ino` 添加宏不能统一配置其他编译单元。修改内部配置时应一起评估通道数、mbuf 池、接收预算和同时运行的业务。固定提交、许可证和本地修改见 [vendor 说明](../src/vendor/nimble-arduino/README.deotaland.md)。

## 自定义宿主

自行维护其他宿主构建时，可对所有库编译单元统一定义 `DEOTALAND_USE_IDF_NIMBLE=1`，改用宿主的 ESP-IDF NimBLE，并跳过库内主机源码。这需要自行提供匹配的 SDK、CoC 配置及构建集成；该宏不能启用已编译 SDK 中缺失的 CoC。普通 Arduino 使用保持默认配置即可。

## 实机收图与验证边界

串口确认 `begin` 成功、IMAGE 支持和相机初始化成功。App 建立当前连接的 IMAGE CoC 后，通过串口、按钮或 `camera0=true` 拍照；App 根据 `0x54` 接收图片，收到 `0x55` 后解码并检查结束状态。断连后的旧图片不能继续发送到新会话。[CoCDiagnostics](../examples/CoCDiagnostics/CoCDiagnostics.ino) 可检查状态和队列，但不能代替 App 的解码和保存结果。

ESP32-S3 AIOT-Basic 已与 Android RoRoLee 3.4.2 完成一次 `camera0` 拍照、JPEG 接收与画面描述，见[硬件记录](HARDWARE-VALIDATION.zh-CN.md)。持续上传、RGB565、其他接收端与长期稳定性仍须分别验证，编译通过不能代替收图结果。

## 官方依据

- [NimBLE-Arduino 2.5.1](https://github.com/h2zero/NimBLE-Arduino/releases/tag/2.5.1)：本库固定的上游发布。
- [上游 nimconfig.h](https://github.com/h2zero/NimBLE-Arduino/blob/2.5.1/src/nimconfig.h)：可从源码配置 CoC，上游默认值为 0。
- [Arduino-ESP32 FAQ](https://docs.espressif.com/projects/arduino-esp32/en/latest/faq.html)：预编译 SDK 配置不能由草图宏补齐；本库通过随库编译主机源码提供 CoC。
