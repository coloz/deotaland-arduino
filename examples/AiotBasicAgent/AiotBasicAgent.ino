/* Complete AIOT-Basic application: real microphone/I2S audio, camera JPEG, ST7789,
 * WS2812, BLE VOICE/AUDIO/TTS/IMAGE, I/O, prompts and optional App OTA.
 * Read README.md before selecting USB, PSRAM and partition options.
 * Serial: p/r=PTT, a/s=App audio capture/stop, c=photo upload, t=prompt,
 * l=local preview toggle, x=cancel media, o=abort OTA, v=confirm healthy OTA,
 * f=forget binding, b/e=SD record/stop, y=play latest, u=upload latest PCM,
 * d=refresh recording list, ?=diagnostics. Workers never call Deotaland.
 */
#include <Arduino.h>
#include <Deotaland.h>
#include <agent_link_transport.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include "BoardConfig.h"
#include "BoardAudio.h"
#include "BoardDisplay.h"
#include "BoardRecording.h"
#include "CameraCapture.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || !CONFIG_IDF_TARGET_ESP32S3
#error "AiotBasicAgent requires ESP32-S3. Select ESP32S3 Dev Module."
#endif
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
#error "Enable USB CDC On Boot: LCD uses GPIO43/44, which are UART0 pins."
#endif

