// AIOT-Basic has an independent I2S amplifier and digital microphone.
// No codec I2C, external MCLK or PA GPIO is used. GPIO46 remains free for SD CS.
// I2S0 captures standard I2S32 by default, or PDM on an explicitly selected
// two-wire variant. I2S1 independently drives standard I2S TX.
// API basis: https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/peripherals/i2s.html
#include "BoardAudio.h"
#include "BoardConfig.h"

#include <atomic>
#include <cstring>
#include <new>
#include <driver/i2s_pdm.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {
constexpr const char* kTag = "AIOT.Audio";
constexpr size_t kSamples = 320;                    // 20 ms, mono PCM16
constexpr size_t kMonoBytes = kSamples * 2;
constexpr size_t kStereoBytes = kSamples * 4;
constexpr size_t kPlaybackBytes = 64 * 1024;         // PSRAM, 2048 ms
constexpr size_t kCapturePackets = 8;               // 160 ms, internal RAM
constexpr size_t kDmaBuffers = 4;
constexpr size_t kStages = 4;
constexpr uint32_t kSampleRate = 16000;
constexpr TickType_t kWorkerDelay = pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1;
constexpr TickType_t kJoinDelay = pdMS_TO_TICKS(1) ? pdMS_TO_TICKS(1) : 1;
constexpr gpio_num_t kBclk = GPIO_NUM_39, kWs = GPIO_NUM_40, kDout = GPIO_NUM_38;
constexpr gpio_num_t kMicClk = GPIO_NUM_41, kMicDin = GPIO_NUM_2;
#if !AIOT_MIC_USE_PDM
constexpr gpio_num_t kMicWs = GPIO_NUM_42;
// MSM261S4030H0R outputs signed, MSB-first 24-bit data in a 32-bit I2S slot.
// Keep the high 16 bits without filtering, unsigned reinterpretation or gain.
constexpr int16_t MicPcm16(int32_t raw) { return static_cast<int16_t>(raw >> 16); }
#endif
}

struct BoardAudio::Impl {
    struct Stage { int16_t stereo[kSamples * 2]; size_t monoBytes = 0; } stages[kStages];
    struct Capture { int16_t mono[kSamples]; uint32_t epoch = 0; };
    Capture* capture = nullptr; // Internal RAM, reserved only when RX is requested.
    struct Dma { void* address = nullptr; size_t monoBytes = 0; uint32_t epoch = 0; } dma[kDmaBuffers];
    // All state used by the ISR, including its staging buffers, stays in SRAM.
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    std::atomic<bool> usable{false}, stopping{false}, txStopped{true}, rxStopped{true};
    StaticSemaphore_t ringMutexStorage{}, captureMutexStorage{};
    SemaphoreHandle_t ringMutex = nullptr, captureMutex = nullptr;
    TaskHandle_t txTask = nullptr, rxTask = nullptr;
    i2s_chan_handle_t tx = nullptr, rx = nullptr;
    bool txEnabled = false, rxEnabled = false;
    uint8_t* ring = nullptr;
    size_t ringHead = 0, ringUsed = 0;                // ringMutex
    TickType_t lastEnqueueTick = 0;                  // ringMutex
    size_t captureHead = 0, captureUsed = 0, captureOffset = 0; // captureMutex
    size_t stageHead = 0, stageUsed = 0;             // mux: task producer / ISR consumer
    uint32_t playbackEpoch = 1, pendingBytes = 0, playedBytes = 0;
    bool cancelPending = false;
    bool captureEnabled = false;
    uint32_t captureEpoch = 1, runningCaptureEpoch = 0, captureDrops = 0;
    uint8_t volume = 60;                             // mux, software amplitude percentage

    static bool sent(i2s_chan_handle_t, i2s_event_data_t* event, void* context);
    static bool overflow(i2s_chan_handle_t, i2s_event_data_t*, void* context);

    void fault(const char* operation, esp_err_t error) {
        ESP_LOGE(kTag,"%s: %s",operation,esp_err_to_name(error));
        usable.store(false,std::memory_order_release);
        stopping.store(true,std::memory_order_release);
    }

