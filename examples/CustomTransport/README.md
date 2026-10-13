# CustomTransport：UART 控制协议桥接

[草图](CustomTransport.ino)和[适配器](SerialAgentTransport.h)演示 `AGENT_TRANSPORT_CUSTOM`：
在 UART/Arduino `Stream` 上发送完整 Agent Link 控制帧，并用固定队列和有限步进保持主循环运行。
**官方 App 不直接识别这种 UART 包装**；电脑或另一台设备必须提供对应的二进制桥接程序。

## 前置条件

- 有标准库和足够 RAM 的 32 位 Arduino 板，例如使用自定义传输的 STM32 或 ESP32。
- 能读写二进制串口的桥接程序；本例没有提供 PC 桥接客户端。
- 所选 `Stream` 必须实现有效的 `availableForWrite()`，并保证在所报容量内写入快速返回。
- 不需要 BLE、CoC、麦克风或扬声器；本适配器只支持控制帧，不支持媒体流。

`Stream` 默认的 `availableForWrite()` 返回 0；若具体串口没有实现，适配器将一直无法发出数据。
非 ESP32 的板卡/Core 还需满足本库的 C++11 和内存要求；这里不声称已完成 STM32 真机联调。
安装步骤见[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)，适配器约定见 [API 参考](../../docs/REFERENCE.zh-CN.md)。

## 串口与稳定身份配置

源码默认使用 `SerialAgentTransport uartTransport(Serial)`，并执行 `Serial.begin(115200)`。
这条 `Serial` 就是二进制协议链路，不能在上面另加 `Serial.print()` 日志或发送文本命令。
上传完成后让桥接程序独占该端口，不要同时用串口监视器发送文本。

产品中建议改用板卡支持的独立硬件 UART，例如把构造参数与初始化对象都改为对应的 `Serial1`。
UART 引脚、端口初始化和电平按实际板卡选择；本例没有指定通用 TX/RX GPIO。
若 ESP32 的启动/调试日志也走同一硬件 UART，日志仍可能污染帧，应将桥接与日志分到不同串口。
USB CDC 的 `Serial` 行为取决于板卡/Core；不能假定每个 `Stream` 都等同于硬件 UART。

`deviceId[16]` 是固定的示范身份，所有照抄本例的设备会得到相同 ID。
替换为每台实体设备稳定、唯一的 16 字节值，并跨重启保持不变；不要每次启动随机生成临时 ID。
`config.device_name` 可改为产品名；传输实例及其引用的串口必须在库使用期间持续有效。

## 帧格式

每个方向都使用如下二进制结构，外层长度包含完整的 6 字节协议头：

```text
[完整 Agent Link 帧长度：LE16]
[version=01][type][command/event id][sequence][payload 长度：LE16][payload]
```

外层完整帧长度允许 6～512 字节；不是以换行分隔，也不是 ASCII 十六进制文本。
内层 type 为 `01` 命令、`02` 响应、`03` 事件；请求响应通过 sequence 配对。
例如查询设备信息 `0x01`、sequence 为 1、无载荷时，桥接程序实际发送这 8 个字节：

```text
06 00 01 01 01 01 00 00
```

接收响应也先读取 2 字节外层长度，再读取完整帧；不能假定一次串口 read 会拿到一整帧。
启动时会发送清单等事件，桥接程序应持续读取，并按 type/id/sequence 分派响应和事件。

## 上传与操作

1. 打开 [CustomTransport.ino](CustomTransport.ino)，确保同目录的 header 一起保留。
2. 配置实际串口和唯一 `deviceId`，选择正确板卡后上传。
3. 启动桥接程序，打开对应串口；默认速率为 115200，使用二进制读写。
4. 先解析设备发出的事件，再发送上述设备信息查询，检查响应的 id 和 sequence。
5. 约每 5 秒程序尝试上报固定电量 80%、未充电；库可能对相同电量去重。

本例没有 `p`、`?` 等文本命令，也没有启动失败日志。启动失败应通过板级 LED 或调试器观察 `started`。
适配器启动后立即报告连接和 READY，即使串口另一端还没有客户端；READY 不证明桥接对端在线。
若需真实连接检测，须自行修改适配器，在对端可用后才提交连接/就绪事件。

## 缓冲与当前限制

- TX 固定容纳 4 帧；队列满返回 `ESP_ERR_NO_MEM`，调用者在后续循环重试。
- 每次适配器 `poll()` 最多读取 64 字节、发送 64 字节；不会阻塞等待整帧或 flush。
- 核心 RX 队列满时保留已收完整帧；未完成帧空闲超过约 1 秒会重置接收进度。
- 非法外层长度会丢弃其声明的载荷；截断后等待空闲超时重新同步。
- 桥接端须限制发送速度或实现硬件流控；适配器不能阻止底层 UART RX 缓冲溢出。

VOICE/AUDIO/IMAGE/VIDEO/FILE 的操作指针为空，相关流接口返回不支持。
本例不提供认证、加密或 App 网络桥接；帧中的加密标志不代表已有加密实现。
其他练习见[示例索引](../README.zh-CN.md)，BLE 的不同传输方式见 [CoC 说明](../../docs/BLE-COC.zh-CN.md)。