namespace {
BoardAudio audio;
BoardDisplay screen;
BoardRecording recordings;
CameraCapture camera;
DeotalandImageUpload upload;
CameraImage sending;
agent_output_cb_t callbacks = {};
agent_link_io_desc_t cameraEndpoint = {}, volumeEndpoint = {}, uptimeEndpoint = {}, recordingEndpoint = {};
esp_err_t audioError = ESP_ERR_INVALID_STATE, screenError = ESP_ERR_INVALID_STATE;
esp_err_t cameraError = ESP_ERR_INVALID_STATE, beginError = ESP_ERR_INVALID_STATE;
esp_err_t voiceError = ESP_ERR_INVALID_STATE, captureError = ESP_ERR_INVALID_STATE;
esp_err_t imageError = ESP_ERR_INVALID_STATE, playbackError = ESP_ERR_INVALID_STATE;
esp_err_t otaError = ESP_ERR_NOT_SUPPORTED;
esp_err_t sdError = ESP_ERR_NOT_SUPPORTED;
bool started = false, resetPending = false, diagnosticPending = true;
bool promptPending = false, photoPending = false, previewEnabled = AIOT_PREVIEW_INTERVAL_MS > 0;
bool captureForUpload = false, captureExpired = false, otaWasBusy = false;
bool pttRequested = false, listenRequested = false, discardReply = false;
bool rawButton = false, stableButton = false, buttonMustRelease = false;
bool volumePending = false, ledPending = true;
bool textHold = false, captureEnabled = false, recordingRequested = false;
bool recordingOwned = false;
uint8_t volume = AIOT_INITIAL_VOLUME;
uint32_t ledRgb = 0x000010, epoch = 0, photoEpoch = 0, photoAt = 0, cameraAt = 0;
uint32_t lastPreview = 0, lastDiagnostic = 0, lastPrompt = 0;
#if AIOT_UPTIME_REPORT_INTERVAL_MS > 0
uint32_t lastReading = 0;
#endif
uint32_t buttonAt = 0, listenAt = 0, listenLimit = 0, lastOpen = 0, lastClose = 0;
uint32_t captureStarted = 0, droppedFrames = 0, uploadedImages = 0;
uint32_t captureDriverDrops = 0, pendingTimeoutDrops = 0;
uint32_t pendingReplacementDrops = 0, cancelPendingDrops = 0;
uint32_t lastText = 0, recordingStarted = 0;
uint64_t capturedBytes = 0, playedBytes = 0;
// Inspect actual captured PCM; successful DMA alone cannot prove a mic signal.
uint64_t micSamples = 0, micSquares = 0, micChanges = 0, micClipped = 0;
int64_t micSum = 0;
int16_t micMinimum = 32767, micMaximum = -32768, micPrevious = 0;
char textLog[515] = {}; // 512 text bytes plus CRLF and terminator.
size_t textLogOffset = 0, textLogLength = 0;
constexpr size_t kDiagnosticBytes = 4096;
char* diagnosticLog = nullptr; // Once allocated: PSRAM first, internal fallback.
size_t diagnosticLogOffset = 0, diagnosticLogLength = 0;
uint32_t diagnosticAllocateAt = 0;
bool diagnosticAllocationFailed = false;

enum class MicPhase { Idle, Open, Closing, Draining };
MicPhase micPhase = MicPhase::Idle;
agent_stream_t micKind = AGENT_STREAM_VOICE;
agent_stream_handle_t micHandle = nullptr;
bool micComplete = true, micFault = false;
uint8_t pcmPending[640]; // 20ms, PCM16 mono, 16 kHz
size_t pcmLength = 0, pcmOffset = 0;
uint32_t pcmAt = 0;
uint32_t micCloseAt = 0;
// One file reader feeds either the local speaker or one AUDIO stream.
enum class FileMode { None, Play, Upload };
enum class FilePhase { Opening, Sending, Closing, Draining };
FileMode fileMode = FileMode::None;
FilePhase filePhase = FilePhase::Opening;
agent_stream_handle_t fileHandle = nullptr;
uint8_t filePending[512];
size_t fileLength = 0, fileOffset = 0;
uint32_t fileAt = 0, fileAttempt = 0;
bool fileComplete = true;

bool retryable(esp_err_t error) { return Deotaland.isRetryable(error); }
void report(const char* operation, esp_err_t error) {
  if (!Serial) return;
  char line[192];
  const int count = std::snprintf(line, sizeof(line), "%s: %s\r\n", operation,
                                  Deotaland.errorName(error));
  // Short logs are optional: never wait for a monitor to consume them.
  if (count > 0 && size_t(count) < sizeof(line) && Serial.availableForWrite() >= count)
    Serial.write(reinterpret_cast<const uint8_t*>(line), size_t(count));
}
void discardPendingMic() {
  if (pcmLength > pcmOffset) ++cancelPendingDrops;
  pcmLength = pcmOffset = 0;
}
void consumeCaptureDrops() {
  const uint32_t lost = audio.takeCaptureDrops();
  if (!lost) return;
  droppedFrames += lost;
  captureDriverDrops += lost;
  if (micPhase == MicPhase::Open || micPhase == MicPhase::Closing) micComplete = false;
  if (recordingOwned && fileMode == FileMode::None && recordings.busy()) {
    recordings.stopRecording(false);
    report("microphone queue overflow during SD recording", ESP_ERR_NO_MEM);
  }
}

void requestPhoto() {
  if (photoPending || upload.busy() || sending.data || !started ||
      !Deotaland.isReady() || cameraError != ESP_OK || imageError != ESP_OK ||
      agent_link_ota_is_busy()) return;
  photoPending = true;
  photoEpoch = epoch;
  photoAt = millis();
}

void onCamera(const char*, const uint8_t* args, size_t length, void*) {
  if (!args || length != 1) return;
  if (args[0]) requestPhoto();
  else {
    photoPending = false;
    upload.cancel();
    if (camera.busy() && captureForUpload) captureExpired = true;
  }
}
void onVolume(const char*, const uint8_t* args, size_t length, void*) {
  if (!args || length != 2) return;
  const unsigned value = unsigned(args[0]) | (unsigned(args[1]) << 8);
  if (value <= 100) { volume = value; volumePending = true; }
}
void onLed(uint32_t rgb, void*) { ledRgb = rgb & 0x00ffffff; ledPending = true; }
void onText(const char* utf8, void*) {
  screen.showText(utf8);
  textHold = true;
  lastText = millis();
  // Preserve UTF-8 for Serial; LCD's built-in font has ASCII glyphs only.
  textLogLength = utf8 ? strnlen(utf8, sizeof(textLog) - 3) : 0;
  if (textLogLength) memcpy(textLog, utf8, textLogLength);
  if (textLogLength) { textLog[textLogLength++] = '\r'; textLog[textLogLength++] = '\n'; }
  textLog[textLogLength] = '\0';
  textLogOffset = 0;
}
void onAudioEnd(void*) {
  // END means all PCM was handed to the sink. Physical DMA continues draining.
  discardReply = false;
}
size_t playbackWrite(const uint8_t* pcm, size_t bytes, void*) {
  if (resetPending) return 0;
  if (discardReply || fileMode == FileMode::Play || agent_link_ota_is_busy() ||
      agent_link_ota_needs_reconnect()) return bytes;
  return audio.enqueuePlayback(pcm, bytes);
}
size_t playbackQueued(void*) {
  return fileMode == FileMode::Play || discardReply || agent_link_ota_is_busy() ||
         agent_link_ota_needs_reconnect() ? 0 : audio.playbackBytes();
}
void onListen(bool start, uint32_t maxMs, void*) {
  if (audioError != ESP_OK || captureError != ESP_OK || fileMode != FileMode::None ||
      agent_link_ota_is_busy() || agent_link_ota_needs_reconnect()) return;
  listenRequested = start;
  if (start) {
    pttRequested = false;
    listenAt = millis();
    listenLimit = maxMs && maxMs < AIOT_CAPTURE_LIMIT_MS ? maxMs : AIOT_CAPTURE_LIMIT_MS;
  }
}
void onState(agent_state_t state, void*) {
  if (state != AGENT_STATE_READY) {
    // A READY event later in the same poll cannot revive the old media session.
    ++epoch;
    resetPending = true;
    pttRequested = listenRequested = photoPending = promptPending = false;
    buttonMustRelease = true;
  }
}
void onStatus(const agent_link_status_t* status, void*) {
  if (!status) return;
  screen.showStatus(status->title, status->hint);
}
void onOta(const agent_ota_status_t* status, void*) {
  char hint[80];
  snprintf(hint, sizeof(hint), "%lu/%lu bytes, error %u",
           (unsigned long)status->received_bytes, (unsigned long)status->total_bytes,
           unsigned(status->error_code));
  screen.showStatus("OTA", hint);
  diagnosticPending = true;
}

void onRecording(const char*, const uint8_t* args, size_t length, void*) {
  if (args && length == 1) {
    recordingRequested = args[0] != 0 && !agent_link_ota_is_busy();
    if (!args[0]) { consumeCaptureDrops(); recordings.stopRecording(); }
  }
}
bool onCommand(uint16_t command, const uint8_t* payload, size_t length,
               uint8_t* response, size_t capacity, size_t* used, void*) {
  if (command != 0x04 || !recordings.ready()) return false;
  // Event capacity is control MTU minus 6; response overhead is 10 bytes.
  const size_t eventCapacity = Deotaland.maxPromptBytes();
  *used = min(capacity, eventCapacity > 4 ? eventCapacity - 4 : size_t(0));
  return recordings.handleList(payload, length, response, used) == ESP_OK;
}

void setCapture(bool enabled) {
  if (enabled != captureEnabled) {
    if (enabled) {
      micSamples = micSquares = micChanges = micClipped = 0; micSum = 0;
      micMinimum = 32767; micMaximum = -32768; micPrevious = 0;
    }
    audio.setCapture(enabled);
    captureEnabled = enabled;
  }
}

void stopMic(bool complete) {
  consumeCaptureDrops();
  if (micPhase == MicPhase::Open || micPhase == MicPhase::Closing)
    micComplete = micComplete && complete;
  if (micPhase == MicPhase::Open) {
    micPhase = MicPhase::Closing;
    micCloseAt = millis();
    lastClose = micCloseAt - 20;
  }
}
void stopFile() {
  if (fileMode == FileMode::None) return;
  recordings.closeReader();
  fileComplete = false;
  fileLength = fileOffset = 0;
  if (fileHandle) { filePhase = FilePhase::Closing; fileAt = millis(); }
  else fileMode = FileMode::None;
}
void cancelMedia() {
  pttRequested = listenRequested = photoPending = promptPending = false;
  stopMic(false);
  recordingRequested = false;
  recordings.stopRecording(false);
  stopFile();
  discardPendingMic();
  upload.cancel();
  if (camera.busy()) captureExpired = true;
  const bool replyQueued = started && Deotaland.playbackActive();
  if (fileMode != FileMode::Play) audio.cancelPlayback();
  // Consume the cancelled reply without requeueing its old PCM. END/disconnect
  // releases this flag; cancel cannot make the App start a new TTS session.
  discardReply = discardReply || replyQueued;
}
void resetMedia() {
  resetPending = false;
  upload.cancel();
  CameraCapture::release(sending);
  if (camera.busy()) captureExpired = true;
  // A locally started SD recording can continue while BLE reconnects.
  if (!recordings.recording()) setCapture(false);
  if (fileMode != FileMode::Play) audio.cancelPlayback();
  micPhase = MicPhase::Idle;
  micHandle = nullptr;
  discardPendingMic();
  micFault = false;
  discardReply = false;
  if (fileMode == FileMode::Upload) {
    recordings.closeReader();
    fileMode = FileMode::None;
    fileHandle = nullptr;
    fileLength = fileOffset = 0;
  }
}

void pollMic(uint32_t now) {
  if (listenRequested && uint32_t(now - listenAt) >= listenLimit) listenRequested = false;
  if (micPhase == MicPhase::Open && uint32_t(now - captureStarted) >= AIOT_CAPTURE_LIMIT_MS) {
    pttRequested = listenRequested = false;
    buttonMustRelease = true;
  }
  const bool wanted = (listenRequested || pttRequested) && Deotaland.isReady() &&
                      audio.captureReady() && fileMode == FileMode::None && !agent_link_ota_is_busy() &&
                      !agent_link_ota_needs_reconnect();
  const agent_stream_t desired = listenRequested ? AGENT_STREAM_AUDIO : AGENT_STREAM_VOICE;
  if (micPhase == MicPhase::Open && (!wanted || desired != micKind))
    stopMic(audio.captureReady() && Deotaland.isReady() && !agent_link_ota_is_busy());

  if (micPhase == MicPhase::Idle && wanted && !micFault && audioError == ESP_OK &&
      uint32_t(now - lastOpen) >= 100) {
    lastOpen = now;
    const esp_err_t support = desired == AGENT_STREAM_AUDIO ? captureError : voiceError;
    if (support != ESP_OK) { pttRequested = listenRequested = false; report("mic prepare", support); return; }
    agent_stream_opts_t opts = {};
    opts.name = "aiot-basic-microphone";
    const esp_err_t error = Deotaland.openStream(desired, &opts, &micHandle);
    if (retryable(error) || error == ESP_ERR_INVALID_STATE) return; // Peer may still open/subscribe.
    if (error != ESP_OK) { pttRequested = listenRequested = false; report("mic open", error); return; }
    micKind = desired;
    micPhase = MicPhase::Open;
    micComplete = true;
    pcmLength = pcmOffset = 0;
    captureStarted = now;
    screen.showText(desired == AGENT_STREAM_VOICE ? "Listening to Agent..." : "Recording for App...");
  }
  // Keep the last real frame until fully accepted. A bounded microphone queue
  // absorbs short congestion; a >200ms stall drops this frame and marks END
  // incomplete instead of busy-waiting or replaying it in a new session.
  if (micPhase == MicPhase::Open || micPhase == MicPhase::Closing) {
    if (pcmLength && uint32_t(now - pcmAt) >= 200) {
      pcmLength = pcmOffset = 0;
      micComplete = false;
      ++droppedFrames;
      ++pendingTimeoutDrops;
    }
    if (pcmLength) {
      const size_t bytes = min(size_t(512), pcmLength - pcmOffset);
      const esp_err_t error = Deotaland.writeStream(micHandle, pcmPending + pcmOffset, bytes);
      if (error == ESP_OK) {
        pcmOffset += bytes;
        capturedBytes += bytes;
        if (pcmOffset == pcmLength) pcmLength = pcmOffset = 0;
      } else if (!retryable(error)) {
        discardPendingMic();
        pttRequested = listenRequested = false;
        stopMic(false);
        report("mic write", error);
      }
    }
  }
  if (micPhase == MicPhase::Closing && !pcmLength && uint32_t(now - lastClose) >= 20) {
    consumeCaptureDrops();
    lastClose = now;
    const esp_err_t error = Deotaland.closeStream(micHandle, micComplete);
    if (!retryable(error)) {
      micHandle = nullptr;
      micPhase = error == ESP_OK ? MicPhase::Draining : MicPhase::Idle;
      micFault = error != ESP_OK; // Reconnect before reusing a failed END.
      report(micComplete ? "mic END accepted locally" : "mic incomplete END", error);
    }
  }
  if (micPhase == MicPhase::Closing && uint32_t(now - micCloseAt) >= 2000) {
    micPhase = MicPhase::Idle;
    micHandle = nullptr;
    micFault = true;
    pttRequested = listenRequested = false;
    discardPendingMic();
    report("mic END stalled; reconnect before another stream", ESP_ERR_TIMEOUT);
  }
  if (micPhase == MicPhase::Draining) {
    agent_stream_status_t status = {};
    if (Deotaland.streamStatus(micKind, &status) == ESP_OK &&
        status.state != AGENT_STREAM_DRAINING && status.state != AGENT_STREAM_OPEN) {
      micPhase = MicPhase::Idle;
      // END(false) intentionally reports FAILED. A locally accepted END still
      // retires the stream, so a cancelled utterance may start a new session.
    }
  }
}

void pollRecording(uint32_t now) {
  recordings.poll();
  consumeCaptureDrops();
  if (recordingOwned && !recordings.busy()) recordingOwned = false;
  if (recordings.recording() && !audio.captureReady()) {
    recordings.stopRecording(false);
    report("audio driver failed during SD recording", ESP_FAIL);
  }
  if (recordingRequested && !recordings.recording() && !recordings.busy() &&
      fileMode == FileMode::None && !agent_link_ota_is_busy() && audio.captureReady()) {
    recordingRequested = false; // One attempt; do not retry failed SD writes indefinitely.
    const esp_err_t error = recordings.startRecording();
    if (error == ESP_OK) {
      recordingStarted = now;
      recordingOwned = true;
      screen.showText("RECORDING TO SD. Serial e stops.");
    } else report("SD record start", error);
  }
  if (recordings.recording() && uint32_t(now - recordingStarted) >= AIOT_CAPTURE_LIMIT_MS)
    recordings.stopRecording();
  if (recordings.takeDrops()) report("SD recording queue overflow (partial WAV)", ESP_ERR_NO_MEM);
  const bool networkCapturing = micPhase == MicPhase::Open;
  const bool sdCapturing = recordings.recording();
  setCapture((networkCapturing || sdCapturing) && !agent_link_ota_is_busy());
  // One real microphone frame fans out to SD and network. Reading it twice
  // would give them different halves of the recording.
  if (!captureEnabled || (networkCapturing && pcmLength && !sdCapturing)) return;
  uint8_t fresh[sizeof(pcmPending)];
  const size_t bytes = audio.readCapture(fresh, sizeof(fresh));
  if (!bytes) return;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    const int16_t value = static_cast<int16_t>(uint16_t(fresh[i]) | (uint16_t(fresh[i + 1]) << 8));
    if (value < micMinimum) micMinimum = value;
    if (value > micMaximum) micMaximum = value;
    if (micSamples && value != micPrevious) ++micChanges;
    if (value == -32768 || value == 32767) ++micClipped;
    micPrevious = value; micSum += value;
    micSquares += uint64_t(int64_t(value) * value); ++micSamples;
  }
  if (sdCapturing && recordings.enqueuePcm(fresh, bytes) != bytes) {
    recordings.stopRecording(false);
    report("SD record incomplete", ESP_ERR_NO_MEM);
  }
  if (networkCapturing) {
    if (pcmLength) { micComplete = false; ++droppedFrames; ++pendingReplacementDrops; }
    else {
      memcpy(pcmPending, fresh, bytes);
      pcmLength = bytes; pcmOffset = 0; pcmAt = now;
    }
  }
}