    void produce() {
        int16_t mono[kSamples];
        while (!stopping.load(std::memory_order_acquire)) {
            portENTER_CRITICAL(&mux);
            const bool cancel = cancelPending;
            const uint32_t epoch = playbackEpoch;
            const uint8_t gain = volume;
            const size_t count = stageUsed, tail = (stageHead + stageUsed) % kStages;
            portEXIT_CRITICAL(&mux);
            if (cancel) {
                // Independent I2S1: stop DMA before touching its buffers. The
                // Microphone RX on I2S0 continues without losing its clocks.
                esp_err_t e = i2s_channel_disable(tx);
                if (e != ESP_OK) { fault("cancel TX stop",e); break; }
                txEnabled = false;
                xSemaphoreTake(ringMutex,portMAX_DELAY);
                ringHead = ringUsed = 0;
                xSemaphoreGive(ringMutex);
                for (auto& buffer : dma) {
                    if (buffer.address) std::memset(buffer.address,0,kStereoBytes);
                }
                portENTER_CRITICAL(&mux);
                stageHead = stageUsed = 0;
                for (auto& buffer : dma) { buffer.monoBytes = 0; buffer.epoch = playbackEpoch; }
                portEXIT_CRITICAL(&mux);
                e = i2s_channel_enable(tx); txEnabled = e == ESP_OK;
                if (e != ESP_OK) { fault("cancel TX restart",e); break; }
                portENTER_CRITICAL(&mux);
                if (playbackEpoch == epoch) cancelPending = false;
                portEXIT_CRITICAL(&mux);
                vTaskDelay(kWorkerDelay);
                continue;
            }
            if (count == kStages || xSemaphoreTake(ringMutex,0) != pdTRUE) {
                vTaskDelay(kWorkerDelay); continue;
            }
            // Accumulate short network chunks into full 20ms blocks instead
            // of inserting silence after every 512-byte receive. A final tail
            // is padded only after 40ms with no enqueue (no END API required).
            const bool tailReady = ringUsed && TickType_t(xTaskGetTickCount() - lastEnqueueTick) >= pdMS_TO_TICKS(40);
            const size_t bytes = ringUsed >= kMonoBytes ? kMonoBytes : (tailReady ? ringUsed : 0);
            const size_t first = bytes < kPlaybackBytes - ringHead ? bytes : kPlaybackBytes - ringHead;
            if (bytes) {
                std::memcpy(mono,ring + ringHead,first);
                if (first < bytes) std::memcpy(reinterpret_cast<uint8_t*>(mono) + first,ring,bytes - first);
                ringHead = (ringHead + bytes) % kPlaybackBytes; ringUsed -= bytes;
            }
            xSemaphoreGive(ringMutex);
            if (!bytes) { vTaskDelay(kWorkerDelay); continue; }
            Stage& next = stages[tail];
            const size_t samples = bytes / 2;
            for (size_t i = 0; i < kSamples; ++i) {
                const int16_t value = i < samples ? static_cast<int16_t>(int32_t(mono[i]) * gain / 100) : 0;
                next.stereo[2*i] = next.stereo[2*i+1] = value;
            }
            next.monoBytes = bytes;
            portENTER_CRITICAL(&mux);
            // ISR consumption preserves the producer tail. Cancel never resets
            // stage indices until this producer reaches its cancellation path.
            if (!cancelPending && playbackEpoch == epoch) ++stageUsed;
            portEXIT_CRITICAL(&mux);
        }
        txStopped.store(true,std::memory_order_release);
        vTaskDelete(nullptr);
    }

