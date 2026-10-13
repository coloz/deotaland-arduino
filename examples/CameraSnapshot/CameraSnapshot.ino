/*
 * Real camera -> JPEG -> App. Default: ESP32-S3 + GC2145 + PSRAM.
 * Check camera_pins.h and enable PSRAM. The library bundles NimBLE with CoC;
 * an ordinary compatible official ESP32 Arduino Core is sufficient.
 * Connect a compatible App and open its separate IMAGE channel (PSM 0x0082).
 *
 * Triggers: camera0=true from App/Agent, serial 'c', or GPIO3 active-HIGH button.
 * Serial 'p' asks the Agent to take/analyze a picture by invoking camera0;
 * '?' prints diagnostics. Optional automatic text prompts default to disabled.
 * Capture/encode runs in a FreeRTOS worker. Deotaland calls stay in loop().
 * JPEG memory is retained across backpressure, then freed after local END or
 * failure. Local completion does not prove that the App received the image.
 */
#include <Arduino.h>
#include <Deotaland.h>
#include <DeotalandImageUpload.h>
#include "CameraCapture.h"
#include "camera_pins.h"

namespace {
CameraCapture camera;
DeotalandImageUpload upload;
CameraImage sending;
#if DEOTALAND_CAMERA_AVAILABLE
agent_link_io_desc_t cameraEndpoint = {};
esp_err_t beginError = ESP_ERR_INVALID_STATE;
#endif
esp_err_t supportError = ESP_ERR_INVALID_STATE;
esp_err_t cameraError = ESP_ERR_INVALID_STATE;
bool started = false;
bool captureRequested = false;
bool workflowBusy = false;
bool connectionChanged = false;
bool captureExpired = false;
bool promptRequested = false;
bool diagnosticRequested = false;
uint32_t connectionEpoch = 0;
uint32_t requestEpoch = 0;
uint32_t requestAt = 0;
uint32_t captureAt = 0;
uint32_t lastDiagnostic = 0;
#if CAMERA_AUTO_PROMPT_INTERVAL_MS > 0
uint32_t lastAutoPrompt = 0;
#endif
uint32_t lastPromptAttempt = 0;
#if DEOTALAND_CAMERA_AVAILABLE
bool buttonRaw = false;
bool buttonStable = false;
uint32_t buttonChangedAt = 0;
#endif

void scheduleCapture() {
  // All triggers and callbacks run on loop's thread. Coalesce repeated requests.
  if (workflowBusy || captureRequested) return;
  captureRequested = true;
  requestEpoch = connectionEpoch;
  requestAt = millis();
}

#if DEOTALAND_CAMERA_AVAILABLE
void actuate(const char*, const uint8_t* args, size_t length, void*) {
  if (args && length == 1 && args[0] == 1) scheduleCapture();
}

void onState(agent_state_t state, void*) {
  if (state != AGENT_STATE_READY) {
    // Even disconnect+READY within one poll invalidates the previous JPEG.
    ++connectionEpoch;
    captureRequested = false;
    promptRequested = false;
    connectionChanged = true;
  }
}
#endif

void reportError(const char* operation, esp_err_t result) {
  Serial.print(operation); Serial.print(": ");
  Serial.println(Deotaland.errorName(result));
  Serial.println(Deotaland.errorHint(result));
}

void printDiagnostic() {
  Serial.println("CameraSnapshot: c=capture, p=ask Agent, ?=diagnostics");
#if !DEOTALAND_CAMERA_AVAILABLE
  Serial.println("Camera driver unavailable: use ESP32/S2/S3 with esp_camera and img_converters.");
#else
  if (beginError != ESP_OK) reportError("BLE begin", beginError);
  else if (supportError != ESP_OK) {
    reportError("IMAGE support", supportError);
    Serial.println("Check the library's bundled NimBLE backend and a BLE-capable target; see docs/BLE-COC.zh-CN.md.");
  } else if (cameraError != ESP_OK) {
    reportError("camera init", cameraError);
    Serial.println("Check GC2145 wiring in camera_pins.h, sensor model, and enabled PSRAM.");
  } else {
    Serial.print("Link: "); Serial.println(Deotaland.stateName(Deotaland.state()));
    Serial.println("Camera ready. App must also open IMAGE CoC; END acceptance is local only.");
  }
#endif
}

void pollButton(uint32_t now) {
#if DEOTALAND_CAMERA_AVAILABLE
  if (CAPTURE_BUTTON_PIN < 0) return;
  const bool pressed = digitalRead(CAPTURE_BUTTON_PIN) == HIGH;
  if (pressed != buttonRaw) { buttonRaw = pressed; buttonChangedAt = now; }
  if (pressed != buttonStable && uint32_t(now - buttonChangedAt) >= CAMERA_BUTTON_DEBOUNCE_MS) {
    buttonStable = pressed;
    if (pressed) scheduleCapture();
  }
#else
  (void)now;
#endif
}

void pollPrompt(uint32_t now) {
#if CAMERA_AUTO_PROMPT_INTERVAL_MS > 0
  if (uint32_t(now - lastAutoPrompt) >= CAMERA_AUTO_PROMPT_INTERVAL_MS) {
    lastAutoPrompt = now;
    if (started && cameraError == ESP_OK && Deotaland.isReady()) promptRequested = true;
  }
#endif
  if (!promptRequested || uint32_t(now - lastPromptAttempt) < 100) return;
  lastPromptAttempt = now;
  if (!started || cameraError != ESP_OK || !Deotaland.isReady()) {
    promptRequested = false;
    Serial.println("Prompt needs a ready link and camera.");
    return;
  }
  const esp_err_t result = Deotaland.pushPrompt(CAMERA_AUTO_PROMPT_TEXT);
  if (Deotaland.isRetryable(result)) return;
  promptRequested = false;
  if (result != ESP_OK) reportError("prompt", result);
  else Serial.println("Prompt queued; Agent must invoke camera0=true for a picture.");
}

void pollCapture(uint32_t now) {
  if (connectionChanged) {
    connectionChanged = false;
    upload.cancel(); // Clears its borrowed data before we free the old JPEG.
    CameraCapture::release(sending);
    if (camera.busy()) captureExpired = true;
  }
  upload.poll(now); // One bounded open/write/END operation per iteration.
  if (sending.data && !upload.busy()) {
    if (upload.state() == DeotalandImageUpload::State::Completed)
      Serial.println("JPEG queued with END locally; App receipt is not acknowledged here.");
    else reportError("image upload", upload.result());
    CameraCapture::release(sending);
  }

  // A timed-out worker is allowed to finish safely. Its eventual JPEG is freed,
  // and another capture cannot start while it still owns a driver framebuffer.
  if (camera.busy() && !captureExpired &&
      uint32_t(now - captureAt) >= CAMERA_CAPTURE_TIMEOUT_MS) {
    captureExpired = true;
    reportError("camera capture", ESP_ERR_TIMEOUT);
  }
  CameraImage captured;
  if (camera.take(captured)) {
    if (captured.result != ESP_OK) reportError("camera capture", captured.result);
    else if (!captureExpired && captured.epoch == connectionEpoch && Deotaland.isReady()) {
      agent_stream_opts_t opts = {};
      opts.encoding = AGENT_ENC_JPEG;
      opts.width = captured.width;
      opts.height = captured.height;
      opts.total_bytes = static_cast<uint32_t>(captured.length);
      const esp_err_t result = upload.start(opts, captured.data, captured.length, now,
                                            CAMERA_UPLOAD_TIMEOUT_MS);
      if (result == ESP_OK) {
        sending = captured;
        captured.data = nullptr; // Ownership remains here until upload releases its borrow.
        Serial.printf("JPEG %ux%u, %u bytes: upload started\n",
                      static_cast<unsigned>(sending.width), static_cast<unsigned>(sending.height),
                      static_cast<unsigned>(sending.length));
      } else reportError("image start", result);
    }
    CameraCapture::release(captured); // Includes stale disconnect/timeout results.
  }

  if (!captureRequested) return;
  if (!started || supportError != ESP_OK || cameraError != ESP_OK ||
      requestEpoch != connectionEpoch || !Deotaland.isReady()) {
    captureRequested = false;
    Serial.println("Capture unavailable: check diagnostics and connect the App first.");
    return;
  }
  if (uint32_t(now - requestAt) >= CAMERA_UPLOAD_TIMEOUT_MS) {
    captureRequested = false;
    reportError("waiting for IMAGE", ESP_ERR_TIMEOUT);
    return;
  }
  if (camera.busy() || upload.busy() || sending.data) return;
  agent_stream_status_t status = {};
  const esp_err_t result = Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status);
  if (result != ESP_OK) {
    captureRequested = false;
    reportError("IMAGE status", result);
    return;
  }
  // closeStream succeeds before all bytes drain. Wait before the next image;
  // also avoid borrowing a stream opened by a different part of the sketch.
  if (status.state == AGENT_STREAM_DRAINING || status.state == AGENT_STREAM_OPEN) return;
  captureRequested = false;
  captureExpired = false;
  captureAt = now;
  if (!camera.request(connectionEpoch)) reportError("camera request", ESP_ERR_INVALID_STATE);
}
} // namespace