void requestFile(FileMode mode) {
  if (!recordings.ready() || recordings.busy() || fileMode != FileMode::None ||
      agent_link_ota_is_busy() || !audio.ready()) {
    report("Finish recording/playback before reading latest WAV", ESP_ERR_INVALID_STATE);
    return;
  }
  if (mode == FileMode::Upload && (!Deotaland.isReady() || captureError != ESP_OK || micFault ||
      agent_link_ota_needs_reconnect())) {
    report("AUDIO upload requires READY and CoC", ESP_ERR_INVALID_STATE);
    return;
  }
  pttRequested = listenRequested = false;
  stopMic(true);
  const esp_err_t error = recordings.openLatestPcm();
  if (error != ESP_OK) { report("open latest SD PCM", error); return; }
  fileMode = mode;
  filePhase = FilePhase::Opening;
  fileLength = fileOffset = 0;
  fileHandle = nullptr;
  fileAt = millis();
  fileAttempt = fileAt - 100;
  fileComplete = true;
  if (mode == FileMode::Play) {
    discardReply = discardReply || (started && Deotaland.playbackActive());
    audio.cancelPlayback();
  }
}

void pollFile(uint32_t now) {
  if (fileMode == FileMode::None) return;
  if (fileMode == FileMode::Play && !audio.ready()) {
    report("audio driver failed during SD replay", ESP_FAIL);
    stopFile();
    return;
  }
  if ((agent_link_ota_is_busy() || (fileMode == FileMode::Upload && !Deotaland.isReady())) &&
      filePhase != FilePhase::Closing && filePhase != FilePhase::Draining) stopFile();
  if (fileMode == FileMode::None) return;
  const esp_err_t readerError = recordings.readerError();
  if (readerError != ESP_OK && !retryable(readerError) && filePhase != FilePhase::Closing &&
      filePhase != FilePhase::Draining) { report("SD PCM reader", readerError); stopFile(); }
  if (fileMode == FileMode::None) return;
  if (filePhase == FilePhase::Opening) {
    if (uint32_t(now - fileAt) >= AIOT_IMAGE_TIMEOUT_MS) {
      report("waiting for SD/AUDIO", ESP_ERR_TIMEOUT); stopFile(); return;
    }
    if (!recordings.readerReady() || micPhase != MicPhase::Idle ||
        uint32_t(now - fileAttempt) < 100) return;
    fileAttempt = now;
    if (fileMode == FileMode::Upload) {
      agent_stream_status_t status = {};
      if (Deotaland.streamStatus(AGENT_STREAM_AUDIO, &status) != ESP_OK ||
          status.state == AGENT_STREAM_OPEN || status.state == AGENT_STREAM_DRAINING) return;
      agent_stream_opts_t opts = {};
      opts.name = "aiot-basic-sd-recording";
      opts.total_bytes = recordings.readerBytes();
      const esp_err_t error = Deotaland.openStream(AGENT_STREAM_AUDIO, &opts, &fileHandle);
      if (retryable(error) || error == ESP_ERR_INVALID_STATE) return;
      if (error != ESP_OK) { report("SD AUDIO open", error); stopFile(); return; }
    }
    filePhase = FilePhase::Sending;
    fileAt = now;
  }
  if (filePhase == FilePhase::Sending) {
    if (!fileLength) {
      fileLength = recordings.readPcm(filePending, sizeof(filePending));
      fileOffset = 0;
    }
    if (fileLength) {
      const size_t bytes = fileLength - fileOffset;
      if (fileMode == FileMode::Play) fileOffset += audio.enqueuePlayback(filePending + fileOffset, bytes);
      else {
        const esp_err_t error = Deotaland.writeStream(fileHandle, filePending + fileOffset, bytes);
        if (error == ESP_OK) fileOffset += bytes;
        else if (!retryable(error)) { report("SD AUDIO write", error); stopFile(); return; }
      }
      if (fileOffset == fileLength) { fileLength = fileOffset = 0; fileAt = now; }
    }
    if (uint32_t(now - fileAt) >= AIOT_IMAGE_TIMEOUT_MS) {
      report("SD playback/upload stalled", ESP_ERR_TIMEOUT); stopFile(); return;
    }
    if (!fileLength && recordings.readerEof()) {
      recordings.closeReader();
      filePhase = fileMode == FileMode::Upload ? FilePhase::Closing : FilePhase::Draining;
      fileAt = now;
    }
  }
  if (filePhase == FilePhase::Closing && uint32_t(now - fileAt) >= 2000) {
    fileHandle = nullptr;
    fileMode = FileMode::None;
    micFault = true;
    report("SD AUDIO END stalled; reconnect before another stream", ESP_ERR_TIMEOUT);
    return;
  }
  if (filePhase == FilePhase::Closing && uint32_t(now - fileAttempt) >= 20) {
    fileAttempt = now;
    const esp_err_t error = Deotaland.closeStream(fileHandle, fileComplete);
    if (!retryable(error)) {
      fileHandle = nullptr;
      filePhase = FilePhase::Draining;
      if (error != ESP_OK) { micFault = true; fileMode = FileMode::None; }
      report("SD AUDIO END accepted locally", error);
    }
  }
  if (filePhase == FilePhase::Draining) {
    if (fileMode == FileMode::Play) {
      if (!audio.playbackBytes()) fileMode = FileMode::None;
    } else {
      agent_stream_status_t status = {};
      if (Deotaland.streamStatus(AGENT_STREAM_AUDIO, &status) == ESP_OK &&
          status.state != AGENT_STREAM_OPEN && status.state != AGENT_STREAM_DRAINING) fileMode = FileMode::None;
    }
  }
}

