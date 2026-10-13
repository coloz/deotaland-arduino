# Agent_link-main 对照与 Arduino 版本完善记录

核对日期：2026-10-13；适用库版本：**0.9.3**。对照对象是本工作区的 `Agent_link-main/firmware/components/agent_link` 和 `firmware/boards` 实际源码，范围为原 BLE 功能及 RoRoLee App 互操作；Muse 不在此次移植和验收范围内。原项目部分 README 的状态表较旧，以下结论以代码为准。

RoRoLee Android 3.4.2 已确认调用 `camera0` 后收到照片和画面描述。完整音频收发与播放、App OTA 尚未完成端到端验收；“已移植”及主机/SDK 检查通过不能等同于所有硬件功能已通过。

## 功能覆盖

| 功能 | Agent_link-main 实际实现 | Arduino 库当前情况 |
| --- | --- | --- |
| BLE 控制与设备信息 | FFC0/FFC1/FFC4、Battery、DIS、共享设备 UUID、配对绑定 | 已移植；增加轮询预算、队列统计和断线隔离 |
| VOICE 语音上行 | FFA0/FFA1 GATT Notify，PCM16 | 已移植；控制发送优先、短尾最多合并 20 ms、拥塞重试和预热；旧协议没有可告知对端截断的 END 标记 |
| AUDIO / ASR 上行 | CoC PSM 0x0081，设备上传 PCM | 已移植；[AiotBasicAgent](../examples/AiotBasicAgent/README.md) 接入板载数字麦克风，也可上传 SD 录音解码后的 PCM |
| TTS 音频下行 | PCM16 / IMA-ADPCM 解码和流控 | 已移植；可部分接收的播放接口、真实水位统计和回复状态查询；修复已收到 CoC SDU 被后到的 GATT END 越过而丢失的尾音 |
| IMAGE 图片上行 | PSM 0x0082；JPEG / RGB565；0x54 / 0x55 开始、结束事件 | 已移植；[图片上传器](IMAGE-UPLOAD.zh-CN.md) 支持超过发送环容量的照片、分块重试、取消和断线清理；修正 API 编码到 wire JPEG=0 / RGB565=1 的映射 |
| 通用 I/O | 清单、分页、执行器、读数、自动 LED / 屏幕 / 震动端点 | 已移植；增加类型化缓存读数与离线最新值合并；本次补严格 UTF-8 校验 |
| SD 录音与列表 | Korvo 板有 PCM / IMA-ADPCM WAV 和 0x04 分页 | AIOT-Basic 示例有后台录音、分页、本地回放和 PCM 上传；可选 ADPCM 存储及读取解码；两个目录的列表缓存按实际文件数增长，移除此前 Arduino 列表的 128 项截断 |
| BLE OTA | 型号检查、SHA-256、Flash 写入、取消、重启、健康确认 | 已移植；默认须显式启用；本次补分区前置检查、Arduino OTA 方法及可选严格版本检查 |
| Agent 列表 / 选择上报、图片和视频下行 | 有接口声明，未完整分发 | 保留接口及不支持状态；不是本次 Arduino 移植遗漏 |
| 各板专用硬件和 UI | ES8311、触摸、温湿度、气压、电量、工牌 UI 等按板实现 | 尚未逐板移植。AIOT-Basic 驱动和接线只适用于对应硬件；通用 SDK 回调可接用户自己的驱动 |

BLE 内置后端随库编译 NimBLE-Arduino 2.5.1，默认两路 CoC；无需改 Arduino Core。两路是同时连接数量上限，OTA 与 AUDIO/TTS 共用 0x0081。详细约定见 [CoC 说明](BLE-COC.zh-CN.md) 和 [API 参考](REFERENCE.zh-CN.md)。

原版核对入口：`firmware/components/agent_link/src/agent_link.cpp`、`transport_ble.cpp`、`ota_service.cpp`，以及 `firmware/boards/korvo/` 的录音与 ADPCM 驱动。这些路径属于单独的 Agent_link-main 工程，不随 Arduino 安装包分发。