void setup() {
  Serial.begin(115200);
#if DEOTALAND_CAMERA_AVAILABLE
  if (CAPTURE_BUTTON_PIN >= 0) pinMode(CAPTURE_BUTTON_PIN, INPUT_PULLDOWN);
  cameraEndpoint.id = "camera0";
  cameraEndpoint.dir = AGENT_IO_OUT;
  cameraEndpoint.kind = "camera.capture";
  cameraEndpoint.value = AGENT_VAL_BOOL;
  cameraEndpoint.desc = "Set true to capture a fresh camera image and upload JPEG to the app";
  cameraEndpoint.display_name = "Camera Snapshot";
  beginError = Deotaland.registerIo(cameraEndpoint, actuate);
  if (beginError == ESP_OK)
    beginError = Deotaland.begin("Deotaland-Camera", AGENT_CAP_CAMERA, nullptr, onState);
  started = beginError == ESP_OK;
  if (started) {
    agent_stream_status_t status = {};
    supportError = Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status);
    // Skip framebuffers/tasks when the active backend cannot upload images.
    if (supportError == ESP_OK) cameraError = camera.begin();
  }
#endif
  lastDiagnostic = millis();
#if CAMERA_AUTO_PROMPT_INTERVAL_MS > 0
  lastAutoPrompt = lastDiagnostic;
#endif
  printDiagnostic();
}

void loop() {
  if (started) Deotaland.poll();
  const uint32_t now = millis();
  for (uint8_t count = 0; count < 8 && Serial.available(); ++count) {
    const int command = Serial.read();
    if (command == 'c' || command == 'C') scheduleCapture();
    else if (command == 'p' || command == 'P') promptRequested = true;
    else if (command == '?') diagnosticRequested = true;
  }
  pollButton(now);
  pollCapture(now);
  pollPrompt(now);
  workflowBusy = captureRequested || camera.busy() || upload.busy() || sending.data;
  if ((diagnosticRequested && uint32_t(now - lastDiagnostic) >= 500) ||
      uint32_t(now - lastDiagnostic) >= 10000) {
    diagnosticRequested = false;
    lastDiagnostic = now;
    printDiagnostic();
  }
  // Let the idle task and background capture run on dual-core ESP32 too.
  // A taskYIELD alone would not let lower-priority idle tasks feed their WDT.
  delay(1);
}