void pollCamera(uint32_t now) {
  upload.poll(now);
  if (sending.data && !upload.busy()) {
    if (upload.state() == DeotalandImageUpload::State::Completed) {
      ++uploadedImages;
      report("JPEG + END accepted locally (no peer ACK)", ESP_OK);
    } else report("JPEG upload", upload.result());
    CameraCapture::release(sending);
  }
  if (camera.busy() && uint32_t(now - cameraAt) >= AIOT_CAMERA_TIMEOUT_MS) captureExpired = true;
  CameraImage image;
  if (camera.take(image)) {
    if (image.result != ESP_OK) report("camera", image.result);
    else if (!captureExpired && image.epoch == epoch && !agent_link_ota_is_busy()) {
      if (screen.ready() && (captureForUpload || !textHold || uint32_t(now - lastText) >= 20000)) {
        const esp_err_t error = screen.showJpeg(image.data, image.length);
        if (error != ESP_OK) report("LCD JPEG preview", error);
      }
      if (captureForUpload && Deotaland.isReady()) {
        agent_stream_opts_t opts = {};
        opts.encoding = AGENT_ENC_JPEG;
        opts.width = image.width;
        opts.height = image.height;
        opts.total_bytes = static_cast<uint32_t>(image.length);
        const esp_err_t error = upload.start(opts, image.data, image.length, now, AIOT_IMAGE_TIMEOUT_MS);
        if (error == ESP_OK) {
          sending = image;
          image.data = nullptr;
        } else report("JPEG start", error);
      }
    }
    CameraCapture::release(image);
  }
  if (photoPending && (photoEpoch != epoch || !Deotaland.isReady() ||
      uint32_t(now - photoAt) >= AIOT_IMAGE_TIMEOUT_MS)) photoPending = false;
  if (cameraError != ESP_OK || camera.busy() || upload.busy() || sending.data ||
      agent_link_ota_is_busy()) return;
  if (photoPending) {
    agent_stream_status_t status = {};
    if (Deotaland.streamStatus(AGENT_STREAM_IMAGE, &status) != ESP_OK ||
        status.state == AGENT_STREAM_OPEN || status.state == AGENT_STREAM_DRAINING) return;
  } else if (!previewEnabled || screenError != ESP_OK || micPhase != MicPhase::Idle ||
             recordings.recording() || audio.playbackBytes() ||
             (textHold && uint32_t(now - lastText) < 20000) ||
             uint32_t(now - lastPreview) < AIOT_PREVIEW_INTERVAL_MS) return;
  captureForUpload = photoPending;
  photoPending = false;
  captureExpired = false;
  cameraAt = lastPreview = now;
  if (!camera.request(epoch)) report("camera request", ESP_ERR_INVALID_STATE);
}