## 本次修复与优化

### TTS 数据与 END 的接收顺序

原移植路径在 NimBLE host 收到完整 CoC SDU 后先保留 mbuf，随后才由 `poll()` 把音频放入核心接收队列。如果 GATT `0x05 status=3` 在此期间到达，END 会先入队，已收到的 PCM/ADPCM 尾部被误当作 END 后的迟到数据丢弃。

现在 SDU 到达时先在同一 FIFO 放入小票据，mbuf 留在传输层。`poll()` 处理票据并按播放背压消费全部数据后，才处理后面的 END 或下一 START；不在 host 回调执行板级播放操作，也不为此复制完整媒体。满队列时保留 SDU、暂不补接收信用，并对无法保持顺序的 GATT 写入返回资源不足。断线、停止或控制会话变化会隔离旧数据并释放缓冲；真正发生在 END 之后的迟到数据仍会丢弃。

票据重试由 worker 提交 NimBLE 事件，主循环只唤醒 worker，避免事件队列满时阻塞 `poll()`。PCM 部分写入、两块 ADPCM 跨轮询、下一 START、队列满、快重连及 OTA 共用通道已有实际生产代码回归；手机 TTS 播放仍需独立验收。

### 回复开始、结束重发

App 对 `0x05 VoiceReply` 的确切重发此前可能清空库内待播放 PCM，或重复调用 `on_audio_end`。现在对最近一次成功请求的 sequence 和完整 payload 做连续重试去重，仍返回应答。遇到另一个命令、连接世代变化、停止或重新初始化时清除记录。

这是音频控制请求的局部去重，不是所有执行器命令的通用事务缓存。执行器仍应使用明确目标值，例如“设为关闭”；不要依赖重复“切换一次”只执行一次。同一个 session ID 配上新 sequence 仍能开始新回复。

### 字符串值验证

`AGENT_VAL_STR` 读数和执行器参数现在拒绝内嵌 NUL、无效或截断的 UTF-8，包括过长编码、代理区字符和超过 U+10FFFF 的值。有效中文及空串仍可使用。这样线上的完整值不会在 C 字符串回调中被悄悄截断。按 UTF-8 字节计算容量，详见 [读数与类型](REFERENCE.zh-CN.md)。


### BLE 协商与 CoC 发送背压

BLE 后端请求 15～30 ms、零 latency 的连接参数，并在链路过程结束后对支持 BLE 5 的芯片请求 2M PHY。请求只是偏好，中央设备可以拒绝或稍后改回其他参数；失败不会被当作业务错误，也不保证获得指定吞吐。诊断构建记录实际连接参数、DLE 和 PHY。VOICE 的控制优先与短尾合并继续保留。

RoRoLee 图片联调发现 MSYS 小池耗尽时，SDK 的 `ENOMEM` 可发生在部分 SDU 已提交之后。现在短尾输入也从大池申请，host 按真实发送工厂、对端 MPS 和小池余量做发送前预算，保留控制余量并限制连续提交。仅 SDK 接受前的资源不足可安全重试；SDK 消耗后的错误保留为失败，不能重播以免重复图片或音频。合法低 MPS 若容不下原 512 字节 SDU 的预算，会缩小 SDU；媒体字节和协议字段不变。

IMAGE 另修正公开编码枚举到线协议的显式转换：`AGENT_ENC_JPEG=1` 对应 wire JPEG **0**，`AGENT_ENC_RAW=0` 对应 wire RGB565 **1**。原始 JPEG 没有新增前缀、尾部或实验 END 延迟。该修复后 RoRoLee 的照片与描述已实际确认，详见[硬件记录](HARDWARE-VALIDATION.zh-CN.md)。

### OTA 分区、内存和 Arduino 接口

库在准备和接受升级前检查更新槽与当前运行分区不同，并且存在至少 8 KiB 的 `otadata`。单应用槽不会因 `esp_ota_get_next_update_partition()` 返回运行槽而被误认成可升级布局；镜像大小仍必须放入目标槽。

