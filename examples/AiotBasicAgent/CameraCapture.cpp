#include "CameraCapture.h"

#include <stdlib.h>
#include <string.h>

#if DEOTALAND_CAMERA_AVAILABLE
#include <Arduino.h>
#include "BoardTasks.h"
#include "driver/i2c_master.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "img_converters.h"
#include "camera_pins.h"

#if AIOT_CAMERA_PSRAM_DMA
// Optional in older Arduino camera bundles. A weak reference keeps those
// bundles buildable and preserves their ordinary internal DMA path.
extern "C" esp_err_t esp_camera_set_psram_mode(bool enable) __attribute__((weak));
#endif

esp_err_t CameraCapture::begin() {
  if (worker_) return ESP_ERR_INVALID_STATE;
#if CAMERA_REQUIRE_PSRAM
  if (!psramFound()) return ESP_ERR_NO_MEM;
#endif
  // The Core camera's SCCB_Init otherwise picks its compile-time default I2C1.
  // Precreate I2C0 and pass SDA=-1 so SCCB_Use_Port reuses our explicit bus.
  i2c_master_bus_config_t busConfig = {};
  busConfig.i2c_port = CAMERA_SCCB_I2C_PORT;
  busConfig.sda_io_num = static_cast<gpio_num_t>(CAM_PIN_SIOD);
  busConfig.scl_io_num = static_cast<gpio_num_t>(CAM_PIN_SIOC);
  busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
  busConfig.glitch_ignore_cnt = 7;
  busConfig.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  esp_err_t result = i2c_new_master_bus(&busConfig, &bus);
  if (result != ESP_OK) return result;
  sccbBus_ = bus;
  requests_ = xQueueCreate(1, sizeof(uint32_t));
  results_ = xQueueCreate(1, sizeof(CameraImage));
  if (!requests_ || !results_) result = ESP_ERR_NO_MEM;

  bool cameraInitialized = false;
  if (result == ESP_OK) {
    camera_config_t config = {};
    config.pin_pwdn = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_sccb_sda = -1;
    config.pin_sccb_scl = -1;
    config.sccb_i2c_port = CAMERA_SCCB_I2C_PORT;
    config.pin_d7 = CAM_PIN_D7;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d0 = CAM_PIN_D0;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_pclk = CAM_PIN_PCLK;
    config.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = CAMERA_PIXEL_FORMAT;
    config.frame_size = CAMERA_FRAME_SIZE;
    config.jpeg_quality = CAMERA_SENSOR_JPEG_QUALITY;
    config.fb_count = CAMERA_FB_COUNT;
    config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    result = esp_camera_init(&config);
#if AIOT_CAMERA_PSRAM_DMA
    if (result == ESP_OK && psramFound() && esp_camera_set_psram_mode) {
      // The public API reinitializes the driver. No frame or worker is yet
      // published, and SCCB reuses our externally owned I2C0 bus. Apply all
      // sensor adjustments only after this transition has completed.
      result = esp_camera_set_psram_mode(true);
      if (result != ESP_OK) {
        ESP_LOGW("AIOT.Camera", "PSRAM DMA unavailable; restoring normal capture");
        (void)esp_camera_deinit();
        (void)esp_camera_set_psram_mode(false);
        result = esp_camera_init(&config);
      }
    }
#endif
    cameraInitialized = result == ESP_OK;
    if (cameraInitialized) {
      sensor_t* sensor = esp_camera_sensor_get();
      if (!sensor) result = ESP_ERR_NOT_SUPPORTED;
      else {
        gc2145_ = sensor->id.PID == GC2145_PID;
        if (sensor->set_vflip && sensor->set_vflip(sensor, CAMERA_VFLIP) != 0) result = ESP_FAIL;
        if (sensor->set_hmirror && sensor->set_hmirror(sensor, CAMERA_HMIRROR) != 0) result = ESP_FAIL;
        // GC2145 starts with its capture engine held in reset. Only this sensor
        // uses these page/restart registers; never apply them to an OV2640.
        if (gc2145_ && sensor->set_reg && result == ESP_OK) {
          if (sensor->set_reg(sensor, 0xfe, 0xff, 0x00) != 0) result = ESP_FAIL;
          vTaskDelay(pdMS_TO_TICKS(20));
          if (sensor->set_reg(sensor, 0xfe, 0xff, 0x10) != 0) result = ESP_FAIL;
          vTaskDelay(pdMS_TO_TICKS(20));
        }
      }
      if (result == ESP_OK &&
          aiotCreateBackgroundTask(workerEntry, "aiot_camera", 8192, this, 1, &worker_) != pdPASS)
        result = ESP_ERR_NO_MEM;
    }
  }
  if (result != ESP_OK) {
    if (cameraInitialized) esp_camera_deinit();
    if (requests_) vQueueDelete(requests_);
    if (results_) vQueueDelete(results_);
    requests_ = results_ = nullptr;
    worker_ = nullptr;
    i2c_del_master_bus(bus);
    sccbBus_ = nullptr;
  }
  return result;
}

