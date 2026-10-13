#include "BoardDisplay.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "sdkconfig.h"
#endif

#if defined(ARDUINO_ARCH_ESP32) && CONFIG_IDF_TARGET_ESP32S3
#include "BoardTasks.h"
#include <atomic>
#include <cstring>
#include <new>
#include <cstdio>
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"

namespace {
// AIOT-Basic board.json wiring. LCD and SD must use different SPI hosts.
constexpr spi_host_device_t kHost = SPI2_HOST;
constexpr int kSclk = 21, kMosi = 47, kDc = 43, kCs = 44;
constexpr int kWidth = 240, kHeight = 240, kBodyY = 48;
constexpr int kRows = 8;
constexpr size_t kStripeBytes = size_t(kWidth) * kRows * 2;
constexpr size_t kMaxJpegBytes = 128 * 1024;
constexpr size_t kMaxDecodedBytes = 640 * 480 * 2;
// Match boards/common/st7789_lcd's default orientation. Calibrate these if a
// different panel revision needs an offset or red/blue channel selection.
constexpr int kGapX = 0, kGapY = 0;
constexpr bool kBgr = false, kInvert = true;

// Original 5x7 row-bit font authored for this example (same MIT license as the
// library). Lowercase is shown as uppercase; unsupported glyphs become '?'.
constexpr char kGlyphChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_:!?/+=()%";
constexpr uint8_t kGlyphRows[][7] = {
  {0,0,0,0,0,0,0},
  {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
  {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
  {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
  {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
  {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
  {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
  {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
  {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
  {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
  {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
  {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
  {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
  {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
  {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
  {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
  {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
  {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
  {0,0,0,0,0,6,6}, {0,0,0,31,0,0,0},
  {0,0,0,0,0,0,31}, {0,6,6,0,6,6,0},
  {4,4,4,4,4,0,4}, {14,17,1,2,4,0,4},
  {1,2,2,4,8,8,16}, {0,4,4,31,4,4,0},
  {0,0,31,0,31,0,0}, {2,4,8,8,8,4,2},
  {8,4,2,2,2,4,8}, {17,18,2,4,8,9,17},
};
static_assert(sizeof(kGlyphRows) / sizeof(kGlyphRows[0]) == sizeof(kGlyphChars) - 1,
              "Font and character map must agree");

const uint8_t* glyph(char ch) {
  if (ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
  const char* found = std::strchr(kGlyphChars, ch);
  if (!found) found = std::strchr(kGlyphChars, '?');
  return kGlyphRows[found - kGlyphChars];
}

void readable(char* dst, size_t capacity, const char* src, bool explainUtf8) {
  if (!capacity) return;
  if (!src) src = "";
  size_t used = 0;
  bool hasUtf8 = false;
  // Bound both scanning and copying when text comes from an App callback.
  for (size_t i = 0; i < capacity * 4 && src[i]; ++i)
    if (uint8_t(src[i]) >= 128) { hasUtf8 = true; break; }
  if (hasUtf8 && explainUtf8) {
    constexpr char notice[] = "UTF8: SEE SERIAL/APP\n";
    for (size_t i = 0; notice[i] && used + 1 < capacity; ++i) dst[used++] = notice[i];
  }
  for (size_t i = 0; src[i] && i < capacity * 4 && used + 1 < capacity; ++i) {
    const uint8_t ch = uint8_t(src[i]);
    if (ch >= 128) {
      dst[used++] = '?';
      // One replacement for one UTF-8 code point, even if its glyph is absent.
      while (i + 1 < capacity * 4 && (uint8_t(src[i + 1]) & 0xc0) == 0x80) ++i;
    } else if (ch == '\n') dst[used++] = '\n';
    else if (ch == '\t') dst[used++] = ' ';
    else if (ch >= 32 && ch <= 126) dst[used++] = char(ch);
  }
  dst[used] = 0;
}

// Parse only bounded baseline JPEG headers before allocating a decoder output.
// This also avoids calling an unbounded JPEG-to-RGB convenience API.
bool jpegDimensions(const uint8_t* jpeg, size_t length, uint16_t& width, uint16_t& height) {
  if (!jpeg || length < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return false;
  size_t offset = 2;
  while (offset + 4 <= length) {
    if (jpeg[offset++] != 0xff) return false;
    while (offset < length && jpeg[offset] == 0xff) ++offset;
    if (offset >= length) return false;
    const uint8_t marker = jpeg[offset++];
    if (marker == 0xd9 || marker == 0xda || marker == 0) return false;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) continue;
    if (offset + 2 > length) return false;
    const size_t segment = size_t(jpeg[offset]) * 256 + jpeg[offset + 1];
    if (segment < 2 || segment > length - offset) return false;
    if (marker == 0xc0) {
      if (segment < 8 || jpeg[offset + 2] != 8) return false;
      height = uint16_t(uint16_t(jpeg[offset + 3]) * 256 + jpeg[offset + 4]);
      width = uint16_t(uint16_t(jpeg[offset + 5]) * 256 + jpeg[offset + 6]);
      return width && height && width <= 640 && height <= 480;
    }
    offset += segment;
  }
  return false;
}

void putPixel(uint8_t* stripe, int firstRow, int rows, int x, int y, uint16_t color) {
  if (x < 0 || x >= kWidth || y < firstRow || y >= firstRow + rows) return;
  const size_t index = (size_t(y - firstRow) * kWidth + x) * 2;
  stripe[index] = uint8_t(color >> 8);
  stripe[index + 1] = uint8_t(color);
}

void drawText(uint8_t* stripe, int firstRow, int rows, const char* text,
              int x, int y, int scale, int maxLines, uint16_t color) {
  const int startX = x;
  int line = 0;
  for (size_t i = 0; text[i] && line < maxLines; ++i) {
    if (text[i] == '\n' || x + 5 * scale > kWidth - 4) {
      x = startX;
      y += 8 * scale;
      if (++line >= maxLines) break;
      if (text[i] == '\n') continue;
    }
    if (y < firstRow + rows && y + 7 * scale > firstRow) {
      const uint8_t* shape = glyph(text[i]);
      for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 5; ++gx) {
        if (!(shape[gy] & (1u << (4 - gx)))) continue;
        for (int dy = 0; dy < scale; ++dy) for (int dx = 0; dx < scale; ++dx)
          putPixel(stripe, firstRow, rows, x + gx * scale + dx, y + gy * scale + dy, color);
      }
    }
    x += 6 * scale;
  }
}
} // namespace

struct BoardDisplay::Impl {
  struct Job {
    char title[32] = {}, hint[40] = {}, text[224] = {};
    uint8_t* jpeg = nullptr;
    size_t jpegLength = 0;
    bool statusChanged = false;
    uint8_t bodyChanged = 0; // 0=unchanged, 1=text, 2=JPEG
  } pending;
  QueueHandle_t queue = nullptr;
  TaskHandle_t worker = nullptr;
  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_handle_t panel = nullptr;
  uint8_t* stripe = nullptr;
  std::atomic<bool> online{false};
  std::atomic<esp_err_t> error{ESP_OK};
  bool dirty = false;

  static bool colorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* context) {
    auto* self = static_cast<Impl*>(context);
    BaseType_t wake = pdFALSE;
    if (self->worker) vTaskNotifyGiveFromISR(self->worker, &wake);
    return wake == pdTRUE;
  }

  esp_err_t drawStripe(int y, int rows) {
    // A single DMA buffer is reused only after its completion callback. On a
    // timeout the task stops drawing and retains the buffer until reset.
    (void)ulTaskNotifyTake(pdTRUE, 0);
    esp_err_t result = esp_lcd_panel_draw_bitmap(panel, 0, y, kWidth, y + rows, stripe);
    if (result == ESP_OK && !ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)))
      result = ESP_ERR_TIMEOUT;
    return result;
  }

  static void workerEntry(void* context) { static_cast<Impl*>(context)->run(); }

  void run() {
    Job job;
    char title[32] = {}, hint[40] = {}, text[224] = {};
    uint8_t* rgb = nullptr;
    uint16_t imageWidth = 0, imageHeight = 0;
    for (;;) {
      if (xQueueReceive(queue, &job, portMAX_DELAY) != pdTRUE) continue;
      if (!online.load()) { heap_caps_free(job.jpeg); continue; }
      if (job.statusChanged) {
        std::memcpy(title, job.title, sizeof(title));
        std::memcpy(hint, job.hint, sizeof(hint));
      }
      if (job.bodyChanged) {
        heap_caps_free(rgb);
        rgb = nullptr;
        if (job.bodyChanged == 1) std::memcpy(text, job.text, sizeof(text));
        else {
          esp_err_t result = ESP_ERR_INVALID_ARG;
          if (jpegDimensions(job.jpeg, job.jpegLength, imageWidth, imageHeight)) {
            const size_t bytes = size_t(imageWidth) * imageHeight * 2;
            if (bytes <= kMaxDecodedBytes)
              rgb = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            result = rgb ? ESP_OK : ESP_ERR_NO_MEM;
            if (rgb) {
              esp_jpeg_image_cfg_t config = {};
              config.indata = job.jpeg;
              config.indata_size = uint32_t(job.jpegLength);
              config.outbuf = rgb;
              config.outbuf_size = uint32_t(bytes);
              config.out_format = JPEG_IMAGE_FORMAT_RGB565;
              config.out_scale = JPEG_IMAGE_SCALE_0;
              // ESP32-S3 ROM decoder: swap=1 gives ST7789 high-byte-first RGB565.
              config.flags.swap_color_bytes = 1;
              esp_jpeg_image_output_t decoded = {};
              result = esp_jpeg_decode(&config, &decoded);
              if (result == ESP_OK && (decoded.width != imageWidth || decoded.height != imageHeight ||
                                      decoded.output_len != bytes)) result = ESP_ERR_INVALID_SIZE;
            }
          }
          error.store(result);
          if (result != ESP_OK) {
            heap_caps_free(rgb);
            rgb = nullptr;
            std::snprintf(text, sizeof(text), "JPEG DISPLAY ERROR\n%s", esp_err_to_name(result));
          }
        }
      }
      heap_caps_free(job.jpeg); // Uploaded source and display copy have independent ownership.
      int drawWidth = 0, drawHeight = 0, left = 0, top = 0;
      if (rgb) {
        const int areaHeight = kHeight - kBodyY;
        drawWidth = kWidth;
        drawHeight = int(uint32_t(imageHeight) * drawWidth / imageWidth);
        if (drawHeight > areaHeight) {
          drawHeight = areaHeight;
          drawWidth = int(uint32_t(imageWidth) * drawHeight / imageHeight);
        }
        if (drawWidth < 1) drawWidth = 1;
        if (drawHeight < 1) drawHeight = 1;
        left = (kWidth - drawWidth) / 2;
        top = kBodyY + (areaHeight - drawHeight) / 2;
      }
      for (int y = 0; y < kHeight; y += kRows) {
        const int rows = y + kRows <= kHeight ? kRows : kHeight - y;
        std::memset(stripe, 0, kStripeBytes);
        drawText(stripe, y, rows, title, 4, 4, 2, 1, 0x07ff);
        drawText(stripe, y, rows, hint, 4, 25, 1, 2, 0xffff);
        if (rgb) {
          for (int dy = 0; dy < rows; ++dy) {
            const int destY = y + dy;
            if (destY < top || destY >= top + drawHeight) continue;
            const uint32_t sourceY = uint32_t(destY - top) * imageHeight / drawHeight;
            for (int x = 0; x < drawWidth; ++x) {
              const uint32_t sourceX = uint32_t(x) * imageWidth / drawWidth;
              const size_t source = (size_t(sourceY) * imageWidth + sourceX) * 2;
              const size_t dest = (size_t(dy) * kWidth + left + x) * 2;
              stripe[dest] = rgb[source]; stripe[dest + 1] = rgb[source + 1];
            }
          }
        } else drawText(stripe, y, rows, text, 4, kBodyY, 2, 12, 0xffff);
        const esp_err_t result = drawStripe(y, rows);
        if (result != ESP_OK) {
          error.store(result);
          online.store(false);
          break;
        }
        // Background decoding/drawing never monopolizes the Arduino poll task.
        vTaskDelay(1);
      }
    }
  }
};

esp_err_t BoardDisplay::begin() {
  if (impl_) return ESP_ERR_INVALID_STATE;
  // The completion ISR accesses this context even when external memory is
  // unavailable. Keep it internal independently of the worker stack policy.
  void* storage = heap_caps_malloc(sizeof(Impl), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  auto* self = storage ? new (storage) Impl : nullptr;
  if (!self) return ESP_ERR_NO_MEM;
  bool busOwned = false;
  esp_err_t result = ESP_OK;
  self->queue = xQueueCreate(1, sizeof(Impl::Job));
  self->stripe = static_cast<uint8_t*>(heap_caps_malloc(kStripeBytes,
                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!self->queue || !self->stripe) result = ESP_ERR_NO_MEM;
  if (result == ESP_OK) {
    spi_bus_config_t bus = {};
    bus.sclk_io_num = kSclk; bus.mosi_io_num = kMosi; bus.miso_io_num = -1;
    bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
    bus.max_transfer_sz = int(kStripeBytes);
    result = spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO);
    // Reject a host already owned by another driver; different wiring cannot be shared blindly.
    busOwned = result == ESP_OK;
  }
  if (result == ESP_OK) {
    esp_lcd_panel_io_spi_config_t config = {};
    config.dc_gpio_num = kDc; config.cs_gpio_num = kCs;
    config.pclk_hz = 40 * 1000 * 1000; config.spi_mode = 0;
    config.lcd_cmd_bits = 8; config.lcd_param_bits = 8; config.trans_queue_depth = 2;
    result = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kHost), &config, &self->io);
  }
  if (result == ESP_OK) {
    esp_lcd_panel_io_callbacks_t callbacks = {};
    callbacks.on_color_trans_done = Impl::colorDone;
    result = esp_lcd_panel_io_register_event_callbacks(self->io, &callbacks, self);
  }
  if (result == ESP_OK) {
    esp_lcd_panel_dev_config_t config = {};
    config.reset_gpio_num = -1; // No LCD RESET/BL GPIO in AIOT-Basic board.json.
    config.rgb_ele_order = kBgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB;
    config.bits_per_pixel = 16;
    result = esp_lcd_new_panel_st7789(self->io, &config, &self->panel);
  }
  if (result == ESP_OK) result = esp_lcd_panel_reset(self->panel);
  if (result == ESP_OK) result = esp_lcd_panel_init(self->panel);
  if (result == ESP_OK) result = esp_lcd_panel_invert_color(self->panel, kInvert);
  if (result == ESP_OK) result = esp_lcd_panel_swap_xy(self->panel, false);
  if (result == ESP_OK) result = esp_lcd_panel_mirror(self->panel, false, false);
  if (result == ESP_OK) result = esp_lcd_panel_set_gap(self->panel, kGapX, kGapY);
  if (result == ESP_OK) result = esp_lcd_panel_disp_on_off(self->panel, true);
  if (result == ESP_OK) {
    self->online.store(true);
    if (aiotCreateBackgroundTask(Impl::workerEntry, "aiot_lcd", 6144, self, 1, &self->worker) != pdPASS)
      result = ESP_ERR_NO_MEM;
  }
  if (result != ESP_OK) {
    if (self->panel) esp_lcd_panel_del(self->panel);
    if (self->io) esp_lcd_panel_io_del(self->io);
    if (busOwned) spi_bus_free(kHost);
    if (self->queue) vQueueDelete(self->queue);
    heap_caps_free(self->stripe);
    self->~Impl();
    heap_caps_free(self);
    return result;
  }
  impl_ = self;
  showStatus("AIOT BASIC", "STARTING");
  showText("WAITING FOR APP");
  return ESP_OK;
}

bool BoardDisplay::ready() const { return impl_ && impl_->online.load(); }
esp_err_t BoardDisplay::lastError() const { return impl_ ? impl_->error.load() : ESP_ERR_INVALID_STATE; }

void BoardDisplay::showStatus(const char* title, const char* hint) {
  if (!ready()) return;
  readable(impl_->pending.title, sizeof(impl_->pending.title), title, false);
  readable(impl_->pending.hint, sizeof(impl_->pending.hint), hint, false);
  impl_->pending.statusChanged = true;
  impl_->dirty = true;
}

void BoardDisplay::showText(const char* utf8) {
  if (!ready()) return;
  readable(impl_->pending.text, sizeof(impl_->pending.text), utf8, true);
  // Latest requested body wins; a pending JPEG has not crossed to the task yet.
  heap_caps_free(impl_->pending.jpeg);
  impl_->pending.jpeg = nullptr;
  impl_->pending.jpegLength = 0;
  impl_->pending.bodyChanged = 1;
  impl_->dirty = true;
}

esp_err_t BoardDisplay::showJpeg(const uint8_t* data, size_t length) {
  if (!ready()) return ESP_ERR_INVALID_STATE;
  if (!data || !length) return ESP_ERR_INVALID_ARG;
  if (length > kMaxJpegBytes) return ESP_ERR_INVALID_SIZE;
  auto* copy = static_cast<uint8_t*>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!copy) return ESP_ERR_NO_MEM;
  std::memcpy(copy, data, length);
  heap_caps_free(impl_->pending.jpeg);
  impl_->pending.jpeg = copy;
  impl_->pending.jpegLength = length;
  impl_->pending.bodyChanged = 2;
  impl_->dirty = true;
  return ESP_OK;
}

void BoardDisplay::poll() {
  if (!ready() || !impl_->dirty) return;
  if (xQueueSend(impl_->queue, &impl_->pending, 0) != pdTRUE) return;
  impl_->pending.statusChanged = false;
  impl_->pending.bodyChanged = 0;
  impl_->pending.jpeg = nullptr; // Queue/task owns the display copy now.
  impl_->pending.jpegLength = 0;
  impl_->dirty = false;
}

#else
struct BoardDisplay::Impl {};
esp_err_t BoardDisplay::begin() { return ESP_ERR_NOT_SUPPORTED; }
bool BoardDisplay::ready() const { return false; }
esp_err_t BoardDisplay::lastError() const { return ESP_ERR_NOT_SUPPORTED; }
void BoardDisplay::showStatus(const char*, const char*) {}
void BoardDisplay::showText(const char*) {}
void BoardDisplay::poll() {}
esp_err_t BoardDisplay::showJpeg(const uint8_t*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
#endif