    void receive() {
        int16_t mono[kSamples];
#if AIOT_MIC_USE_PDM
        uint8_t* const input = reinterpret_cast<uint8_t*>(mono);
        constexpr size_t inputBytes = kMonoBytes, sampleBytes = sizeof(int16_t);
#else
        int32_t raw[kSamples];
        uint8_t* const input = reinterpret_cast<uint8_t*>(raw);
        constexpr size_t inputBytes = sizeof(raw), sampleBytes = sizeof(int32_t);
#endif
        uint32_t observedEpoch = 0;
        size_t accumulated = 0;
        while (!stopping.load(std::memory_order_acquire)) {
            portENTER_CRITICAL(&mux);
            const uint32_t epoch = captureEpoch;
            const bool enabled = captureEnabled;
            portEXIT_CRITICAL(&mux);
            if (observedEpoch != epoch) {
                // IDF disable resets the read pointer; enable resets the RX
                // descriptor queue. I2S1 playback is independent of this port.
                esp_err_t e = i2s_channel_disable(rx);
                if (e == ESP_OK) rxEnabled = false;
                if (e == ESP_OK) { e = i2s_channel_enable(rx); rxEnabled = e == ESP_OK; }
                if (e != ESP_OK) { fault("capture DMA restart",e); break; }
                portENTER_CRITICAL(&mux); runningCaptureEpoch = epoch; portEXIT_CRITICAL(&mux);
                xSemaphoreTake(captureMutex,portMAX_DELAY);
                captureHead = captureUsed = captureOffset = 0;
                xSemaphoreGive(captureMutex);
                accumulated = 0; observedEpoch = epoch;
            }
            size_t got = 0;
            const esp_err_t e = i2s_channel_read(rx,input + accumulated,
                                                inputBytes - accumulated,&got,40);
            if (e != ESP_OK && e != ESP_ERR_TIMEOUT) {
                portENTER_CRITICAL(&mux); ++captureDrops; portEXIT_CRITICAL(&mux);
                fault("capture read",e); break;
            }
            if ((got % sampleBytes) || got > inputBytes - accumulated) {
                portENTER_CRITICAL(&mux); ++captureDrops; portEXIT_CRITICAL(&mux);
                // Reset the SDK read pointer too; skipping an odd byte alone
                // would leave every later PCM word shifted out of alignment.
                accumulated = 0; observedEpoch = epoch - 1; continue;
            }
            accumulated += got;
            if (accumulated != inputBytes) continue;
            accumulated = 0;
            if (!enabled) continue; // drain DMA continuously while capture is off
#if !AIOT_MIC_USE_PDM
            for (size_t i = 0; i < kSamples; ++i) mono[i] = MicPcm16(raw[i]);
#endif
            xSemaphoreTake(captureMutex,portMAX_DELAY);
            portENTER_CRITICAL(&mux);
            const bool current = captureEnabled && captureEpoch == observedEpoch;
            portEXIT_CRITICAL(&mux);
            if (current && captureUsed < kCapturePackets) {
                Capture& packet = capture[(captureHead + captureUsed) % kCapturePackets];
                std::memcpy(packet.mono,mono,kMonoBytes);
                packet.epoch = observedEpoch;
                ++captureUsed;
            } else if (current) {
                portENTER_CRITICAL(&mux); ++captureDrops; portEXIT_CRITICAL(&mux);
            }
            // Full capture queue drops this newest block; its capacity is fixed.
            xSemaphoreGive(captureMutex);
        }
        rxStopped.store(true,std::memory_order_release);
        vTaskDelete(nullptr);
    }

    bool release() {
        usable.store(false,std::memory_order_release);
        stopping.store(true,std::memory_order_release);
        for (unsigned n = 0; n < 1000 && (!txStopped.load() || !rxStopped.load()); ++n)
            vTaskDelay(kJoinDelay);
        if (!txStopped.load() || !rxStopped.load()) {
            ESP_LOGE(kTag,"audio workers did not retire; resources retained to avoid dangling DMA");
            return false;
        }
        if (rxEnabled) { if (i2s_channel_disable(rx) != ESP_OK) return false; rxEnabled = false; }
        if (txEnabled) { if (i2s_channel_disable(tx) != ESP_OK) return false; txEnabled = false; }
        if (rx) { if (i2s_del_channel(rx) != ESP_OK) return false; rx = nullptr; }
        if (tx) { if (i2s_del_channel(tx) != ESP_OK) return false; tx = nullptr; }
        if (ring) { heap_caps_free(ring); ring = nullptr; }
        if (capture) { heap_caps_free(capture); capture = nullptr; }
        if (captureMutex) vSemaphoreDelete(captureMutex);
        if (ringMutex) vSemaphoreDelete(ringMutex);
        return true;
    }
};

