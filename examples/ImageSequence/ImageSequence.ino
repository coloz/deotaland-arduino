/* Three paced RGB565 test cards, demonstrating bounded memory and cancellation.
 * Target: ESP32 BLE; no camera, PSRAM, display, or filesystem is needed.
 * This is a finite still-image sequence, not video or real camera capture.
 * App: subscribe 0xFFC4, open IMAGE PSM 0x0082. camera0=true starts three cards;
 * camera0=false cancels. Serial b=start, x=cancel, ?=status at 115200 baud.
 * Frames use big-endian RGB565. The buffer is never modified while borrowed.
 * Each frame waits for local transport draining before the next one starts.
 */
#include <Deotaland.h>

namespace {
constexpr uint16_t kWidth = 64;
constexpr uint16_t kHeight = 48;
constexpr uint8_t kFrameCount = 3;
constexpr uint32_t kFrameGapMs = 2000;
constexpr uint32_t kChannelWaitMs = 30000;
uint8_t pixels[size_t(kWidth) * kHeight * 2];
DeotalandImageUpload upload;
agent_link_io_desc_t cameraEndpoint = {};
esp_err_t beginError = ESP_ERR_INVALID_STATE;
esp_err_t supportError = ESP_ERR_INVALID_STATE;
bool started = false;
bool beginRequested = false;
bool cancelRequested = false;
bool sequenceActive = false;
bool frameActive = false;
bool reportRequested = false;
uint8_t completedFrames = 0;
uint32_t waitStartedAt = 0;
uint32_t nextFrameAt = 0;
uint32_t lastProgressAt = 0;
uint32_t lastReportAt = 0;

const char* streamState(agent_stream_state_t state) {
  switch (state) {
    case AGENT_STREAM_UNAVAILABLE: return "UNAVAILABLE";
    case AGENT_STREAM_IDLE: return "IDLE";
    case AGENT_STREAM_OPEN: return "OPEN";
    case AGENT_STREAM_DRAINING: return "DRAINING";
    case AGENT_STREAM_COMPLETED: return "COMPLETED";
    case AGENT_STREAM_FAILED: return "FAILED";
  }
  return "UNKNOWN";
}

void onState(agent_state_t state, void*) {
  if (state != AGENT_STATE_READY) {
    beginRequested = false;
    cancelRequested = true; // Handles/data never carry over to a new connection.
  }
}

void onCamera(const char*, const uint8_t* args, size_t bytes, void*) {
  // Only schedule work here; callback returns to poll without rendering/waiting.
  if (!args || bytes != 1) return;
  if (args[0] == 1) beginRequested = true;
  else if (args[0] == 0) cancelRequested = true;
}

void makeFrame(uint8_t index) {
  // 6144 bytes of reusable RAM. Do not call while upload.busy() is true.
  for (uint16_t y = 0; y < kHeight; ++y) {
    for (uint16_t x = 0; x < kWidth; ++x) {
      const uint16_t red = (x + index * 10) % kWidth * 31 / (kWidth - 1);
      const uint16_t green = y * 63 / (kHeight - 1);
      const uint16_t blue = ((x / 8 + y / 8 + index) & 1) ? 31 : 0;
      const uint16_t color = uint16_t((red << 11) | (green << 5) | blue);
      const size_t offset = (size_t(y) * kWidth + x) * 2;
      pixels[offset] = uint8_t(color >> 8);
      pixels[offset + 1] = uint8_t(color);
    }
  }
}

void report() {
  Serial.println("ImageSequence: b=3 test frames, x=cancel, ?=status; camera0 true/false");
  Serial.print("begin: "); Serial.println(Deotaland.errorName(beginError));
  Serial.print("IMAGE support: "); Serial.println(Deotaland.errorName(supportError));
  Serial.printf("Control=%s, sequence=%u, completed=%u/%u, uploader=%u\n",
                Deotaland.stateName(Deotaland.state()), unsigned(sequenceActive),
                unsigned(completedFrames), unsigned(kFrameCount), unsigned(upload.state()));
  agent_stream_status_t status = {};
  const esp_err_t error = started
    ? Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status) : ESP_ERR_INVALID_STATE;
  Serial.printf("IMAGE status=%s, state=%s, queued=%u, last_error=%s\n",
                Deotaland.errorName(error), streamState(status.state),
                unsigned(status.queued_bytes), Deotaland.errorName(status.last_error));
  Serial.println("Synthetic still images only. Local completion is not an App delivery ACK.");
}