void pollButton(uint32_t now) {
  if (AIOT_PTT_BUTTON_PIN < 0) return;
  const bool pressed = digitalRead(AIOT_PTT_BUTTON_PIN) == LOW;
  if (pressed != rawButton) { rawButton = pressed; buttonAt = now; }
  if (!pressed) buttonMustRelease = false;
  if (pressed != stableButton && uint32_t(now - buttonAt) >= AIOT_BUTTON_DEBOUNCE_MS) {
    stableButton = pressed;
    if (!buttonMustRelease) { pttRequested = pressed; if (pressed) listenRequested = false; }
    if (started && Deotaland.isReady()) {
      const uint8_t event[] = {0, uint8_t(pressed ? 1 : 0)};
      Deotaland.pushEvent(AGENT_EVT_BUTTON, event, sizeof(event));
    }
  }
}

void appendDiagnostic(const char* format, ...) {
  const size_t capacity = kDiagnosticBytes - diagnosticLogLength;
  if (capacity < 2) return;
  va_list arguments;
  va_start(arguments, format);
  const int count = std::vsnprintf(diagnosticLog + diagnosticLogLength, capacity, format, arguments);
  va_end(arguments);
  if (count > 0) diagnosticLogLength += min(size_t(count), capacity - 1);
}

void appendStreamDiagnostic(const char* name, agent_stream_t kind) {
  agent_stream_status_t status = {};
  const esp_err_t result = Deotaland.streamStatus(kind, &status);
  if (result != ESP_OK) {
    appendDiagnostic("%s streamStatus=%s\r\n", name, Deotaland.errorName(result));
    return;
  }
  // State describes the last local stream; COMPLETED does not prove CoC is connected.
  appendDiagnostic("%s state=%u sent=%lu accepted=%lu queued=%u rejected=%lu last=%s(%d)\r\n",
                   name, unsigned(status.state), (unsigned long)status.sent_bytes,
                   (unsigned long)status.accepted_bytes, unsigned(status.queued_bytes),
                   (unsigned long)status.rejected_writes, Deotaland.errorName(status.last_error),
                   int(status.last_error));
}

