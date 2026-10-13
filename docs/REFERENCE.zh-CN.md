# API 与进阶参考

适用库版本：**0.9.3**；核对日期：2026-10-13。本文对应当前源码：包含随库编译的 NimBLE CoC、相机拍照与分块图片上传、类型化读数、BLE 连接状态，以及流、播放、OTA 和自定义传输约定。

**升级后必须在 loop() 中调用 Deotaland.poll()。** 硬件回调在这个调用线程执行。通用库不绑定屏幕、麦克风、相机或传感器引脚；应用通过回调和 I/O 端点接入硬件。CameraSnapshot 示例已提供真实 ESP32 相机驱动接入，默认原版 GC2145 引脚。

## 安装与入门

安装和逐步练习见 [中文指南](BEGINNER-GUIDE.zh-CN.md)，完整程序见 [示例索引](../examples/README.zh-CN.md)。从 BasicBLE 开始。常见错误与处理见 [入门排查表](BEGINNER-GUIDE.zh-CN.md#troubleshooting)。

BLE 物理连接先进入 CONNECTED，订阅事件通知 0xFFC4 后才 READY。连接状态回调由 poll 派发；断连会立即使 isReady 和旧会话失效，避免快速重连时发送上一连接的数据。所有应用都应检查 begin 和发送 API 的返回值；推荐从 BasicBLE 完整示例开始。READY 只代表控制通道，AUDIO/IMAGE 还需要对应 CoC 通道就绪。

## 平台边界

| 层 | 依赖和支持范围 |
| --- | --- |
| 协议、清单、I/O、流会话、PCM/ADPCM | C++11 标准库，不依赖 ESP-IDF、Arduino 引脚或 RTOS |
| 内置 BLE | Arduino-ESP32 3.x、有 BLE 控制器的芯片；随库编译 NimBLE-Arduino，CoC 默认 2；无 BLE 时返回不支持 |
| 自定义传输 | AGENT_TRANSPORT_CUSTOM；STM32 等具有标准库和足够 RAM 的 32 位 Arduino 硬件 |
| OTA | 显式启用的 ESP32 后端，需要 CoC 和 OTA 分区布局 |

元数据声明 ESP32 和 STM32。其他平台可复用核心并提供适配器，但没有声称真机验证。AVR/UNO 的内存和标准库条件不适合此实现；修改 architectures 不能替代芯片 BLE 驱动。

当前编译基线为 Arduino-ESP32 3.3.12，其预编译 SDK 为 ESP-IDF 5.5.5；Core 版本与 ESP-IDF 版本是两个不同的编号。

## 非阻塞约定

- `poll(max_events, max_us)` 同时限制 RX 工作片数和协作式时间预算，默认 `poll(4, 2000)`。`poll(n, 0)` 取消时间限制，`poll(0, ...)` 不做工作。九类服务轮转，预算耗尽后下次从剩余位置继续。预算在工作片之间检查，无法抢占用户回调或平台调用。
- PCM 每片最多 512 字节输入，ADPCM 每片最多一个 256 字节块（输出至多 1010 字节 PCM）；清单每步最多生成 256 字节或尝试一个分片，支持在字段和转义字符中途暂停。拥塞后保留进度。
- RX 固定容量：默认 16 槽，每个最大 1024 字节；媒体最多占 12 槽，为控制和状态各预留两个槽。入队复制数据。stats() 提供拒收数、队列峰值、发送重试、背压、清单进度，以及 `poll_last_us / poll_max_us / poll_budget_exhaustions`。
- 发送 ESP_OK 表示已入队，不保证对端收到。ESP_ERR_NO_MEM / ESP_ERR_TIMEOUT 时保留原数据，在后续循环重试。文件和图片不能跳过失败的数据块。
- BLE 重试及驱动收尾由后台任务处理。自定义 send_ctrl / send_stream / poll 也必须快速返回，不调用阻塞连接或 flush。
- begin / end 涉及平台驱动，不承诺硬实时耗时。end 发起后台收尾，先等待 `canBegin()` 再次为 true，再调用 begin。
- 用户回调仅由 poll 串行派发，不能等待、递归调用 poll 或调用生命周期方法。持续播放、显示及电机动作由应用状态机执行。大量串口日志本身也可能阻塞。
- 公共业务 API 在同一个 loop 线程调用，不从 ISR 调用。ESP32 RX 入口支持 BLE 线程入队；非 ESP32 自定义后端应在其 poll 中提交接收事件。

常规 BLE 事件、读数、命令解析/应答和清单热路径不逐包分配堆内存；BLE 控制队列固定 16 槽，为应答、流控和结束等重要消息预留两个槽，保持 FIFO 顺序。VOICE/AUDIO/IMAGE 分别使用 16/16/32 KiB 媒体环和各 6 KiB 工作任务栈，可在 begin 后通过 `prepareStreams(1u << AGENT_STREAM_VOICE)` 提前创建；关闭流后资源复用，end 后台收尾时释放。未预热仍在首次打开时创建。建议每轮 writeStream 不超过 512 字节，NO_MEM/TIMEOUT 原块稍后重试；INVALID_SIZE 时检查块容量及总长度，只有块容量超限才拆小；VOICE 必须按 PCM16 偶数字节写入。初始配置、云端 JSON、OTA 和底层无线栈仍可能分配内存。

清单默认上限 8192 字节，溢出不发送残缺 JSON，stats().last_error 为 INVALID_SIZE。可用 AGENT_LINK_MANIFEST_BYTES、AGENT_LINK_MANIFEST_STEP_BYTES、AGENT_LINK_RX_SLOTS / AGENT_LINK_RX_BYTES、AGENT_LINK_READING_CACHE_BYTES 等全局编译参数调整；资源和兼容性说明见性能文档。所有参数须对库的源文件统一生效，仅在 ino 中定义不能改变库的编译参数。

自定义传输可以通过全局编译参数 `AGENT_LINK_DISABLE_BLE`、`AGENT_LINK_DISABLE_OTA` 排除不用的后端及其驱动依赖。例如 Arduino CLI 加上 `--build-property "compiler.cpp.extra_flags=-DAGENT_LINK_DISABLE_BLE -DAGENT_LINK_DISABLE_OTA"`。默认启用芯片支持的 BLE 后端；不支持 BLE 的芯片会返回 NOT_SUPPORTED。

<a id="api-overview"></a>
## API 概览（0.9.3）

| 能力 | API / 状态 |
| --- | --- |
| 生命周期 | begin, poll, end, reset, canBegin, state, isReady, stats |
| 连接进度与身份 | status / on_status、deviceId、forget；SETUP / CONNECTING / PAIRING / BLOCKED / CONNECTED / READY 等显示阶段 |
| 自描述硬件 | registerIo, pushReading, updateReading, notifyManifestChanged；GetReading / SetReadingConfig |
| 类型化缓存读数 | updateBool, updateInt32, updateUInt16, updateFloat, updateText；检查已注册端点类型 |
| 事件和提示词 | pushEvent, pushPrompt, reportBattery；maxPromptBytes 查询当前单包容量 |
| 易用诊断 | stateName, errorName, errorHint, isRetryable；不进行串口输出或动态字符串分配 |
| 统一流 | prepareStreams, openStream, writeStream, closeStream, sendStream, streamStatus |
| 语音 | VOICE 经 GATT 0xFFA1；AUDIO 经 CoC PSM 0x0081 |
| 图片 | IMAGE 经独立 CoC PSM 0x0082，RGB565 / JPEG 元数据 |
| 分块图片上传 | DeotalandImageUpload：start, poll, cancel, busy, state, result, acceptedBytes；见[指南](IMAGE-UPLOAD.zh-CN.md) |
| TTS | PCM16 / IMA-ADPCM，输出 16kHz 单声道 PCM16；configurePlayback / playbackStats、真实缓冲水位、事件 0x20 流控 |
| 身份查询 | 命令 0x01 / deviceId；共用 NVS agent_link/dev_uuid，兼容迁移 al_dev/sn；自定义硬件指定 16 字节 device_id |
| OTA | prepareOta、setOtaCallback、otaStatus / otaBusy / otaSupported / otaNeedsReconnect、abortOta、markOtaValid、runningVersion；0x37 / 0x56 / 0x38；默认关闭 |

setPlaybackBufferMs(ms) 声明真实播放缓冲容量，默认 400ms，支持范围为 120～120000ms，超出会截取到边界。使用内置流控的播放缓冲至少应容纳 120ms PCM。不要声明超过实际容量的数值。

`configurePlayback` 注册快速返回的 write / queued 回调：write 返回实际接收的偶数字节数，0 表示本轮无空间，库保留未接收的 PCM 并继续背压；queued 返回应用队列及 DMA 中尚未播放的字节数。`on_audio_end` 在完整 PCM 全部交给应用后触发，物理播放可能尚未结束。`playbackActive()` 查询协议 TTS 回复是否尚未结束（含库内待排空数据），不把应用队列或 DMA 的尾音视为新回复；用于判断取消应影响哪个会话，须在 `poll` 同一线程调用。未配置时兼容原 on_audio_out 和时间估算流控，原回调必须一次接收全部数据。完整应用缓冲示例见 [BufferedPlayback](../examples/BufferedPlayback/BufferedPlayback.ino)，真实硬件整合见 [AiotBasicAgent](../examples/AiotBasicAgent/README.md)。

`updateReading(id, value, length)` 只更新固定缓存，离线和拥塞时合并为最新值，poll 稍后发送；默认最大 125 字节。PERIODIC 根据配置周期自动发送缓存值，应用仍负责采样；ON_CHANGE 精确比较完整缓存，OFF 只缓存。`pushReading` 保持即时发送接口，失败由调用方重试。超过缓存上限的值只能走 pushReading，受 MTU 限制，不能用于缓存查询或去重。重连后策略恢复 PASSTHROUGH，并重新发送缓存的最新读数。

一般传感器优先使用以下类型化接口，无需传入地址和 sizeof。先注册对应 IN 端点；这些接口会按类型检查、将标量编码为小端并复制到缓存，离线也能更新。`ESP_OK` 表示缓存接受，不代表已经发送或收到对端确认。

| 方法 | 端点 value 类型 | 输入与边界 |
| --- | --- | --- |
| updateBool(id, value) | AGENT_VAL_BOOL | C++ bool，编码为一个 0/1 字节 |
| updateInt32(id, value) | AGENT_VAL_I32 | int32_t，四字节有符号整数 |
| updateUInt16(id, value) | AGENT_VAL_U16 | uint16_t，两字节无符号整数 |
| updateFloat(id, value) | AGENT_VAL_F32 | float，拒绝 NaN/Inf |
| updateText(id, utf8) | AGENT_VAL_STR | 以 NUL 结尾的输入字符串；按 UTF-8 字节缓存，不保存 NUL；空串可清空值，nullptr 无效 |

类型不匹配返回 `ESP_ERR_INVALID_ARG`，即使 I32/F32 等类型宽度相同也拒绝。原始 `void*` 接口只能依据描述符校验字节宽度和值，无法推断调用者原本的 C++ 类型。原始读数和执行器输入同样拒绝非法 BOOL 及非有限 F32/VEC2/VEC3；注册时拒绝非有限或倒置量程。量程和 args_schema 是描述信息，不会自动截断读数或执行完整 JSON Schema 校验。无效更新不会覆盖上次有效缓存。

`maxPromptBytes()` 返回当前单条提示词可用的 UTF-8 字节数，未 READY 时为 0；默认 BLE ATT MTU 23 时通常为 14。读数还需扣除 ID 的字节数加 2 字节，不能将提示词容量直接当成读数容量。缓存上限与链路 MTU 是两个限制：缓存成功后若暂时无法放入包，后台会记录 `stats().last_error` 并保留样本，待容量或连接恢复后重试。

0.8.1 对 `AGENT_VAL_STR` 统一验证 UTF-8：读数和执行器参数中的内嵌 NUL、非法或截断的编码返回参数错误。空串可用；`AGENT_VAL_BLOB` 仍可承载任意二进制。

`errorName()`/`errorHint()` 解释 API 返回值；`stateName()` 解释 `agent_state_t`，不要传入 `status().phase`。`isRetryable()` 仅将 NO_MEM/TIMEOUT 视为发送拥塞候选，应在后续 loop 限频重试；初始化或资源预热时 NO_MEM 可能需要减少内存占用。`stats().last_error` 是最近记录的历史错误，成功不会自动清零，也不覆盖每个公共 API 的参数错误。动态注册端点会自动使清单更新；原地修改已注册描述符后才需显式调用 `notifyManifestChanged()`。

内置传输仅支持 BLE。VIDEO、FILE 和 selectAgent 未实现，自定义后端可支持通用流类型。

0.8.0 在普通 Arduino 构建中随库编译 NimBLE-Arduino 2.5.1，统一启用两个 CoC 通道。AUDIO、IMAGE、TTS 和显式启用的 OTA 使用这套栈，官方 Core 的预编译 CoC=0 不再限制这些业务。两个通道是同时连接上限；OTA 与 TTS 共用 PSM 0x0081。App 仍须打开相应通道。配置与协议见 [CoC 说明](BLE-COC.zh-CN.md)。GATT 控制和 VOICE 不依赖 CoC。

内置 BLE 可在 begin 成功后、连接 App 之前用 `streamStatus(kind, &status)` 检查流类型支持：返回 NOT_SUPPORTED 表示当前构建缺少能力，返回 ESP_OK 只表示查询成功；流状态 UNAVAILABLE 可能只是对端未连接，不能据此判断 Core 缺功能。`prepareStreams()` 则会实际预分配缓存和任务。0.6.1 的 ImageStream、RealtimeASR、BufferedPlayback 在缺少相应 CoC 能力时停止相关示例任务，VoicePTT 独立检查 VOICE 准备结果与 TTS/CoC 限制；四例每 10 秒及串口 `?` 可重看中文诊断。通过 AUDIO 查询推断 TTS 的 CoC 支持仅适用于内置 BLE，不是 CUSTOM 后端的通用 TTS 能力检查。

统一流调用顺序是 openStream(kind, opts, &handle) → writeStream(handle, bytes, count) → closeStream(handle)。同类型只有一个流，重复 open 返回当前句柄；断连或停止使流关闭。新建会话不会复用旧句柄，旧句柄不能操作新会话。IMAGE 必须给出非零宽、高；JPEG 还需要 total_bytes，RAW 的零长度自动推导为宽×高×2。close 失败时会话仍保留：暂时拥塞可重试，永久错误应停止任务，等待重连或重启后端再新建流。句柄仅限当前会话，关闭后不要继续持有并操作它。

sendStream 适合整块一次入队的小对象，已有同类型流时返回状态错误。如果数据已接受而 END 暂时拥塞，返回成功并由 poll 自动重试 END，调用方不要重发数据。在排空完成前，该类型暂时不能再次 open。`streamStatus` 可查询 OPEN / DRAINING / COMPLETED / FAILED、排队字节和拒收计数；COMPLETED 表示数据交给本地栈、END 入本地队列，不代表对端 ACK。VOICE 没有线上的 END 标记，close(false) 只记录本地失败，仍排空已接收数据。分块传输见 ImageStream 示例。

`DeotalandImageUpload` 适合大于排队缓存的完整照片。`start(opts, data, bytes, now_ms, timeout_ms=30000)` 验证元数据并借用缓冲区，不复制整图；在 `loop()` 中调用 `poll(millis())`，每次最多执行一个流操作，写入块不超过 512 字节。每块入队失败时保留偏移重试；超时、取消和连接代际变化会终止任务。`busy()` 为 false 后可以释放图像；Completed 代表本地接受。`cancel()` 后继续 poll 处理取消 END；永久关闭错误可能保留核心流，此时需等断线后建立新会话，不能覆盖旧流。完整接口及真实相机接入见[图片上传指南](IMAGE-UPLOAD.zh-CN.md)。

## BLE 状态与设备身份

`status()` / `on_status` 提供连接阶段、传输类型、title、hint 和 detail。回调由 `poll()` 派发，中间更新可合并；`READY` 表示事件通道已订阅，媒体还需各自通道就绪。Wi-Fi 配置、平台配置、网络快照和激活码字段已移除。

所有传输统一限制 model 最多 39、firmware_rev 最多 31、manufacturer 最多 63 UTF-8 字节，超长 begin 返回 INVALID_SIZE；未指定 model 时采用 device_name，因此也受 39 字节限制。

`deviceId()` 返回标准 UUID 字符串。ESP32 在 BLE RF 启动后首次生成并持久化于 `agent_link/dev_uuid`，保存失败不使用临时身份；已有 BLE UUID 保持不变。`forget()` 同步清除 BLE 配对记录并保留 UUID。CUSTOM 暂无统一的忘记绑定操作。请在 loop 中调用，避免从回调调用。

完整 BLE 固件和 OTA 需要足够的应用分区；验证使用 `esp32:esp32:esp32s3:PartitionScheme=min_spiffs`，保留双 OTA 槽。按最终固件大小和实际 Flash 容量选择分区。

0.8.2 起，ESP32 的 I/O registry、manifest 和 BLE 控制发送队列在首次使用/启动时优先分配 PSRAM，不存在或不足时回退内部 RAM；两者均不足时，注册或启动返回 `ESP_ERR_NO_MEM`。不在全局构造阶段申请 PSRAM。`stop` 保留 registry 与已准备的存储，`deinit` 在停止并解绑后释放 registry/manifest；BLE 控制队列常驻保留供后续启动复用。公开业务 API 仍须在同一应用线程调用，不得从 ISR 调用；RX critical section、DMA、Flash 写缓冲和任务栈不因本优化迁到 PSRAM。非 ESP32 便携构建保留固定静态存储。板上水位、OTA 预热与未验收项目见[硬件实测记录](HARDWARE-VALIDATION.zh-CN.md)。

BLE 连接时请求 15–30 ms 间隔、零从机延迟和 4 秒监督超时，最终值由中央设备决定；拒绝请求不会阻止控制通道进入 READY。VOICE 在控制 FIFO 或已取出的控制包等待提交时让出发送机会，避免持续音频抢占首个空闲 mbuf；原有 FIFO 顺序与 3 秒故障断连规则保留。VOICE 满包立即发送，短尾在 ring 中等待新数据最多 20 ms，END 立即刷新，减少跨调用边界的小通知。诊断构建可全局传入 `-DAGENT_LINK_BLE_LINK_DIAGNOSTICS=1` 查看实际间隔、DLE 和 PHY 事件；普通应用无需启用。请求参数或 DLE 支持不等于保证某个接收端能持续接收 32 KB/s PCM。

## 接入其他硬件

1. 声明 agent_link_io_desc_t，用 registerIo 注册传感器/执行器；ID 最多 63 字节。描述符、字符串及用户上下文须持续有效。
2. 使用 agent_output_cb_t 实现文本、音频、灯、电机或私有命令。
3. ESP32 使用内置 BLE；其他硬件提供 agent_transport_t，设置 config.transport=AGENT_TRANSPORT_CUSTOM 和 config.custom_transport。
4. 后端 bind 保存接收回调，提交完整 Agent Link 帧。ctrl_mtu 返回**含 6 字节头的完整帧容量**；发送函数须在返回前复制或消费输入，不可留存临时指针。
5. 自定义硬件提供稳定唯一的 config.device_id。名称和版本字符串在 init 时复制；传输实例须覆盖库的使用期。

CustomTransport 给出固定队列、有限步进的 UART 控制适配器，格式为 [完整帧长度 LE16][Agent Link v1 帧]，需要电脑或联网设备上的桥接程序。官方 App 不直接识别此 UART 包装；示例未提供音视频传输。

全局 Deotaland 和 C API 共用**一个实例**。不要建立多个 DeotalandClass 管理不同设备。end 保留 I/O 注册以便重连；reset / agent_link_deinit 清除注册与绑定，切换硬件配置时使用它。

I/O 注册表最多 32 个端点，自动生成的 led0 / motor0 / screen0 也占用名额；配置需要的自动端点放不下时，begin 返回 ESP_ERR_NO_MEM。传输适配器须按顺序上报断连、连接和接收数据，并在断连时清除自己的待发队列。

## OTA

包含 agent_link_ota.h，设置 config.enable_ota=true 以及准确的 model / firmware_rev；agent_link_ota_supported 查询当前构建及配置支持。OTA 进度回调在 poll 执行，flash 擦写和校验在专用任务。OTA 与 TTS 共用 PSM 0x0081，升级时应停止其他大流量任务。`agent_link_ota_needs_reconnect()` 为 true 表示已接受的升级结束/取消后仍保留共享数据通道，必须重新连接再使用 AUDIO/TTS 或发起新 OTA，避免残余固件字节混入音频；未曾取得通道的 START 拒绝不会触发该状态。

Arduino 可直接使用 `Deotaland.setOtaCallback(cb)`、`otaStatus()`、`otaBusy()`、`otaSupported()`、`otaNeedsReconnect()`、`abortOta()`、`markOtaValid()` 和 `runningVersion()`，与对应 C API 共用同一实例。状态和回调都从调用 `poll()` 的线程访问；健康确认须由应用自己的自检决定。

`prepareOta()` 和 START 都要求更新分区与运行分区地址不同，并且存在至少 8 KiB 的 `otadata`。缺少布局时预热返回 `ESP_ERR_NOT_FOUND`，START 不会接管媒体通道；镜像字节数还须小于等于目标槽容量。首次 USB 上传即应选择正确双槽布局。

可在 begin 后调用 `prepareOta()`，提前分配 32 KiB 接收环、4 KiB 内部 RAM 写缓冲、8 KiB 任务栈和事件队列；IDF 5.1+ 且支持静态对象分配时，接收环优先使用 PSRAM，分配失败回退内部 RAM。写缓冲和任务栈始终位于内部 RAM。资源保留到**设备重启**，Deotaland.end/reset 不释放这组常驻资源。未预热时兼容首次 START 创建。写入聚合为 4 KiB，支持时默认按写入进度擦除；每次 poll 最多派发一次 OTA 进度。预热不能消除 ESP-IDF 内部申请或 Flash 对系统实时性的影响。

可全局定义 `AGENT_LINK_OTA_REQUIRE_NEWER_VERSION=1`，只接受比运行镜像更新的数字版本；默认关闭。该比较使用 `esp_app_desc.version`，独立于协议字段 `config.firmware_rev`。格式为 1～3 个 uint32 数字段，例如 `1.2.3`；非空 `-prerelease` / `+build` 后缀不参与排序，非法或溢出的目标版本被拒绝。它不提供完整 SemVer 预发行排序。此宏必须用于所有库编译单元，例如 Arduino CLI 的 `--build-property "compiler.cpp.extra_flags=-DAGENT_LINK_OTA_REQUIRE_NEWER_VERSION=1"`，只在草图中写 `#define` 不会改变库编译结果。

启用引导回滚时，应用完成硬件自检后调用 agent_link_ota_mark_valid；库不会代替应用判定固件健康。

应持续 poll，直到成功终态被传输后端接受；随后后台约 2 秒后重启。end/reset 或断连可取消尚未重启的会话，等待 canBegin 后再重新 begin；直接重新初始化不会取消正在清理的 OTA。OTA 命令响应走 Service B 的 0xFFB1，即使命令从 0xFFC1 写入，也须订阅 0xFFB1 响应。

除最近一条成功 `0x05 VoiceReply` 的确切连续重试会去重并应答外，协议不提供通用命令事务缓存，超时重发可能再次执行动作；执行器建议接受明确目标值，例如“设为关闭”，避免“切换一次”在重传后产生反效果。

## 示例与实机联调

- BasicBLE：状态、电量与设备命令。
- CachedSensor：类型化温度缓存、离线合并、重连同步及错误诊断。
- SerialPrompt：短提示词、当前容量查询、100ms 重试及 5 秒超时取消。
- ResponsiveLoop：时间预算、耗时监测及非阻塞执行器状态机；发送失败重试至少间隔 100ms，永久错误结束本轮上报。
- SensorActuator / AdvancedIO：通用端点、周期调度及动态清单。
- VoicePTT / RealtimeASR：按键和采样状态机，使用统一 VOICE / AUDIO 流。
- BufferedPlayback：固定播放环、部分接收、真实水位及非阻塞硬件驱动接入；默认驱动返回 0，须替换板级实现才会播放。
- ImageStream：通用流和逐步重试。
- CameraSnapshot：真实 GC2145/ESP32 相机、后台 JPEG 编码、camera0 端点和分块上传。
- StoredJpegUpload：Flash 中的真实 JPEG，无需相机验证收图。
- ImageSequence：RGB565 测试图序列、上传进度及取消。
- CoCDiagnostics：业务流支持、连接状态及队列诊断。
- CoCOtaUpdate：OTA 显式启用、预热、进度、取消及健康确认。
- CustomTransport：没有 ESP-IDF 的 UART 适配器。
- AiotBasicAgent：ESP32-S3 AIOT-Basic 麦克风、扬声器、相机、屏幕、彩灯和可用时的 SD 录音。
- LifecycleSelfTest / HardwareSelfTest：板端集成检查。

以上共 19 个示例，打开入口和硬件替换点见 [完整索引](../examples/README.zh-CN.md)。通用语音示例默认产生合成 PCM，ImageStream 使用 2×2 测试图；CameraSnapshot 接入真实相机，麦克风和扬声器需自己的驱动，AiotBasicAgent 则提供指定板卡的外设接入。

ESP32-S3 AIOT-Basic 与 RoRoLee Android 3.4.2 已确认一次 BLE 拍照请求、JPEG 接收和画面描述，见[硬件实测记录](HARDWARE-VALIDATION.zh-CN.md)。这次 JPEG 验收不覆盖 RAW、连续图片、其他板卡或手机，也不替代音频接收/ASR、TTS 声学播放和 OTA 升级的目标硬件验收。

从源码仓库生成 Arduino 安装包可运行 `tools/package-library.ps1`，再用 `tools/check-library-package.py` 检查包结构、文档链接及私有头文件。完整命令见 [打包说明](../README.md#生成安装包)。这两个维护工具不装入安装 ZIP。

许可证见 [LICENSE](../LICENSE)。
