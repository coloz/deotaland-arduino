# 相机拍照与图片上传

`DeotalandImageUpload` 和 [CameraSnapshot](../examples/CameraSnapshot/CameraSnapshot.ino) 完成设备端的相机采集、JPEG 编码及 BLE 图片上传。0.8.0 内置启用 CoC 的 NimBLE 主机，可使用普通官方 ESP32 Arduino Core。协议保持与 Agent_link-main 一致：图片数据走 L2CAP CoC `PSM 0x0082`，开始/结束通过事件通知 `0x54 / 0x55` 发送。

## 运行真实相机示例

1. 使用有 PSRAM 的 ESP32 相机板。示例默认采用原版 ESP32-S3 GC2145 的接线；先核对 [camera_pins.h](../examples/CameraSnapshot/camera_pins.h)，按实际硬件修改。GC2145 使用 RGB565 帧并在后台编码为 JPEG；支持 JPEG 的传感器也可使用原生 JPEG。
2. 开启与板卡匹配的 PSRAM、Flash、USB/串口设置。示例依赖 Core 中的 `esp32-camera` 驱动，不能仅凭有 BLE 就判断支持相机。
3. 使用本库 0.8.0 或更新版本。库已内置 NimBLE-Arduino 2.5.1 并从源码启用两个 CoC 通道，普通官方 ESP32 Arduino Core 即可编译，不需要另装 BLE 库或重编译 SDK。[CoC 说明](BLE-COC.zh-CN.md)介绍配置和协议。
4. 编译 CameraSnapshot，打开 115200 串口。连接 App，订阅事件特征 `0xFFC4`，并打开图片 CoC `0x0082`。READY 只表示控制通道已就绪。
5. 串口发送 `c`，或按接在 GPIO3 的高电平有效按钮。App 也可控制 `camera0` 端点，写入 BOOL `true` 发起拍照。串口 `?` 查看诊断，`p` 向 Agent 发送拍照分析的文字请求，由 Agent 再调用 `camera0`。

相机采集和 JPEG 编码由后台任务处理。所有 Deotaland 接口仍在 `loop()` 线程调用，App 回调只安排任务。同一时刻只处理一个拍照/上传请求；旧连接中的请求和照片不会在重连后补发。

`camera0` 与原版保持相同的 ID、`camera.capture` 语义、OUT 方向和 BOOL 类型。`false` 不触发拍照，非法参数由核心拒绝。文字提示词是独立功能，不会自动附带照片；Agent 要取新照片时需要调用这个端点。

`camera_pins.h` 中 `CAMERA_AUTO_PROMPT_INTERVAL_MS` 默认是 0，关闭周期提示词；改成 `10000u` 可恢复原版每 10 秒的文字提示。长提示词需 App 协商足够大的 MTU；示例会报告长度错误。采集默认 15 秒超时，上传默认 30 秒超时；采集超时后等待后台安全返回，再释放并丢弃结果。

## 上传已有图片

文件、相机或其他来源的 JPEG/RGB565 也可直接交给上传器，不依赖相机驱动。上传器不分配图片缓存，而是借用调用方的缓冲区。每轮最多发送 512 字节，图片可大于 BLE 的 32 KiB 排队缓存。

```cpp
#include <Deotaland.h>

DeotalandImageUpload upload;

// 已 begin 成功且 READY；jpeg 指向已有图片，必须保持有效。
esp_err_t beginPhoto(const uint8_t* jpeg, size_t bytes,
                     uint16_t width, uint16_t height) {
  agent_stream_opts_t opts = {};
  opts.encoding = AGENT_ENC_JPEG;
  opts.width = width;
  opts.height = height;
  opts.total_bytes = static_cast<uint32_t>(bytes);
  return upload.start(opts, jpeg, bytes, millis());
}

void loop() {
  Deotaland.poll();
  upload.poll(millis());
  // !upload.busy() 后才能释放或复用 jpeg。
  // upload.state() / result() / acceptedBytes() 用于查询本地进度。
  delay(1); // 让后台任务和默认 ESP32 idle watchdog 获得运行机会。
}
```

- JPEG 必须给出非零宽、高和有效字节数；`total_bytes` 为零时上传器使用传入的 `bytes`，非零时必须与之相同。
- RAW 是大端 RGB565，字节数必须等于 `width × height × 2`。相机的原始像素字节顺序可能不同，不能直接把所有 RGB565 相机帧当作协议 RAW 图片发送；CameraSnapshot 统一上传 JPEG。
- `start()` 返回 `ESP_OK` 表示上传器接受任务。它不会立即把整张照片塞入发送队列。已有 IMAGE 流时不能启动另一个上传器。
- `poll(now_ms)` 持续推进等待通道、分块写入、关闭等步骤。NO_MEM/TIMEOUT 时保留当前块和偏移，下轮重试；默认上传期限是 30 秒，失败或取消后另有最长 1 秒的 END 清理。毫秒时间使用无符号差值，支持 `millis()` 回绕。
- `cancel()` 停止读取图片并安排取消结束；继续调用 `poll()` 完成清理，直到 `busy()` 为 false。断连会使任务失败，调用方需为新连接重新采集照片。
- Completed 只表示全部数据和结束标记已被本地接受。使用 `Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status)` 查看后台排空；开始下一张照片前应等待排空。协议没有图片送达 ACK，不能据此宣称 App 已成功展示或保存。

API 的 `encoding` 与 BLE `0x54 ImageStart` 的 `format` 编号不同，BLE 后端负责转换；调用方仍使用公开枚举，不需修改图片或枚举值。

| 图片 | API `encoding` | `0x54` 的 `format` |
| --- | --- | --- |
| JPEG | `AGENT_ENC_JPEG`（1） | 0 |
| 大端 RGB565 | `AGENT_ENC_RAW`（0） | 1 |

0.8.2 修正了这层转换。此前直接发送 API 枚举，会把 JPEG 标成 RGB565、把 RGB565 标成 JPEG，即使字节全部提交，接收端仍可能拒绝或无法解码。

## 接入其他相机

修改 CameraSnapshot 的引脚及像素格式配置。GC2145 没有原生 JPEG，必须采集 RGB565 后用 `frame2jpg()` 编码。示例为 GC2145 保留原版的采集引擎启动处理，仅在识别到该传感器时执行。

相机帧和 JPEG 缓冲区必须各释放一次。采集结果在后台完成后移交主循环；如果断线、请求过期或上传失败，示例会丢弃并释放该结果。不要在 BLE 回调或中断中调用相机驱动、JPEG 编码或上传器。

自定义传输可实现 IMAGE 流来复用上传器。内置图片传输使用 BLE CoC。本功能完成设备到 App 的 BLE 协议链路；配套 App SDK 不在本仓库内。

无需相机时，先使用 [StoredJpegUpload](../examples/StoredJpegUpload/StoredJpegUpload.ino) 发送 Flash 中的真实 JPEG；[ImageSequence](../examples/ImageSequence/ImageSequence.ino) 演示连续 RGB565 测试图、进度与取消。[CoCDiagnostics](../examples/CoCDiagnostics/CoCDiagnostics.ino) 区分编译支持和对端通道就绪。

ESP32-S3 AIOT-Basic 与 RoRoLee Android 3.4.2 已完成一次 App 调用 `camera0`、接收照片并返回画面描述的实测，记录见 [硬件验证](HARDWARE-VALIDATION.zh-CN.md)。其他板卡、RAW 接收、颜色与连续传输仍应分别验收；本地完成状态不能代替 App 的接收结果。
