#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>

// AIOT-Basic SPI SD (SPI3: SCK3/MISO35/MOSI14/CS46).
// One long-lived instance; public methods are called only by the Arduino loop.
// Except begin()/destruction, calls return immediately without filesystem I/O.
// A dedicated worker owns mount, file creation, writes, WAV finalisation, directory
// scans and reads. Input/output is PCM16/mono/16000 Hz; AIOT_SD_ADPCM optionally
// selects IMA-ADPCM WAV storage. Bounded PSRAM buffers; no auto formatting.
class BoardRecording {
public:
    BoardRecording() = default;
    ~BoardRecording();
    BoardRecording(const BoardRecording&) = delete;
    BoardRecording& operator=(const BoardRecording&) = delete;

    esp_err_t begin();
    bool ready() const;
    esp_err_t startRecording();
    // Stops accepting immediately. Buffered PCM is still written and the WAV is
    // finalised; complete=false retains that partial file without selecting it
    // as the latest successfully completed recording. Cancellation is accepted
    // until final rename is committed; a later cancel does not undo a saved file.
    void stopRecording(bool complete = true);
    bool recording() const;  // true from accepted start through accepting PCM
    bool busy() const;       // recording, finalisation or reader ownership
    size_t enqueuePcm(const uint8_t* pcm, size_t bytes);
    uint32_t takeDrops();    // PCM bytes rejected since the previous call
    esp_err_t lastError() const;

    // poll copies worker metadata into stable loop-owned strings, without I/O.
    void poll();
    const char* latestPath() const;
    esp_err_t refreshList();
    bool listTruncated() const;
    // Korvo 0x04 payload: empty, offset LE16 + max entries, or + scope (0/1).
    // *responseBytes is input capacity and output used; minimum response is 7B.
    // This serialises only the directory cache, grown on the worker as needed.
    // An incomplete scan/allocation failure returns an error, not a partial
    // successful directory. The caller applies its ATT budget.
    esp_err_t handleList(const uint8_t* request, size_t requestBytes,
                         uint8_t* response, size_t* responseBytes);

    // Mutually exclusive with recording. ESP_OK reserves the reader immediately;
    // readerReady becomes true after the worker validates fmt/data chunks.
    // PCM16 or IMA-ADPCM (256B/505-sample blocks), mono/16000 Hz WAV is accepted.
    // Returns decoded little-endian PCM16; bytes exclude WAV headers/padded tails.
    esp_err_t openLatestPcm();
    bool readerReady() const;
    uint32_t readerBytes() const; // decoded PCM length, not compressed file size
    const char* readName() const;  // stable basename snapshot after poll()
    size_t readPcm(uint8_t* destination, size_t capacity);
    bool readerEof() const;       // worker EOF and buffered PCM fully consumed
    esp_err_t readerError() const;
    void closeReader();          // asynchronous cancellation; busy until closed

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
