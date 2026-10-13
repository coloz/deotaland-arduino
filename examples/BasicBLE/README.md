# BasicBLE：BLE 连接与模拟电量

这是 BLE 入门示例。无需外接传感器或按钮，通过 USB 串口和兼容 Agent Link 的 App 检查设备启动、连接、事件订阅及电量上报。

## 准备

- 带 BLE 控制器的 ESP32 开发板、数据 USB 线和 Arduino IDE。
- 安装官方 ESP32 Arduino Core 和本库；本库已内置 NimBLE，无需另装或修改 Core。
- 使用兼容 Agent Link 的 App。普通 BLE 扫描工具可调试 GATT，但不会自动完成 Agent 业务。

板型、分区和 USB 串口设置见[入门指南](../../docs/BEGINNER-GUIDE.zh-CN.md)。

应用分区须容纳最终 BLE 固件；启用 OTA 时选择实际 Flash 容量支持的双应用槽。可使用 `PartitionScheme=min_spiffs`，其他容量按最终编译输出选择，勿只改变 Flash 菜单而忽略实际容量。

## 使用步骤

1. 打开 [BasicBLE.ino](BasicBLE.ino)，按实际开发板选择板型、端口及足够大的应用分区，编译上传。
2. 打开 **115200** 波特率的串口监视器。
3. 在 App 中扫描并连接 **`Deotaland-ESP32`**。
4. App 订阅事件特征 **`0xFFC4`** 后，串口应显示 `连接状态: READY`。
5. 查看 App 的模拟电量，以及串口中的设备 UUID。

| 串口输入 | 作用 |
| --- | --- |
| `?` | 查询启动结果、连接状态、UUID 和自定义命令次数 |

状态变化时会打印日志，未发生变化时约每 10 秒打印一次。广播名可通过 `config.device_name` 修改；`config.firmware_rev` 是自己的应用版本。

## 预期结果

- `CONNECTED` 表示已连接，`READY` 表示事件订阅就绪；只连接而未订阅时不会进入 `READY`。
- 就绪后程序每 5 秒尝试上报一次固定的 **80%** 电量，值相同时由库去重，不保证每次都发包。
- 自定义命令 `0x7E` 只增加计数，不操作外设；它是 App 协议命令，不是串口字符。
- `reportBattery` 成功表示本地接受，不能作为对端已经收到的确认。

电量值是模拟数据。接入真实电池测量时，应替换上报值并处理采样错误。

下一步可学习 [CachedSensor](../CachedSensor/README.md)，或返回[示例索引](../README.zh-CN.md)。