bool CameraCapture::request(uint32_t epoch) {
  if (!worker_ || busy_) return false;
  if (xQueueSend(requests_, &epoch, 0) != pdTRUE) return false;
  busy_ = true;
  return true;
}

bool CameraCapture::take(CameraImage& image) {
  if (!results_ || xQueueReceive(results_, &image, 0) != pdTRUE) return false;
  busy_ = false;
  // Queue result is now owned by the loop thread.
  return true;
}

void CameraCapture::workerEntry(void* context) { static_cast<CameraCapture*>(context)->workerLoop(); }

void CameraCapture::workerLoop() {
  for (;;) {
    uint32_t epoch = 0;
    if (xQueueReceive(requests_, &epoch, portMAX_DELAY) != pdTRUE) continue;
    CameraImage image = capture(epoch);
    // One outstanding request: the loop must consume/release the result before
    // asking for another frame, including periodic LCD previews.
    xQueueSend(results_, &image, portMAX_DELAY);
  }
}

CameraImage CameraCapture::capture(uint32_t epoch) {
  CameraImage image;
  image.epoch = epoch;
  // Discard pre-trigger buffers, then obtain a fresh frame. Only
  // this task touches driver framebuffers; potentially blocking fb_get stays here.
  for (size_t i = 0; i < CAMERA_FB_COUNT; ++i) {
    camera_fb_t* old = esp_camera_fb_get();
    if (!old) { image.result = ESP_ERR_TIMEOUT; return image; }
    esp_camera_fb_return(old);
  }
  camera_fb_t* frame = esp_camera_fb_get();
  if (!frame) { image.result = ESP_ERR_TIMEOUT; return image; }
  if (!frame->buf || !frame->len || !frame->width || !frame->height ||
           frame->width > UINT16_MAX || frame->height > UINT16_MAX || frame->len > UINT32_MAX)
    image.result = ESP_ERR_INVALID_SIZE;
  else {
    image.width = uint16_t(frame->width);
    image.height = uint16_t(frame->height);
    if (frame->format == PIXFORMAT_JPEG) {
      image.data = static_cast<uint8_t*>(heap_caps_malloc(frame->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!image.data) image.result = ESP_ERR_NO_MEM;
      else { memcpy(image.data, frame->buf, frame->len); image.length = frame->len; }
    } else if (frame->format == PIXFORMAT_RGB565) {
      const size_t rowBytes = size_t(frame->width) * 2;
      if (frame->height > SIZE_MAX / rowBytes || frame->len < rowBytes * frame->height)
        image.result = ESP_ERR_INVALID_SIZE;
      else {
        // Preserve the sensor's original RGB565 byte order for frame2jpg. The
        // first clean row replaces GC2145's unstable top rows, as in the
        // original project's camera path; other sensors are left untouched.
        const size_t maskRows = CAMERA_MASK_TOP_ROWS;
        if (gc2145_ && maskRows && maskRows < frame->height) {
          const uint8_t* clean = frame->buf + maskRows * rowBytes;
          for (size_t row = 0; row < maskRows; ++row)
            memcpy(frame->buf + row * rowBytes, clean, rowBytes);
        }
        if (!frame2jpg(frame, CAMERA_JPEG_QUALITY, &image.data, &image.length)) image.result = ESP_FAIL;
      }
    } else image.result = ESP_ERR_NOT_SUPPORTED;
  }
  esp_camera_fb_return(frame);
  if (image.result == ESP_OK && (!image.data || !image.length || image.length > UINT32_MAX))
    image.result = ESP_ERR_INVALID_SIZE;
  if (image.result != ESP_OK) { free(image.data); image.data = nullptr; image.length = 0; }
  return image;
}

#else
esp_err_t CameraCapture::begin() { return ESP_ERR_NOT_SUPPORTED; }
bool CameraCapture::request(uint32_t) { return false; }
bool CameraCapture::take(CameraImage&) { return false; }
#endif

void CameraCapture::release(CameraImage& image) {
  free(image.data);
  image = CameraImage{};
}