Arduino 接口增加 `setOtaCallback()`、`otaStatus()`、`otaBusy()`、`otaSupported()`、`otaNeedsReconnect()`、`abortOta()`、`markOtaValid()` 和 `runningVersion()`。[CoCOtaUpdate](../examples/CoCOtaUpdate/README.md) 演示这些方法。进度回调继续在 `poll()` 执行，Flash 操作由后台任务执行。

受支持的 IDF 构建优先将 OTA 的 32 KiB 接收环放入 PSRAM，失败时回退内部 RAM；Flash 写缓冲和任务栈保留在内部 RAM。`prepareOta()` 可在采集开始前预热，不能消除 Flash 擦写对其他任务的影响。

全局定义 `AGENT_LINK_OTA_REQUIRE_NEWER_VERSION=1` 可拒绝相同或更旧的数字版本，默认关闭以保持兼容。比较运行镜像的 `esp_app_desc.version`，不是 `config.firmware_rev`；后者是对外上报的设备固件版本字符串，不能替代运行镜像描述符。版本由 1～3 个 uint32 数字段组成，非空的预发行/构建后缀不参与先后排序，因此这不是完整 SemVer 预发行排序器。

### 可选 ADPCM 录音

[AiotBasicAgent](../examples/AiotBasicAgent/README.md) 的 `AIOT_SD_ADPCM` 默认 `0`，保持原来的无损 PCM16 WAV；设为 `1` 使用原项目同类的 IMA-ADPCM WAV。压缩有损，以 256 字节存储 505 个样本，长录音的数据部分约为 PCM16 的四分之一，文件头和尾块补齐另计。

停止时补齐最后一块，`fact` 记录真实样本数；读取时去除补齐样本并解码回 PCM16。`y` 回放和 `u` 上传接口不变，网络 AUDIO 仍发送 PCM，不能直接把 WAV 头或 ADPCM 文件字节写到 RAW 音频流。SD 的后台缓冲、部分文件保护和 GPIO35 冲突检查继续有效。

