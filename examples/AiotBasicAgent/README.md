# AiotBasicAgent：ESP32-S3 AIOT-Basic 整板 Agent 示例

[AiotBasicAgent.ino](AiotBasicAgent.ino) 把板载数字麦克风、独立 I2S 功放、相机、ST7789 屏幕、WS2812 和 SD 录音接入 Deotaland。通过 BLE 支持 VOICE、AUDIO/ASR、TTS、JPEG 图片、Generic I/O、录音列表和 App OTA；板级配置见 [BoardConfig.h](BoardConfig.h)。

本例使用用户提供的外设接线及 [AIOT 教程](https://arduino.me/a/3218)。麦克风另据板厂 [录音示例](https://arduino.me/a/3107) 补齐 **WS=GPIO42**，默认使用标准 I2S；原说明把双线数字麦克风直接当作 PDM，配置已修正。较新 `oj_esp32s3basic/board.json` 的 ES8311 配置不能与本例独立 I2S 功放接线混用。RoRoLee、Korvo 和 ESP32-CAM 的引脚也不能直接套用。

## 第一次在 Arduino IDE 运行

1. 安装本库 ZIP 和 Espressif 官方 **Arduino-ESP32 3.3.12** Core；其 SDK 为 **ESP-IDF 5.5.5**，这是编译验证基线。本库包含启用两路 CoC 的 NimBLE，无需另装或初始化 `BLEDevice`、`NimBLEDevice`。
2. 打开本目录的 [AiotBasicAgent.ino](AiotBasicAgent.ino)，保留同目录 `.h`、`.cpp` 文件。选择 **ESP32S3 Dev Module** 和板卡原生 USB 端口。
3. 将 **USB CDC On Boot 设为 Enabled**。LCD 占用 GPIO43/44，不能同时将其用作 UART0 的 TX/RX；本例日志走原生 USB CDC。
4. 按实物模组确认 **Flash Size、Flash Mode 和 PSRAM**。相机、音频和录音缓冲需要可用 PSRAM。尤其注意：本板 **SD MISO=GPIO35 与 Octal/OPI Flash 或 PSRAM 冲突**。只有实际使用 Quad/QSPI 存储、并释放 GPIO35 的模组才能使用这组 SD 接线；不要为了 SD 将真实 OPI 模组的菜单强改成 QSPI。八线存储使用 GPIO33～37，见 [Espressif GPIO 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/peripherals/gpio.html)。本例会在 SPI 初始化前拒绝八线配置或已保留的 GPIO35，此时 SD 不可用，其余功能可继续工作。
5. 选择含 **`ota_0`、`ota_1`、`otadata`** 的双应用槽分区，例如官方 **`min_spiffs`**；每个槽都要容纳最终应用。不要用 `Huge APP` 或板卡包中只有一个 `ota_0` 的分区表。Flash/PSRAM 的实际容量尚需核对模组，不能仅凭板名推定 16 MB/8 MB。
6. 检查 [BoardConfig.h](BoardConfig.h)，确认板级引脚与功能开关。准备有受 Core 支持的 FAT 文件系统的 SD 卡（常用 FAT32），断电插入；没有卡也可以先验证 BLE、音频和相机。程序不会格式化卡片。
7. USB 上传后打开 **115200 波特率**串口监视器，复位检查 `audio`、`LCD`、`Camera`、`SD`、`Agent` 初始化结果。发送 `?` 查看状态与帮助。
8. 用支持 Agent Link 的配套 App 连接，完成事件通知订阅及所需 CoC 连接，按下文验证图片和 TTS。`p`/`r` 测试麦克风语音上行；SD 可用时再测试 `b`/`e` 录音、`y` 本地回放和 `u` 上传最近录音。

普通 BLE 扫描工具只能确认广播/GATT，不能代替完整业务 App。手机连接、控制通道 READY、媒体 CoC 可用是不同状态；READY 不等于音频或图片通道已打开。

## 实际硬件接线

| 外设 | 参数 |
| --- | --- |
| ST7789 LCD | 240×240；SPI2 SCK21、MOSI47、DC43、CS44，40 MHz |
| 独立 I2S 功放 | I2S1：BCLK39、LRCK40、DIN38；DIN38 接 ESP 的 TX/DOUT |
| 数字 I2S 麦克风 | I2S0：BCLK41、WS42、DIN2；默认 LEFT；32 位槽 |
| SPI SD 卡 | SPI3：SCK3、MISO35、MOSI14、CS46；默认 10 MHz，与 LCD 分开 |
| 相机时序 | XCLK15、VSYNC6、HREF7、PCLK13 |
| 相机 SCCB | SDA4、SCL5，I2C0 |
| 相机数据 D0～D7 | 11、9、8、10、12、18、17、16 |
| WS2812 | GPIO48；使用 RGB 数据驱动 |

默认麦克风采用标准 Philips I2S：16 kHz、两个 32 位时隙，线上每帧 64 个 BCLK；读取 LEFT 的 32 位字，取高 16 位作为业务 PCM。板厂教程提及的 [MSM261S4030H0R 数据手册](https://files.waveshare.com/upload/0/01/MSM261S4030H0R.pdf) 规定 24 位有效数据装入 32 位字；不能因教程日志中的“PDM”文字改用双线模式。只有实物确为两线 PDM 时设 `AIOT_MIC_USE_PDM=1`；`AIOT_MIC_RIGHT_CHANNEL=1` 可选择 RIGHT，默认 LEFT。业务音频仍为 **16 kHz、16 位有符号、小端、单声道 PCM**；I2S1 功放复制为双声道输出，软件音量独立。当前没有确认 LCD RESET/背光、相机 PWDN/RESET、麦克风使能或功放使能 GPIO，不猜测这些信号。

音频实现在 [BoardAudio.h](BoardAudio.h) / [BoardAudio.cpp](BoardAudio.cpp)，LCD 在 [BoardDisplay.h](BoardDisplay.h) / [BoardDisplay.cpp](BoardDisplay.cpp)，相机在 [CameraCapture.h](CameraCapture.h) / [CameraCapture.cpp](CameraCapture.cpp) 和 [camera_pins.h](camera_pins.h)，SD 在 [BoardRecording.h](BoardRecording.h) / [BoardRecording.cpp](BoardRecording.cpp)。这些是本例的板级驱动，不要求其他 Deotaland 用户采用相同接线。

`BoardAudio::begin(bool enableCapture=true)` 默认同时初始化采集和播放，保留 BLE 用法；`begin(false)` 只保留播放，不创建麦克风 RX DMA、采集队列和采集任务。`ready()` 与 `captureReady()` 分别判断播放及采集是否可用，关闭采集不能被当作麦克风故障。

## 串口操作

命令是单个小写字符，回车/换行会被忽略。网络媒体需要 App READY 与对应通道；本地 SD 录音和回放可在没有 BLE 连接时运行。

| 命令 | 操作 |
| --- | --- |
| `?` | 显示链路、初始化、SD、媒体能力、内存和计数 |
| `p` / `r` | 开始/结束 PTT，上传 VOICE 供 App/Agent 使用 |
| `a` / `s` | 开始 AUDIO 麦克风上传（默认 30 秒）/停止网络采集 |
| `c` | 拍摄一张新图并主动上传 JPEG IMAGE；不保证 App 自动在聊天中展示 |
| `t` | 发送固定 UTF-8 Prompt `你好` |
| `l` | 开关本地相机预览 |
| `b` / `e` | 开始/结束本地 SD WAV 录音 |
| `y` | 从 SD 回放最近完成录音的 PCM |
| `u` | 将最近完成录音的 PCM 通过 AUDIO CoC 上传给 App |
| `d` | 请求后台刷新录音目录缓存；录音/读取期间不执行扫描 |
| `x` | 取消媒体、播放和文件读取，停止 SD 录音并保留 partial 文件 |
| `o` | 中止当前 OTA |
| `v` | 外设初始化通过、OTA 空闲时手动确认当前固件健康 |
| `f` | 清除本地设备绑定，再按 App/平台流程绑定 |

`AIOT_CAPTURE_LIMIT_MS` 默认 **60 秒**，限制 PTT 和 SD 单次录音；App AUDIO 还服从请求的时长。录音和文件读取互斥，`e` 后要等待后台排空并关闭文件，再执行 `y`/`u`。本地回放和 AUDIO 文件上传共用一个读取器；再次操作前先完成或用 `x` 取消。

可选 PTT 按键由 `AIOT_PTT_BUTTON_PIN` 指定，默认 `-1`；若用户接线启用，它使用 `INPUT_PULLUP`、低电平按下。先检查引脚没有与相机、音频、LCD、USB 和 SD 重叠。默认无需物理按键，`p`/`r` 即可使用。

## BLE 通道与 Generic I/O

| 功能 | 必需通道 | 用途 |
| --- | --- | --- |
| 控制、状态、Prompt、设备 I/O | `0xFFC0` 服务，`0xFFC1` 命令/响应，订阅 `0xFFC4` 事件 | 订阅事件后进入 READY；查询能力、执行 I/O、收发短文本 |
| VOICE | 订阅 `0xFFA0/0xFFA1` | 真实麦克风语音交给 App，再转发给 Agent |
| AUDIO / ASR / SD PCM 上传 | CoC **PSM `0x0081`**，同时保留控制事件订阅 | App 决定转录/存储；板端不运行本地语音识别模型 |
| TTS | CoC **PSM `0x0081`** | 接收 PCM，经缓冲送到 I2S 功放 |
| JPEG IMAGE | CoC **PSM `0x0082`**，订阅 `0xFFC4` | 图片和音频使用独立 CoC |
| OTA | `0xFFB0/0xFFB1` 命令/响应，事件 `0xFFC4`，CoC **`0x0081`** | App 明确发起升级并发送应用镜像 |

Prompt 是独立短 UTF-8 文本事件，不会自动附带图片；图片分析需要 App/Agent 的相应工作流。Prompt 和列表响应容量随 ATT MTU 改变，过长内容会被拒绝。本地流结束成功仅表示库已接受数据和 END，实际接收、解码、转录和分析须在 App 验证。

### 用配套 App 验证图片和 TTS

连接设备后，在 App 对 Agent 发送：**“调用已连接设备的camera0拍一张照片并描述”**。先确认 Agent 实际调用 `camera0`，再核对图片接收和描述；这是原项目的 MCP 拍照流程。串口 `c` 仅主动上传图片，不能据此承诺 App 自动把图片展示为聊天消息。SDK 没有图片接收 ACK，`IMAGE state=COMPLETED` 或 `sent=accepted` 只表示本地发送状态，不能代替 App 收图验收。

验证 TTS 前，请在 App 中启用 **AI 结果播放**，再发送“用语音说你好”。只有文字回复不能证明 App 已生成或发送 PCM。用 `?` 核对 `CoC81_connected`、`TTS active`、`acceptedPCM` 和 `playback DMA`，再确认真实声音；具体设置位置以所用 App 为准。本次使用 **RoRoLee Android 3.4.2**，已确认 App 调用 `camera0` 后收到照片和画面描述，详见 [硬件记录](../../docs/HARDWARE-VALIDATION.zh-CN.md)。TTS 仍需单独验收。

成功初始化的硬件才注册端点和相应能力：

| 端点 | 值 | 操作 |
| --- | --- | --- |
| `camera0` | bool | `true` 拍照上传；`false` 取消待拍/在途图片 |
| `recording0` | bool | `true` 开始 SD 录音；`false` 停止并保存 |
| `volume0` | uint16，0～100 | 设置软件播放音量百分比 |
| `uptime0` | int32，秒 | 默认每秒更新运行时间；用户可见，AI 隐藏 |

`uptime0` 标记为 `AGENT_AUD_USER`，清单携带 `"audience":"user"`，供 App/用户查看诊断；App 按此标记向 AI 隐藏该端点。缓存、查询和读数通知仍保留，此标记不阻止上报。

`AIOT_UPTIME_REPORT_INTERVAL_MS` 默认 **`1000`**，控制自动更新运行时间的间隔。在 [BoardConfig.h](BoardConfig.h) 设为 **`0`** 可单独停推，用于排查 App 工具结果混入运行时间遥测；`uptime0` 仍注册，并保留用户可见标记。关闭自动更新且没有其他采样来源时，`GetReading` 没有缓存样本；不再出现运行时间不能证明 App 已收到图片。

LED 颜色走现有 LED 输出命令；文字、TTS 和 App 麦克风请求走各自协议回调。SD 初始化失败时不会上报 RECORDING 或注册 `recording0`。BLE 断连时网络媒体重置；已经开始的本地 SD 录音继续，OTA 或 `x` 则取消它。

## SD WAV、目录分页和读取

卡上目录为 `/records` 和 `/records/messages`。文件使用 **8 位十六进制递增序号**：录音先写 `rec-00000001.partial.wav`，停止后修复 WAV 头并同步关闭；完整成功才转为 `rec-00000001.wav` 并更新最近录音。重启扫描所有本例文件的最大序号，从下一序号继续，不依赖未校准的 FAT 时间。本例优先选择最大序号的有效完成录音；没有本例完成文件时才按 FAT 时间/名称选择其他有效 WAV。

`AIOT_SD_ADPCM` 默认 **`0`**，保存 44 字节头的无损 PCM16 WAV；在 [BoardConfig.h](BoardConfig.h) 设为 **`1`**，保存 60 字节头的单声道 IMA-ADPCM WAV。压缩有损，每个 256 字节块包含 505 个样本，长录音的数据部分约为 PCM16 的四分之一。最后一块补齐，`fact` 记录真实样本数；读取时解码并裁掉补齐部分。该选项只改变 SD 存储，麦克风采集、BLE 音频和回放输出仍为 PCM16。

程序不自动格式化，不覆盖卡上既有文件，也不删除用户文件。取消、掉帧或写入失败保留 `.partial.wav`，它不会作为最近完成录音；突然断电可能留下未修复 WAV 头，需在电脑上检查，不能把 partial 当作完整录音。文件命名/改名失败也会保留已有数据并报告错误。后台在修复、同步和关闭成功后原子决定提交，再改为完成文件名；在提交前接受的取消保留 partial，提交后的迟到取消不会撤销已完成录音。目标名称已有文件时改名失败，不覆盖它。

专用 worker 负责挂载、读写、WAV 校验和扫描。固定 **64 KiB PSRAM 写入 ring** 可缓冲约 2 秒 PCM，固定 **16 KiB 读取 ring** 供播放/上传；SD 延迟超出缓冲容量时停止该次录音并报告不完整，避免阻塞 BLE 轮询。挂载失败会退出 worker 并释放已分配的缓冲与缓存，保留错误状态供诊断。卡片故障通过串口状态排查；本例没有自动热插拔重挂载，换卡请断电复位。

App `0x04 ListRecordings` 沿用 Korvo 分页结构：请求为空，或 `offset(LE16)+max_entries(u8)`，可再加 `scope(u8)`；scope0 为 `/records`，scope1 为 `/records/messages`。每项包含名称、文件字节数、FAT 修改时间和 WAV 时长，返回数量、`has_more`、总数和下一 offset；messages 名称带 `messages/` 前缀。没有 RTC 时如实报告 FAT 时间，不伪造录音日期。

列表只读取缓存，在启动、录音关闭或 `d` 后由后台扫描更新。两个 scope 的缓存按实际文件数在 PSRAM 中增长，不再把第 129 个及之后的文件截掉；内存不足、目录扫描失败或超出协议的 uint16 总数限制时返回错误，不把不完整目录当作成功列表。仅列该目录顶层、非隐藏且名称能由协议表示的 `.wav`；无法表示的长名称会使诊断 `list truncated=1`。过小 ATT MTU 可能连一项都放不下，此时 App 应增大 MTU，不能反复请求同一 offset。列出文件不等于支持任意文件下载或删除。

`y`/`u` 的后台读取器检查 RIFF 的 `fmt`/`data` 块、ADPCM 的 `fact` 样本数及文件边界，支持 **单声道、16 kHz 的 PCM16 和 256 字节块 IMA-ADPCM WAV**。它处理未知块、奇数长度填充及块顺序变化；格式或长度不合法时拒绝读取。`u` 始终发送解码后的 PCM，不把 WAV 头混入 AUDIO 流，也不实现通用 FILE 传输；卡上的完整 WAV 保持不变。本地回放期间收到的 TTS 会被丢弃，防止两个来源混音。取消判断使用库的 `playbackActive()` 跟踪回复会话，DMA 尾音不会误伤下一次 TTS。

## 相机、屏幕和预览

默认相机使用 **240×240 RGB565 采集后软件编码 JPEG**，可识别 OV2640 或原项目使用的 GC2145；实际传给 App 和 LCD 的都是 JPEG。只有确认传感器支持原生 JPEG 后，才在 [camera_pins.h](camera_pins.h) 设置 `CAMERA_NATIVE_JPEG=1`。本地预览默认间隔 1 秒，网络麦克风采集、SD 录音、播放和 OTA 时会暂停；收到 App 文字后保留约 **20 秒**，再恢复自动预览。它不是网络视频流。

LCD 显示链路状态、文字和 JPEG。内置字体只支持 ASCII，小写按大写显示，未覆盖字形用 `?`；**没有中文字库**。中文请查看串口/App 中的 UTF-8，串口本例文字缓冲最多 512 字节。不同 ST7789 模组的方向、偏移或 RGB/BGR 应以实际显示校准。

## App OTA 的准备

首次 USB 上传就要选对双槽分区。OTA 不能补出缺失的 `ota_1` 或修改分区表；应用能编译进一个大槽，也不证明可双槽升级。为同一 PCB 保持稳定型号，并为每次发布填写真实固件版本。

App START 必须给出匹配型号、目标版本、准确字节数和 SHA-256，发送为同一芯片/PCB 编译的 **应用 `.bin`**，不能发送合并烧录镜像、bootloader 或分区表。对外上报的设备固件版本 `firmware_rev` 不会修改 ESP-IDF 应用元数据。

OTA 与 AUDIO/TTS 共用 `0x0081`。升级开始时示例取消采集、播放、图片及 SD 录音/读取；已经开始的升级终止后，`agent_link_ota_needs_reconnect()` 明确阻止网络 VOICE/AUDIO/TTS 与下一次 OTA，须重新连接后再试。此时本地 SD 录音/回放仍可使用。收到 100% 还需 SHA-256 和镜像校验通过才算成功。

需要回滚的项目必须确认实际 bootloader 配置，在硬件和业务健康检查通过后确认新固件；库开关不能给预编译 bootloader 增加 rollback。`v` 只检查本例的外设初始化/空闲条件，不能替代实物功能验收。详情见 [CoCOtaUpdate](../CoCOtaUpdate/README.md)。

## BLE 连接与资源配置

示例固定使用 `AGENT_TRANSPORT_BLE`，启动时初始化麦克风采集与音频播放，并准备 VOICE、AUDIO、IMAGE 和按分区条件启用的 OTA。相机和 LCD 后台任务使用内部 RAM 栈。`AIOT_CAMERA_PSRAM_DMA` 默认关闭；可在支持的 ESP32-S3 相机驱动上显式开启，初始化失败时回退普通采集。

## 边界与验证范围

- 没有确认电池测量、震动器或板载 PTT 按键，不上报模拟电量或凭空提供这些硬件。普通 LED、光线、蜂鸣器、独立按键实验可能需要用户跳线；教程的单模块引脚不能同时套用到相机/音频/SD。
- VIDEO、FILE、图片/视频下行和 Agent 列表等预留接口不代表已有完整业务。相机预览不是视频云传输，录音分页不是任意文件管理器。
- 已在 COM7 的 ESP32-S3 / 16 MB Flash / 8 MB OPI PSRAM 上进行自动检查，详见 [实测记录](../../docs/HARDWARE-VALIDATION.zh-CN.md)。当前麦克风输入仍出现恒值，初始化成功不能视为收音正常；`?` 提供样本变化、DC、AC RMS、削波和内存水位。屏幕观感、实际声音及手机 CoC 端到端传输仍未验收。
- 串口诊断按可写空间分片输出；关闭串口窗口也能继续处理 BLE 与采集。`?` 的丢块分项区分 RX(queue+ISR)、待发送 PCM 超时、替换及取消，便于定位拥塞；输出等待不会阻塞业务。

相关说明：[入门与排错](../../docs/BEGINNER-GUIDE.zh-CN.md)、[API 参考](../../docs/REFERENCE.zh-CN.md)、[BLE CoC](../../docs/BLE-COC.zh-CN.md)、[图片上传](../../docs/IMAGE-UPLOAD.zh-CN.md)、[全部示例](../README.zh-CN.md)。
