#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>

// ESP32-S3 AIOT-Basic: digital microphone + I2S amplifier, PCM16/mono/16 kHz.
// BoardConfig.h selects standard I2S32 RX (default) or confirmed PDM hardware,
// and the physical left/right microphone slot. No DC/gain filter hides input.
// Use one long-lived instance. Public methods belong to the Arduino loop thread;
// capture/playback transfers return immediately and may accept fewer bytes.
// begin() and destruction perform driver operations, so call them outside
// Deotaland callbacks. No worker calls any Deotaland API.
class BoardAudio {
public:
    BoardAudio() = default;
    ~BoardAudio();
    BoardAudio(const BoardAudio&) = delete;
    BoardAudio& operator=(const BoardAudio&) = delete;

    // Playback remains available when capture is disabled to save internal RAM.
    esp_err_t begin(bool enableCapture = true);
    bool ready() const;
    bool captureReady() const;
    // A transition discards old capture packets and restarts only RX DMA.
    void setCapture(bool enabled);
    size_t readCapture(uint8_t* destination, size_t capacity);
    // Dropped input blocks (full queue / malformed driver output), reset on read.
    uint32_t takeCaptureDrops();
    size_t enqueuePlayback(const uint8_t* pcm, size_t bytes);
    // Includes application, internal staging and still-playing DMA bytes.
    size_t playbackBytes() const;
    // Mono PCM bytes whose DMA transmission completed, not merely queued bytes.
    uint32_t takePlayedBytes();
    // Asynchronous flush: the TX task stops, clears and restarts only TX DMA.
    // New enqueue calls return 0 until that cancellation has completed.
    void cancelPlayback();
    esp_err_t setVolume(uint8_t percent);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