修复完成录音改名失败的问题：此前先创建目标空文件，再调用 rename，而 [FatFs 的重命名规则](https://elm-chan.org/fsw/ff/doc/rename.html) 不覆盖已有目标，导致正常保存也失败。现在直接使用其不覆盖语义；目标冲突保留已有文件及 partial。同步关闭后用原子提交点确定取消结果，避免迟到取消产生状态不一致。

挂载失败时退出并释放后台任务、PSRAM 缓冲及目录缓存；读取失败后的旧清理也不会清除新请求的占用状态。新增的 WAV 解析覆盖 ADPCM 样本数、块顺序、奇数长度填充及边界检查。

`0x04 ListRecordings` 继续沿用 Korvo 的 offset、数量、scope 和 `has_more` 分页字段。`/records` 与 `/records/messages` 的缓存按实际文件数在 PSRAM 增长，不再把第 129 个及之后的文件隐去；内存不足、扫描失败或超过 uint16 总数限制明确返回错误。列表是后台扫描的缓存，仍需刷新，且不提供通用文件下载/删除。此板的 OPI 存储与 SD GPIO35 冲突仍须拒绝挂载，动态列表不能绕过引脚保护。

### 预留 Agent 选择接口

`selectAgent()` 在连接 READY 后明确返回 `ESP_ERR_NOT_SUPPORTED`，不再错误提示“尚未就绪”；无效参数及离线调用仍返回相应错误。原项目没有完成选择上报协议，库不会发送猜测的事件。

### 整板 BLE 资源配置

整板示例固定使用 BLE，保留麦克风采集、音频播放、相机与 LCD。相机/LCD 任务使用内部 RAM 栈，可选相机直接 PSRAM DMA 默认关闭。Wi-Fi 后端、云端 MQTT 桥和相关配置、示例已移除。

## 仍需后续工作的范围

- 若要在 ES8311、Korvo 触摸屏或工牌板上直接运行，需要新增对应板的引脚、驱动和示例。不能把这些不同硬件的功能全部套到 AIOT-Basic 上。
- 如需中文屏幕字体，应按目标显示库及字符范围加入字库；当前 AIOT-Basic 内置 ASCII 字体，原项目也未提供通用完整中文字库。
- 除已确认的 RoRoLee Android 单张 JPEG 外，完整 AUDIO/ASR、TTS 播放、其他手机 CoC 兼容、有效麦克风输入、实际声音与屏幕观感、SD 拔卡/断电恢复和 OTA 重启/回滚仍需现场或 App 联调。此次 OPI 模组与 SD GPIO35 冲突，保护拒绝挂载；不得绕过。已完成项目见[硬件记录](HARDWARE-VALIDATION.zh-CN.md)。

## 验证记录

本轮 BLE-only 检查单独列出；其余记录为移除前的历史检查，实际条件见硬件记录。

| 检查 | 范围与结果 |
| --- | --- |
| 本轮 BLE-only 回归 | 全部 19 个 S3 示例、C3/H2 BasicBLE 零警告编译通过；BLE 接收/播放顺序 260 项、核心存储/生命周期 57 项主机断言通过；公共 C API 通过 C11 编译。未做新的真机验收 |
| 可移植核心 / Arduino 封装 | 实际生产代码 C++11 编译，139 项断言通过 |
| 协议与字段语义 | 生产及原版实际函数/头文件共 253 项断言通过；覆盖所有 caps/I/O/事件/codec 枚举、Manifest JSON 语义、DeviceInfo、应答帧及全部合法 ADPCM 步长；IMAGE 实际函数另有 23 项完整报文检查 |
| TTS FIFO 与生命周期 | 实际核心、队列、音频与抽取的 BLE 接收入口 260 项断言通过；OTA 所有权/背压边界替身构建 273 项通过，包括已到 SDU→END、部分播放、下一 START、快重连和缓冲释放 |
| BLE 调度与 PHY | 生产策略 145 项检查通过；实际 advisory PHY 函数在支持、芯片不支持、host 功能关闭三分支共 21 项检查通过；Core 3.3.12 transport 诊断开/关对象编译成功 |
| OTA | 真实服务覆盖默认与严格版本检查、PSRAM 及内部 RAM 回退、IDF 5.0 与静态分配禁用、非 ESP32 平台；全部通过 |
| SD 录音 | 实际 worker/codec 的 PCM 45、ADPCM 45、OPI 防护 1 项，共 91 项主机用例通过；含超过 128 项分页、缓存扩容失败及目录读取错误；平台、任务及文件系统边界使用替身，不能代替有卡实测 |
| COM7 实物与 RoRoLee | 最新诊断构建的实际 BLE/GATT 检查 129 项通过、0 项失败，SD 因 OPI 冲突跳过 1 项。VOICE 采样 255040 个，板端接受/发送与 PC 接收均为 510080 字节；2501 帧（seq 0～2500）无缺口，15.936147 秒内平均 32007.74 B/s，采集丢块与待发送超时均为 0，板端正常结束。实见 30 ms、TX/RX 2M；92 条命令 RTT 中位数 59.791 ms、P95 63.175 ms、最大 90.510 ms。RoRoLee Android 3.4.2 已确认 camera0 照片接收和描述。麦克风 PCM 恒值 -1 仍独立未通过；新 TTS 顺序修复、完整音频和真实 App OTA 仍需端到端验收 |

验证工具和日志位于源码工作区的 `.build`，不会恢复库内 `tests`、`extras`，也不随安装包分发。表中的主机检查使用平台边界替身；COM7 与 RoRoLee 的实物结果、未通过和未覆盖项目单独见[硬件记录](HARDWARE-VALIDATION.zh-CN.md)。每个示例的运行条件见 [19 个示例的索引](../examples/README.zh-CN.md)。