bool printDiagnostic() {
  // Keep an unfinished snapshot and retain any new '?' request until it drains.
  if (!Serial || diagnosticLogOffset < diagnosticLogLength) return false;
  if (!diagnosticLog) {
    const uint32_t now = millis();
    if (diagnosticAllocationFailed && uint32_t(now - diagnosticAllocateAt) < 1000) return false;
    diagnosticAllocateAt = now;
    diagnosticLog = static_cast<char*>(heap_caps_malloc(kDiagnosticBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!diagnosticLog)
      diagnosticLog = static_cast<char*>(heap_caps_malloc(kDiagnosticBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    diagnosticAllocationFailed = !diagnosticLog;
    if (!diagnosticLog) return false; // Optional diagnostics cannot stop the app.
  }
  diagnosticLogOffset = diagnosticLogLength = 0;
  appendDiagnostic("AIOT-Basic: p/r=PTT a/s=App audio c=photo t=prompt l=preview x=cancel\r\n");
  appendDiagnostic("b/e=SD record/stop y=play latest u=upload latest PCM d=refresh list\r\n");
  appendDiagnostic("o=abort OTA v=confirm healthy OTA f=forget binding ?=diagnostics\r\n");
  appendDiagnostic("link=%s, id=%s\r\n", Deotaland.stateName(Deotaland.state()),
                Deotaland.deviceId() ? Deotaland.deviceId() : "unavailable");
  appendDiagnostic("init: audio=%s LCD=%s Camera=%s Agent=%s\r\n",
                Deotaland.errorName(audioError), Deotaland.errorName(screenError),
                Deotaland.errorName(cameraError), Deotaland.errorName(beginError));
  appendDiagnostic("runtime: audio ready=%u capture ready=%u LCD ready=%u LCD error=%s\r\n", unsigned(audio.ready()), unsigned(audio.captureReady()),
                unsigned(screen.ready()), Deotaland.errorName(screen.lastError()));
  appendDiagnostic("SD=%s ready=%u recording=%u error=%s latest=%s list truncated=%u\r\n",
                Deotaland.errorName(sdError), unsigned(recordings.ready()), unsigned(recordings.recording()),
                Deotaland.errorName(recordings.lastError()), recordings.latestPath(), unsigned(recordings.listTruncated()));
  appendDiagnostic("media: VOICE=%s AUDIO=%s IMAGE=%s playback=%s OTA=%s\r\n",
                Deotaland.errorName(voiceError), Deotaland.errorName(captureError),
                Deotaland.errorName(imageError), Deotaland.errorName(playbackError),
                Deotaland.errorName(otaError));
  appendDiagnostic("BLE ATT_MTU=%u CoC81_connected=%u (shared AUDIO/TTS)\r\n",
                   unsigned(agent_transport_ble_att_mtu()), unsigned(agent_transport_ble_l2cap_ready()));
  appendStreamDiagnostic("VOICE", AGENT_STREAM_VOICE);
  appendStreamDiagnostic("AUDIO", AGENT_STREAM_AUDIO);
  appendStreamDiagnostic("IMAGE", AGENT_STREAM_IMAGE);
  const agent_playback_stats_t playback = Deotaland.playbackStats();
  appendDiagnostic("TTS active=%u acceptedPCM=%llu pendingPCM=%u sinkCalls=%lu backpressure=%lu\r\n",
                   unsigned(Deotaland.playbackActive()), (unsigned long long)playback.accepted_pcm_bytes,
                   unsigned(playback.pending_pcm_bytes), (unsigned long)playback.sink_calls,
                   (unsigned long)playback.backpressure_events);
  if (agent_link_ota_needs_reconnect()) appendDiagnostic("OTA finished/aborted: reconnect before VOICE/AUDIO/TTS or another OTA.\r\n");
  appendDiagnostic("mic accepted=%lluB, playback DMA=%lluB queued=%uB, dropped=%lu, images=%lu\r\n",
                (unsigned long long)capturedBytes, (unsigned long long)playedBytes,
                unsigned(audio.playbackBytes()), (unsigned long)droppedFrames,
                (unsigned long)uploadedImages);
  appendDiagnostic("mic drops: RX(queue+ISR)=%lu pendingTimeout=%lu pendingReplacement=%lu cancelPending=%lu\r\n",
                   (unsigned long)captureDriverDrops, (unsigned long)pendingTimeoutDrops,
                   (unsigned long)pendingReplacementDrops, (unsigned long)cancelPendingDrops);
  const double mean = micSamples ? double(micSum) / micSamples : 0;
  const double power = micSamples ? double(micSquares) / micSamples : 0;
  const double variance = power > mean * mean ? power - mean * mean : 0;
  appendDiagnostic("mic mode=%s channel=%s samples=%llu min=%d max=%d DC=%.2f acRMS=%.2f changes=%llu clipped=%llu\r\n",
                AIOT_MIC_USE_PDM ? "PDM16" : "I2S32", AIOT_MIC_RIGHT_CHANNEL ? "RIGHT" : "LEFT",
                (unsigned long long)micSamples, micSamples ? int(micMinimum) : 0,
                micSamples ? int(micMaximum) : 0, mean, std::sqrt(variance),
                (unsigned long long)micChanges, (unsigned long long)micClipped);
  if (micSamples >= 16000 && !micChanges)
    appendDiagnostic("MIC WARNING: constant PCM; check microphone power, wiring and selected channel. Driver ready does not prove sound input.\r\n");
  appendDiagnostic("flash=%u PSRAM size=%u heap=%u minHeap=%u largestInternal=%u freePSRAM=%u largestPSRAM=%u\r\n",
                unsigned(ESP.getFlashChipSize()), unsigned(ESP.getPsramSize()), unsigned(ESP.getFreeHeap()),
                unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                unsigned(ESP.getFreePsram()), unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
  appendDiagnostic("READY needs 0xFFC4, VOICE needs 0xFFA1; CoC PSM 81/82\r\n");
  return true;
}

void pollSerial(uint32_t now) {
  for (unsigned n = 0; n < 16 && Serial.available(); ++n) {
    switch (Serial.read()) {
    case 'p': if (!agent_link_ota_is_busy()) { pttRequested = true; listenRequested = false; } break;
    case 'r': pttRequested = false; break;
    case 'a': onListen(true, 30000, nullptr); break;
    case 's': pttRequested = listenRequested = false; break;
    case 'c': requestPhoto(); break;
    case 't': promptPending = true; break;
    case 'l': previewEnabled = !previewEnabled; textHold = false; break;
    case 'b': if (!agent_link_ota_is_busy()) recordingRequested = true; break;
    case 'e': recordingRequested = false; consumeCaptureDrops(); recordings.stopRecording(); break;
    case 'u': requestFile(FileMode::Upload); break;
    case 'y': requestFile(FileMode::Play); break;
    case 'd': recordings.refreshList(); break;
    case 'x': cancelMedia(); break;
    case 'o': agent_link_ota_abort(); break;
    case 'v':
      if (audio.ready() && screen.ready() && cameraError == ESP_OK && !agent_link_ota_is_busy())
        report("OTA health confirmation", agent_link_ota_mark_valid());
      else report("Check every peripheral before confirming OTA", ESP_ERR_INVALID_STATE);
      break;
    case 'f': if (started) report("forget binding", Deotaland.forget()); break;
    case '?': diagnosticPending = true; break;
    default: break;
    }
  }
  if ((diagnosticPending && uint32_t(now - lastDiagnostic) >= 500) ||
      uint32_t(now - lastDiagnostic) >= 10000) {
    if (printDiagnostic()) {
      diagnosticPending = false;
      lastDiagnostic = now;
    }
  }
  if (!Serial) return;
  // At most one 64-byte fragment per poll. CRLF is part of the text budget.
  if (textLogOffset < textLogLength) {
    const size_t bytes = min(size_t(64), textLogLength - textLogOffset);
    if (Serial.availableForWrite() >= int(bytes))
      textLogOffset += Serial.write(reinterpret_cast<const uint8_t*>(textLog) + textLogOffset, bytes);
  } else if (diagnosticLogOffset < diagnosticLogLength) {
    const size_t bytes = min(size_t(64), diagnosticLogLength - diagnosticLogOffset);
    if (Serial.availableForWrite() >= int(bytes))
      diagnosticLogOffset += Serial.write(reinterpret_cast<const uint8_t*>(diagnosticLog) + diagnosticLogOffset, bytes);
  }
}

esp_err_t registerEndpoints() {
  if (cameraError == ESP_OK) {
    cameraEndpoint.id = "camera0"; cameraEndpoint.dir = AGENT_IO_OUT;
    cameraEndpoint.kind = "camera.capture"; cameraEndpoint.value = AGENT_VAL_BOOL;
    cameraEndpoint.desc = "Capture a fresh camera JPEG and upload it to the App";
    cameraEndpoint.display_name = "Camera Snapshot";
    const esp_err_t error = Deotaland.registerIo(cameraEndpoint, onCamera);
    if (error != ESP_OK) return error;
  }
  if (recordings.ready() && audio.captureReady()) {
    recordingEndpoint.id = "recording0"; recordingEndpoint.dir = AGENT_IO_OUT;
    recordingEndpoint.kind = "audio.record"; recordingEndpoint.value = AGENT_VAL_BOOL;
    recordingEndpoint.desc = "True starts a local SD WAV recording; false stops and saves it";
    const esp_err_t error = Deotaland.registerIo(recordingEndpoint, onRecording);
    if (error != ESP_OK) return error;
  }
  if (audioError == ESP_OK) {
    volumeEndpoint.id = "volume0"; volumeEndpoint.dir = AGENT_IO_OUT;
    volumeEndpoint.kind = "speaker.volume"; volumeEndpoint.value = AGENT_VAL_U16;
    volumeEndpoint.unit = "%"; volumeEndpoint.range_max = 100;
    volumeEndpoint.desc = "I2S speaker software volume, 0 to 100 percent";
    const esp_err_t error = Deotaland.registerIo(volumeEndpoint, onVolume);
    if (error != ESP_OK) return error;
  }
  uptimeEndpoint.id = "uptime0"; uptimeEndpoint.dir = AGENT_IO_IN;
  uptimeEndpoint.kind = "system.uptime"; uptimeEndpoint.value = AGENT_VAL_I32;
  uptimeEndpoint.unit = "s"; uptimeEndpoint.desc = "Elapsed boot time (seconds)";
  uptimeEndpoint.audience = AGENT_AUD_USER; // Diagnostic reading, kept out of AI tools.
  return Deotaland.registerIo(uptimeEndpoint);
}
} // namespace

void setup() {
  Serial.begin(115200); // Do not wait for a USB host before starting the device.
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0); // Also bound a TX-lock/ring race after the capacity check.
#endif
  if (AIOT_PTT_BUTTON_PIN >= 0) pinMode(AIOT_PTT_BUTTON_PIN, INPUT_PULLUP);
  if (AIOT_ENABLE_SD) sdError = recordings.begin();
  audioError = audio.begin();
  if (audioError == ESP_OK) audioError = audio.setVolume(AIOT_INITIAL_VOLUME);
  screenError = screen.begin();
  cameraError = camera.begin();
  callbacks.on_audio_end = onAudioEnd;
  callbacks.on_show_text = onText;
  callbacks.on_led = onLed;
  callbacks.on_listen = onListen;
  callbacks.on_command = onCommand;
  agent_link_config_t config = {};
  config.device_name = AIOT_DEVICE_NAME;
  config.model = AIOT_HARDWARE_MODEL;
  config.firmware_rev = AIOT_FIRMWARE_VERSION;
  config.manufacturer = "Deotaland";
  config.caps = AGENT_CAP_LED | AGENT_CAP_SENSOR;
  if (audioError == ESP_OK) config.caps |= AGENT_CAP_SPEAKER | AGENT_CAP_ACTUATOR;
  if (audio.captureReady()) config.caps |= AGENT_CAP_MIC;
  if (screenError == ESP_OK) config.caps |= AGENT_CAP_SCREEN;
  if (cameraError == ESP_OK) config.caps |= AGENT_CAP_CAMERA | AGENT_CAP_ACTUATOR;
  if (AIOT_PTT_BUTTON_PIN >= 0) config.caps |= AGENT_CAP_BUTTON;
  if (recordings.ready() && audio.captureReady()) config.caps |= AGENT_CAP_RECORDING;
  config.output = &callbacks;
  config.on_state = onState;
  config.on_status = onStatus;
  config.transport = AGENT_TRANSPORT_BLE;
  // The provided board-specific CSV has only ota_0. Require a real second slot
  // and otadata; e.g. official min_spiffs. Do not promise OTA on a single slot.
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
  config.enable_ota = AIOT_ENABLE_OTA && running && update &&
      update->address != running->address &&
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  beginError = registerEndpoints();
  if (beginError == ESP_OK) beginError = Deotaland.begin(config);
  started = beginError == ESP_OK;
  if (started) {
    if (audioError == ESP_OK) {
      voiceError = Deotaland.prepareStreams(1u << AGENT_STREAM_VOICE);
      captureError = Deotaland.prepareStreams(1u << AGENT_STREAM_AUDIO);
      agent_playback_config_t playback = {playbackWrite, playbackQueued, nullptr};
      playbackError = Deotaland.configurePlayback(&playback);
      Deotaland.setPlaybackBufferMs(2000); // 64KiB mono PCM ring, with DMA watermark.
    }
    if (cameraError == ESP_OK) imageError = Deotaland.prepareStreams(1u << AGENT_STREAM_IMAGE);
    agent_link_ota_set_callback(onOta, nullptr);
    if (config.enable_ota) otaError = Deotaland.prepareOta();
  }
  screen.showText("AIOT-Basic ready. Connect Agent App. Serial ? for help.");
  if (!started) screen.showStatus("Agent init failed", Deotaland.errorName(beginError));
}

void loop() {
  consumeCaptureDrops(); // Before callbacks/serial can accept a normal recording END.
  if (started) Deotaland.poll();
  const uint32_t now = millis();
  if (resetPending) resetMedia();
  const bool otaBusy = agent_link_ota_is_busy();
  if (otaBusy && !otaWasBusy) cancelMedia();
  otaWasBusy = otaBusy;
  pollSerial(now);
  pollButton(now);
  if (started) {
    pollMic(now);
    if (promptPending && uint32_t(now - lastPrompt) >= 100) {
      lastPrompt = now;
      const esp_err_t error = Deotaland.pushPrompt("你好");
      if (!retryable(error)) { promptPending = false; report("prompt", error); }
    }
#if AIOT_UPTIME_REPORT_INTERVAL_MS > 0
    if (uint32_t(now - lastReading) >= AIOT_UPTIME_REPORT_INTERVAL_MS) {
      lastReading = now;
      Deotaland.updateInt32("uptime0", int32_t(now / 1000));
    }
#endif
  }
  // SD recording and local replay work even without a BLE connection.
  pollRecording(now);
  pollFile(now);
  pollCamera(now);
  if (volumePending && !otaBusy) {
    volumePending = false;
    report("I2S volume", audio.setVolume(volume));
  }
  if (ledPending && !otaBusy) {
    ledPending = false;
    rgbLedWrite(AIOT_RGB_LED_PIN, uint8_t(ledRgb >> 16), uint8_t(ledRgb >> 8), uint8_t(ledRgb));
  }
  playedBytes += audio.takePlayedBytes();
  screen.poll();
  delay(1); // Let capture/display tasks and the idle watchdog run.
}