// Keep the IRAM ISR out of the class body: Xtensa cannot safely place literals
// for an inline COMDAT function in a separate .iram1 section.
bool IRAM_ATTR BoardAudio::Impl::sent(i2s_chan_handle_t, i2s_event_data_t* event, void* context) {
    auto& s = *static_cast<Impl*>(context);
    // IDF clears this completed descriptor BEFORE the callback. Fill it only
    // from SRAM; it will be played on its next trip around the DMA ring.
    if (!event || event->size != kStereoBytes || !event->dma_buf) return false;
    portENTER_CRITICAL_ISR(&s.mux);
    size_t slot = kDmaBuffers;
    for (size_t i = 0; i < kDmaBuffers; ++i) {
        if (s.dma[i].address == event->dma_buf) { slot = i; break; }
    }
    if (slot == kDmaBuffers) {
        for (size_t i = 0; i < kDmaBuffers; ++i) {
            if (!s.dma[i].address) { s.dma[i].address = event->dma_buf; slot = i; break; }
        }
    }
    if (slot < kDmaBuffers) {
        Dma& finished = s.dma[slot];
        if (finished.epoch == s.playbackEpoch && finished.monoBytes) {
            const uint32_t bytes = static_cast<uint32_t>(finished.monoBytes);
            s.pendingBytes -= bytes <= s.pendingBytes ? bytes : s.pendingBytes;
            s.playedBytes += bytes;
        }
        finished.monoBytes = 0;
        finished.epoch = s.playbackEpoch;
        if (!s.cancelPending && !s.stopping.load(std::memory_order_relaxed) && s.stageUsed) {
            Stage& next = s.stages[s.stageHead];
            std::memcpy(event->dma_buf,next.stereo,kStereoBytes);
            finished.monoBytes = next.monoBytes;
            s.stageHead = (s.stageHead + 1) % kStages;
            --s.stageUsed;
        }
    }
    portEXIT_CRITICAL_ISR(&s.mux);
    return false;
}

bool IRAM_ATTR BoardAudio::Impl::overflow(i2s_chan_handle_t, i2s_event_data_t*, void* context) {
    auto& s = *static_cast<Impl*>(context);
    portENTER_CRITICAL_ISR(&s.mux);
    if (s.captureEnabled && s.captureEpoch == s.runningCaptureEpoch) ++s.captureDrops;
    portEXIT_CRITICAL_ISR(&s.mux);
    return false;
}

BoardAudio::~BoardAudio() {
    if (impl_ && impl_->release()) { impl_->~Impl(); heap_caps_free(impl_); }
}