void service(uint32_t now) {
  if (cancelRequested) {
    cancelRequested = false;
    beginRequested = false;
    sequenceActive = false;
    upload.cancel(); // Immediately releases the borrowed source; still poll END cleanup.
  }
  upload.poll(now);
  if (frameActive && !upload.busy()) {
    frameActive = false;
    Serial.printf("Frame %u local result: %s\n", unsigned(completedFrames + 1),
                  Deotaland.errorName(upload.result()));
    if (sequenceActive && upload.result() == ESP_OK) {
      ++completedFrames;
      if (completedFrames == kFrameCount) {
        sequenceActive = false;
        Serial.println("Sequence complete locally.");
      } else {
        nextFrameAt = now + kFrameGapMs;
        waitStartedAt = now;
      }
    } else sequenceActive = false;
  }
  if (beginRequested) {
    beginRequested = false;
    if (sequenceActive || upload.busy()) Serial.println("Sequence already active; x cancels.");
    else if (!started || supportError != ESP_OK || !Deotaland.isReady())
      Serial.println("Connect the App first; use ? to check CoC support.");
    else {
      completedFrames = 0;
      sequenceActive = true;
      waitStartedAt = now;
      nextFrameAt = now;
    }
  }
  if (upload.busy() && uint32_t(now - lastProgressAt) >= 1000) {
    lastProgressAt = now;
    Serial.printf("Frame %u accepted: %u/%u bytes\n", unsigned(completedFrames + 1),
                  unsigned(upload.acceptedBytes()), unsigned(sizeof(pixels)));
  }
  if (!sequenceActive || upload.busy() || int32_t(now - nextFrameAt) < 0) return;
  if (!Deotaland.isReady() || uint32_t(now - waitStartedAt) >= kChannelWaitMs) {
    sequenceActive = false;
    Serial.println("Sequence stopped: link unavailable or IMAGE wait timed out.");
    return;
  }
  agent_stream_status_t status = {};
  const esp_err_t statusError = Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status);
  if (statusError != ESP_OK) {
    sequenceActive = false;
    Serial.print("IMAGE status: "); Serial.println(Deotaland.errorName(statusError));
    return;
  }
  if (sequenceActive && completedFrames > 0 && status.state == AGENT_STREAM_FAILED) {
    // END acceptance precedes background transmission. Stop automatically
    // advancing when the previous frame subsequently failed in the backend.
    // A later manual burst resets completedFrames and may retry from FAILED.
    sequenceActive = false;
    Serial.print("Sequence stopped after background IMAGE failure: ");
    Serial.println(Deotaland.errorName(status.last_error));
    return;
  }
  if (status.state == AGENT_STREAM_UNAVAILABLE || status.state == AGENT_STREAM_OPEN ||
      status.state == AGENT_STREAM_DRAINING) return;
  makeFrame(completedFrames);
  agent_stream_opts_t options = {};
  options.name = "rgb565-test-sequence";
  options.encoding = AGENT_ENC_RAW;
  options.width = kWidth;
  options.height = kHeight;
  options.total_bytes = sizeof(pixels);
  const esp_err_t error = upload.start(options, pixels, sizeof(pixels), now);
  frameActive = error == ESP_OK;
  if (!frameActive) {
    sequenceActive = false;
    Serial.print("Frame start: "); Serial.println(Deotaland.errorName(error));
  }
}
} // namespace

void setup() {
  Serial.begin(115200);
  cameraEndpoint.id = "camera0";
  cameraEndpoint.dir = AGENT_IO_OUT;
  cameraEndpoint.kind = "camera.capture";
  cameraEndpoint.value = AGENT_VAL_BOOL;
  cameraEndpoint.display_name = "Camera Snapshot";
  cameraEndpoint.desc = "True sends three synthetic RGB565 still images; false cancels. No real camera.";
  beginError = Deotaland.registerIo(cameraEndpoint, onCamera);
  if (beginError == ESP_OK)
    beginError = Deotaland.begin("Deotaland-ImageSequence", AGENT_CAP_CAMERA, nullptr, onState);
  started = beginError == ESP_OK;
  if (started) {
    agent_stream_status_t status = {};
    supportError = Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status);
  }
  report();
}

void loop() {
  if (started) Deotaland.poll();
  const uint32_t now = millis();
  for (uint8_t count = 0; count < 8 && Serial.available(); ++count) {
    const int command = Serial.read();
    if (command == 'b' || command == 'B') beginRequested = true;
    else if (command == 'x' || command == 'X') cancelRequested = true;
    else if (command == '?') reportRequested = true;
  }
  service(now);
  if (reportRequested && uint32_t(now - lastReportAt) >= 500) {
    reportRequested = false;
    lastReportAt = now;
    report();
  }
  delay(1);
}
