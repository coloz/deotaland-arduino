#pragma once

// Defaults match the user's AIOT-Basic wiring and https://arduino.me/a/3218.
// This is the independent I2S amplifier + digital microphone version, not the
// ES8311 variant described by newer oj_esp32s3basic/board.json files.
// Camera wiring is in camera_pins.h; audio/LCD wiring is at the top of their
// BoardAudio.cpp / BoardDisplay.cpp. Do not substitute older Basic kit pins.
#define AIOT_DEVICE_NAME "Deotaland-AIOT-Basic"
#define AIOT_HARDWARE_MODEL "OJ-AIOT-BASIC"
#define AIOT_FIRMWARE_VERSION "1.0.0"

// BLE carries all Agent control and media traffic.
#ifndef AIOT_ENABLE_OTA
#define AIOT_ENABLE_OTA 1
#endif

#define AIOT_RGB_LED_PIN 48
// Board JSON does not specify a physical PTT button. Serial p/r works as-is.
// An optional wired button uses INPUT_PULLUP and is active LOW. Check that its
// GPIO does not overlap camera, audio, LCD, USB or SD pins before enabling it.
#define AIOT_PTT_BUTTON_PIN (-1)
#define AIOT_BUTTON_DEBOUNCE_MS 25u
#define AIOT_CAPTURE_LIMIT_MS 60000u
#define AIOT_IMAGE_TIMEOUT_MS 30000u
#define AIOT_CAMERA_TIMEOUT_MS 5000u
// Optional direct PSRAM DMA on supported ESP32-S3 camera drivers.
// Keep the default capture path; unsupported drivers use ordinary capture.
#ifndef AIOT_CAMERA_PSRAM_DMA
#define AIOT_CAMERA_PSRAM_DMA 0
#endif
// Local preview is independent of IMAGE upload. Zero disables auto-preview.
#define AIOT_PREVIEW_INTERVAL_MS 1000u
#define AIOT_INITIAL_VOLUME 60u
// Automatic uptime reading samples. Zero keeps the endpoint but stops updates.
#ifndef AIOT_UPTIME_REPORT_INTERVAL_MS
#define AIOT_UPTIME_REPORT_INTERVAL_MS 1000u
#endif

// The same-series recording tutorial uses a standard I2S microphone:
// BCLK41 / WS42 / DIN2. Its MSM261S4030H0R label is an I2S part, despite the
// tutorial's PDM wording. Choose PDM only for a confirmed two-wire variant.
// See https://arduino.me/a/3107 and the microphone manufacturer's datasheet.
#ifndef AIOT_MIC_USE_PDM
#define AIOT_MIC_USE_PDM 0
#endif
// Default left channel matches the tutorial. A high L/R select uses right.
// Both modes still expose PCM16 mono at 16 kHz to the application.
#ifndef AIOT_MIC_RIGHT_CHANNEL
#define AIOT_MIC_RIGHT_CHANNEL 0
#endif

// SD is wired SCLK3 / MISO35 / MOSI14 / CS46, on its own SPI bus.
#define AIOT_ENABLE_SD 1
// Storage format only: both PCM and IMA-ADPCM WAV are decoded to PCM16 for
// local playback/AUDIO upload. Default PCM preserves existing recordings.
#ifndef AIOT_SD_ADPCM
#define AIOT_SD_ADPCM 0
#endif