esp_err_t BoardAudio::begin(bool enableCapture) {
    if (impl_) return ESP_ERR_INVALID_STATE;
    void* memory = heap_caps_malloc(sizeof(Impl),MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) return ESP_ERR_NO_MEM;
    impl_ = new (memory) Impl();
    auto& s = *impl_;
    s.ringMutex = xSemaphoreCreateMutexStatic(&s.ringMutexStorage);
    if (enableCapture) {
        s.captureMutex = xSemaphoreCreateMutexStatic(&s.captureMutexStorage);
        s.capture = static_cast<Impl::Capture*>(heap_caps_malloc(kCapturePackets * sizeof(Impl::Capture),
                                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (s.capture) for (size_t i = 0; i < kCapturePackets; ++i) new (s.capture + i) Impl::Capture();
    }
    // No silently smaller capacity: absence of configured PSRAM is a startup error.
    s.ring = static_cast<uint8_t*>(heap_caps_malloc(kPlaybackBytes,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    esp_err_t e = ESP_OK;
    if (!s.ring || !s.ringMutex || (enableCapture && (!s.captureMutex || !s.capture))) e = ESP_ERR_NO_MEM;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
    channel.dma_desc_num = kDmaBuffers; channel.dma_frame_num = kSamples;
    channel.auto_clear_before_cb = true; channel.auto_clear_after_cb = false;
    if (e == ESP_OK) e = i2s_new_channel(&channel,&s.tx,nullptr);
    i2s_std_config_t standard = {};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    standard.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO);
    standard.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    standard.gpio_cfg.mclk = I2S_GPIO_UNUSED; standard.gpio_cfg.bclk = kBclk; standard.gpio_cfg.ws = kWs;
    standard.gpio_cfg.dout = kDout; standard.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (e == ESP_OK) e = i2s_channel_init_std_mode(s.tx,&standard);
    channel.id = I2S_NUM_0;
    channel.auto_clear_before_cb = false;
    if (e == ESP_OK && enableCapture) e = i2s_new_channel(&channel,nullptr,&s.rx);
#if AIOT_MIC_USE_PDM
    i2s_pdm_rx_config_t pdm = {};
    pdm.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(kSampleRate);
    pdm.slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_MONO);
    pdm.slot_cfg.slot_mask = AIOT_MIC_RIGHT_CHANNEL ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;
    pdm.gpio_cfg.clk = kMicClk; pdm.gpio_cfg.din = kMicDin;
    if (e == ESP_OK && enableCapture) e = i2s_channel_init_pdm_rx_mode(s.rx,&pdm);
#else
    i2s_std_config_t microphone = {};
    microphone.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    microphone.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_MONO);
    microphone.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    microphone.slot_cfg.slot_mask = AIOT_MIC_RIGHT_CHANNEL ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
    // STD always clocks two physical slots, even with one active DMA slot:
    // 32 SCK per WS half-period, 64 per frame, BCLK=1.024MHz at 16kHz.
    microphone.slot_cfg.ws_width = 32;
    microphone.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    microphone.gpio_cfg.bclk = kMicClk; microphone.gpio_cfg.ws = kMicWs;
    microphone.gpio_cfg.dout = I2S_GPIO_UNUSED; microphone.gpio_cfg.din = kMicDin;
    if (e == ESP_OK && enableCapture) e = i2s_channel_init_std_mode(s.rx,&microphone);
#endif
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_sent = Impl::sent;
    if (e == ESP_OK) e = i2s_channel_register_event_callback(s.tx,&callbacks,&s);
    callbacks = {};
    callbacks.on_recv_q_ovf = Impl::overflow;
    if (e == ESP_OK && enableCapture) e = i2s_channel_register_event_callback(s.rx,&callbacks,&s);
    if (e == ESP_OK) { e = i2s_channel_enable(s.tx); s.txEnabled = e == ESP_OK; }
    if (e == ESP_OK && enableCapture) { e = i2s_channel_enable(s.rx); s.rxEnabled = e == ESP_OK; }
    // Publish before launching workers: an early worker fault must be able to
    // clear readiness without begin() subsequently overwriting that fault.
    if (e == ESP_OK) s.usable.store(true,std::memory_order_release);
    if (e == ESP_OK) {
        s.txStopped.store(false);
        if (xTaskCreate([](void* p){ static_cast<Impl*>(p)->produce(); },"aiot_pcm_tx",4096,&s,5,&s.txTask) != pdPASS) {
            s.txStopped.store(true); e = ESP_ERR_NO_MEM;
        }
    }
    if (e == ESP_OK && enableCapture) {
        s.rxStopped.store(false);
        if (xTaskCreate([](void* p){ static_cast<Impl*>(p)->receive(); },"aiot_pcm_rx",6144,&s,5,&s.rxTask) != pdPASS) {
            s.rxStopped.store(true); e = ESP_ERR_NO_MEM;
        }
    }
    if (e == ESP_OK && s.stopping.load(std::memory_order_acquire)) e = ESP_FAIL;
    if (e != ESP_OK) {
        ESP_LOGE(kTag,"begin: %s",esp_err_to_name(e));
        if (s.release()) { s.~Impl(); heap_caps_free(impl_); impl_ = nullptr; }
        return e;
    }
    ESP_LOGI(kTag,"I2S1 amp, mic=%s: 16000 Hz mono, volume60, 64KiB PSRAM playback",
             enableCapture ? (AIOT_MIC_USE_PDM ? "PDM" : "I2S32") : "disabled");
    return ESP_OK;
}

bool BoardAudio::ready() const { return impl_ && impl_->usable.load(std::memory_order_acquire); }
bool BoardAudio::captureReady() const { return ready() && impl_->rx && impl_->capture; }

void BoardAudio::setCapture(bool enabled) {
    if (!captureReady()) return;
    auto& s = *impl_;
    portENTER_CRITICAL(&s.mux);
    if (s.captureEnabled != enabled) { s.captureEnabled = enabled; ++s.captureEpoch; }
    portEXIT_CRITICAL(&s.mux);
}

size_t BoardAudio::readCapture(uint8_t* destination, size_t capacity) {
    if (!captureReady() || !destination || capacity < 2) return 0;
    auto& s = *impl_;
    if (xSemaphoreTake(s.captureMutex,0) != pdTRUE) return 0;
    portENTER_CRITICAL(&s.mux);
    const bool enabled = s.captureEnabled;
    const uint32_t epoch = s.captureEpoch;
    portEXIT_CRITICAL(&s.mux);
    while (s.captureUsed && (!enabled || s.capture[s.captureHead].epoch != epoch)) {
        s.captureHead = (s.captureHead + 1) % kCapturePackets;
        --s.captureUsed; s.captureOffset = 0;
    }
    size_t bytes = 0;
    if (s.captureUsed) {
        bytes = kMonoBytes - s.captureOffset;
        if (bytes > capacity) bytes = capacity & ~size_t(1);
        std::memcpy(destination,reinterpret_cast<uint8_t*>(s.capture[s.captureHead].mono) + s.captureOffset,bytes);
        s.captureOffset += bytes;
        if (s.captureOffset == kMonoBytes) {
            s.captureHead = (s.captureHead + 1) % kCapturePackets;
            --s.captureUsed; s.captureOffset = 0;
        }
    }
    xSemaphoreGive(s.captureMutex);
    return bytes;
}

size_t BoardAudio::enqueuePlayback(const uint8_t* pcm, size_t bytes) {
    if (!ready() || !pcm || bytes < 2) return 0;
    auto& s = *impl_;
    if (xSemaphoreTake(s.ringMutex,0) != pdTRUE) return 0;
    portENTER_CRITICAL(&s.mux);
    const bool cancel = s.cancelPending;
    portEXIT_CRITICAL(&s.mux);
    size_t accepted = 0;
    if (!cancel) {
        accepted = bytes < kPlaybackBytes - s.ringUsed ? bytes : kPlaybackBytes - s.ringUsed;
        if (accepted > kMonoBytes) accepted = kMonoBytes; // bounded callback copy
        accepted &= ~size_t(1);
        const size_t tail = (s.ringHead + s.ringUsed) % kPlaybackBytes;
        const size_t first = accepted < kPlaybackBytes - tail ? accepted : kPlaybackBytes - tail;
        std::memcpy(s.ring + tail,pcm,first);
        if (first < accepted) std::memcpy(s.ring,pcm + first,accepted - first);
        s.ringUsed += accepted;
        if (accepted) s.lastEnqueueTick = xTaskGetTickCount();
        portENTER_CRITICAL(&s.mux);
        s.pendingBytes += static_cast<uint32_t>(accepted);
        portEXIT_CRITICAL(&s.mux);
    }
    xSemaphoreGive(s.ringMutex);
    return accepted;
}

size_t BoardAudio::playbackBytes() const {
    if (!ready()) return 0;
    auto& s = *impl_;
    portENTER_CRITICAL(&s.mux); const size_t bytes = s.pendingBytes; portEXIT_CRITICAL(&s.mux);
    return bytes;
}

uint32_t BoardAudio::takePlayedBytes() {
    if (!ready()) return 0;
    auto& s = *impl_;
    portENTER_CRITICAL(&s.mux); const uint32_t bytes = s.playedBytes; s.playedBytes = 0; portEXIT_CRITICAL(&s.mux);
    return bytes;
}

void BoardAudio::cancelPlayback() {
    if (!ready()) return;
    auto& s = *impl_;
    portENTER_CRITICAL(&s.mux);
    ++s.playbackEpoch;
    s.cancelPending = true;
    s.pendingBytes = s.playedBytes = 0;
    portEXIT_CRITICAL(&s.mux);
}

esp_err_t BoardAudio::setVolume(uint8_t percent) {
    if (!ready()) return ESP_ERR_INVALID_STATE;
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    auto& s = *impl_;
    portENTER_CRITICAL(&s.mux); s.volume = percent; portEXIT_CRITICAL(&s.mux);
    return ESP_OK;
}

uint32_t BoardAudio::takeCaptureDrops() {
    if (!impl_) return 0;
    auto& s = *impl_;
    portENTER_CRITICAL(&s.mux);
    const uint32_t drops = s.captureDrops; s.captureDrops = 0;
    portEXIT_CRITICAL(&s.mux);
    return drops;
}
