#include "BoardRecording.h"
#include "BoardConfig.h"
#include "RecordingCodec.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_private/esp_gpio_reserve.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>

namespace {
constexpr spi_host_device_t kHost = SPI3_HOST;
constexpr int kSck = 3, kMiso = 35, kMosi = 14, kCs = 46;
constexpr const char* kMount = "/aiotsd";
constexpr const char* kRecords = "/aiotsd/records";
constexpr const char* kMessages = "/aiotsd/records/messages";
constexpr size_t kWriteBytes = 64 * 1024, kReadBytes = 16 * 1024;
constexpr size_t kChunkBytes = 4096, kInitialEntries = 128, kPathBytes = 320;
constexpr uint32_t kRate = 16000, kBytesPerSecond = kRate * 2;
constexpr bool kStoreAdpcm = AIOT_SD_ADPCM != 0;
constexpr uint32_t kHeaderBytes = kStoreAdpcm ? 60 : 44;

esp_err_t storagePinsAvailable() {
#if (defined(CONFIG_SPIRAM_MODE_OCT) && CONFIG_SPIRAM_MODE_OCT) || \
    (defined(CONFIG_ESPTOOLPY_OCT_FLASH) && CONFIG_ESPTOOLPY_OCT_FLASH)
    ESP_LOGE("aiot-sd", "SD MISO35 conflicts with octal flash/PSRAM; use verified Quad/QSPI hardware");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (esp_gpio_is_reserved(uint64_t(1) << kMiso)) {
        ESP_LOGE("aiot-sd", "SD MISO35 is reserved (flash/PSRAM or another peripheral); SD disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

void put16(uint8_t* p, uint16_t value) { p[0] = value; p[1] = value >> 8; }
void put32(uint8_t* p, uint32_t value) {
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}
uint16_t get16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool mkdirIfMissing(const char* path) {
    if (mkdir(path, 0775) == 0) return true;
    struct stat st = {};
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
bool endsWithWav(const char* name) {
    const size_t length = strlen(name);
    return length >= 4 && strcasecmp(name + length - 4, ".wav") == 0;
}
bool ownSequence(const char* name, uint32_t& sequence) {
    const size_t length = strlen(name);
    if ((length != 16 && length != 24) || memcmp(name, "rec-", 4)) return false;
    if (strcmp(name + 12, ".wav") && strcmp(name + 12, ".partial.wav")) return false;
    sequence = 0;
    for (unsigned i = 4; i < 12; ++i) {
        const char c = name[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return false;
        sequence = (sequence << 4) | static_cast<uint32_t>(digit);
    }
    return true;
}
bool readAt(int fd, uint32_t offset, uint8_t* dst, size_t bytes) {
    if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) return false;
    while (bytes) {
        const ssize_t n = ::read(fd, dst, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        dst += n; bytes -= static_cast<size_t>(n);
    }
    return true;
}
// Walk RIFF chunks, including padding and optional unknown chunks. Bounds are
// checked against both RIFF length and the actual file before every read/seek.
struct WavInfo {
    uint32_t offset = 0, bytes = 0, pcmBytes = 0, durationMs = 0;
    uint16_t format = 0;
    bool playable = false;
};
bool inspectWav(int fd, uint32_t fileBytes, WavInfo& info) {
    info = WavInfo();
    uint8_t header[20];
    if (fileBytes < 12 || !readAt(fd, 0, header, 12) ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) return false;
    const uint64_t end = uint64_t(get32(header + 4)) + 8;
    if (end < 12 || end > fileBytes) return false;
    uint32_t rate = 0, byteRate = 0;
    uint16_t format = 0, channels = 0, bits = 0, align = 0, samplesPerBlock = 0;
    uint32_t factSamples = 0;
    bool haveFormat = false, haveData = false, haveFact = false, adpcmExtension = false;
    uint64_t at = 12;
    unsigned chunks = 0;
    while (at + 8 <= end && chunks++ < 128) {
        if (!readAt(fd, static_cast<uint32_t>(at), header, 8)) return false;
        const uint32_t length = get32(header + 4);
        const uint64_t body = at + 8;
        const uint64_t next = body + length + (length & 1u);
        if (body + length > end || next > end) return false;
        if (!memcmp(header, "fmt ", 4)) {
            if (haveFormat) return false;
            if (length < 16 || !readAt(fd, static_cast<uint32_t>(body), header, 16)) return false;
            format = get16(header); channels = get16(header + 2);
            rate = get32(header + 4); byteRate = get32(header + 8);
            align = get16(header + 12); bits = get16(header + 14);
            if (format == 0x11 && length >= 20) {
                if (!readAt(fd, static_cast<uint32_t>(body + 16), header, 4)) return false;
                const uint16_t extensionBytes = get16(header);
                adpcmExtension = extensionBytes >= 2 && uint32_t(extensionBytes) + 18 <= length;
                samplesPerBlock = get16(header + 2);
            }
            haveFormat = true;
        } else if (!memcmp(header, "data", 4)) {
            if (haveData) return false;
            info.offset = static_cast<uint32_t>(body); info.bytes = length; haveData = true;
        } else if (!memcmp(header, "fact", 4)) {
            if (haveFact || length < 4 || !readAt(fd, static_cast<uint32_t>(body), header, 4)) return false;
            factSamples = get32(header); haveFact = true;
        }
        at = next;
    }
    if (!haveFormat || !haveData || at != end) return false;
    info.format = format;
    info.playable = format == 1 && channels == 1 && bits == 16 && align == 2 &&
                    rate == kRate && byteRate == kBytesPerSecond && !(info.bytes & 1u);
    if (info.playable) info.pcmBytes = info.bytes;
    if (format == 1 && byteRate && channels && bits &&
        align == uint32_t(channels) * bits / 8 && uint64_t(rate) * align == byteRate) {
        info.durationMs = static_cast<uint32_t>(std::min<uint64_t>(
            uint64_t(info.bytes) * 1000 / byteRate, UINT32_MAX));
    }
    if (format == 0x11 && channels == 1 && bits == 4 && align == recording_codec::kBlockBytes &&
        rate == kRate && byteRate == uint64_t(kRate) * recording_codec::kBlockBytes / recording_codec::kSamplesPerBlock &&
        adpcmExtension && samplesPerBlock == recording_codec::kSamplesPerBlock && haveFact &&
        !(info.bytes % recording_codec::kBlockBytes)) {
        const uint64_t blocks = info.bytes / recording_codec::kBlockBytes;
        const uint64_t capacity = blocks * recording_codec::kSamplesPerBlock;
        // Only the final block may contain padding. Also bound decoded length
        // for the uint32 reader API before performing the multiplication.
        if (((!blocks && !factSamples) || (blocks && factSamples <= capacity &&
             factSamples > capacity - recording_codec::kSamplesPerBlock)) && factSamples <= UINT32_MAX / 2) {
            info.playable = true; info.pcmBytes = factSamples * 2;
            info.durationMs = static_cast<uint32_t>(uint64_t(factSamples) * 1000 / kRate);
        }
    }
    return true;
}
void wavHeader(uint8_t* header, uint32_t bytes) {
    memset(header, 0, 44);
    memcpy(header, "RIFF", 4); put32(header + 4, bytes + 36);
    memcpy(header + 8, "WAVEfmt ", 8); put32(header + 16, 16);
    put16(header + 20, 1); put16(header + 22, 1);
    put32(header + 24, kRate); put32(header + 28, kBytesPerSecond);
    put16(header + 32, 2); put16(header + 34, 16);
    memcpy(header + 36, "data", 4); put32(header + 40, bytes);
}
void adpcmWavHeader(uint8_t* header, uint32_t bytes, uint32_t samples) {
    memset(header, 0, 60);
    memcpy(header, "RIFF", 4); put32(header + 4, bytes + 52);
    memcpy(header + 8, "WAVEfmt ", 8); put32(header + 16, 20);
    put16(header + 20, 0x11); put16(header + 22, 1); put32(header + 24, kRate);
    put32(header + 28, uint64_t(kRate) * recording_codec::kBlockBytes / recording_codec::kSamplesPerBlock);
    put16(header + 32, recording_codec::kBlockBytes); put16(header + 34, 4);
    put16(header + 36, 2); put16(header + 38, recording_codec::kSamplesPerBlock);
    memcpy(header + 40, "fact", 4); put32(header + 44, 4); put32(header + 48, samples);
    memcpy(header + 52, "data", 4); put32(header + 56, bytes);
}
}

struct BoardRecording::Impl {
    enum class Mode { Idle, Record, Reader, Scan };
    struct Entry { char name[256]; uint32_t bytes, mtime, durationMs; };
    struct Cache {
        Entry* entries[2];
        uint16_t count[2], capacity[2];
        bool truncated;
        esp_err_t listError;
    };
    SemaphoreHandle_t mutex = nullptr, begun = nullptr, finished = nullptr;
    TaskHandle_t task = nullptr;
    sdmmc_card_t* card = nullptr;
    bool busOwned = false;
    uint8_t* writeRing = nullptr;
    uint8_t* readRing = nullptr;
    uint8_t* chunk = nullptr;
    Cache* cache[2] = {};
    unsigned activeCache = 0;
    size_t writeHead = 0, writeSize = 0, readHead = 0, readSize = 0;
    Mode mode = Mode::Idle;
    bool startPending = false, openPending = false, scanPending = false;
    std::atomic<bool> stopRequested{false}, complete{true}, closeRequested{false};
    // 0 undecided, 1 cancelled, 2 committed. A single CAS orders cancellation
    // against final rename; a late cancel cannot change a committed file's name.
    std::atomic<uint8_t> finalDecision{0};
    std::atomic<bool> recordReserved{false}, readerReserved{false};
    std::atomic<bool> exiting{false}, mounted{false}, accepting{false}, occupied{false};
    std::atomic<bool> readReady{false}, readFinished{false}, truncated{false};
    std::atomic<uint32_t> drops{0}, readTotal{0};
    std::atomic<esp_err_t> error{ESP_OK}, readError{ESP_OK}, beginResult{ESP_FAIL};
    char latestWorker[kPathBytes] = {}, latestView[kPathBytes] = {};
    char readPath[kPathBytes] = {}, nameWorker[256] = {}, nameView[256] = {};
    char recordPath[kPathBytes] = {};
    int recordFd = -1, readFd = -1;
    uint32_t written = 0, readRemaining = 0, maxSequence = 0;
    uint32_t committedSamples = 0, readStorageRemaining = 0;
    uint16_t readFormat = 0;
    recording_codec::Encoder encoder;
    uint8_t encodedBlock[recording_codec::kBlockBytes] = {};
    uint8_t decoded[recording_codec::kPcmBytesPerBlock] = {};
    size_t decodedOffset = 0, decodedSize = 0;
    bool writeFailed = false;

    // Grow only the inactive snapshot on the filesystem worker. The wire uses
    // uint16 offsets/counts; memory failure is an error, never a successful
    // truncated directory that hides the 129th and later recordings.
    bool growEntries(Cache& out, unsigned scope) {
        if (out.count[scope] < out.capacity[scope]) return true;
        if (out.capacity[scope] == UINT16_MAX) {
            out.listError = ESP_ERR_INVALID_SIZE;
            return false;
        }
        const size_t capacity = std::min<size_t>(UINT16_MAX,
            out.capacity[scope] ? size_t(out.capacity[scope]) * 2 : kInitialEntries);
        Entry* entries = static_cast<Entry*>(heap_caps_malloc(capacity * sizeof(Entry),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!entries) { out.listError = ESP_ERR_NO_MEM; return false; }
        if (out.count[scope]) memcpy(entries, out.entries[scope], out.count[scope] * sizeof(Entry));
        heap_caps_free(out.entries[scope]);
        out.entries[scope] = entries;
        out.capacity[scope] = static_cast<uint16_t>(capacity);
        return true;
    }
    void releaseCaches() {
        for (auto& snapshot : cache) {
            if (!snapshot) continue;
            for (auto& entries : snapshot->entries) heap_caps_free(entries);
            heap_caps_free(snapshot);
            snapshot = nullptr;
        }
    }

    bool lock(TickType_t ticks = 0) { return xSemaphoreTake(mutex, ticks) == pdTRUE; }
    void unlock() { xSemaphoreGive(mutex); }
    void wake() { if (task) xTaskNotifyGive(task); }
    static void runTask(void* context) { static_cast<Impl*>(context)->run(); }
    size_t popWrite() {
        lock(portMAX_DELAY);
        const size_t count = std::min(writeSize, kChunkBytes);
        const size_t first = std::min(count, kWriteBytes - writeHead);
        memcpy(chunk, writeRing + writeHead, first);
        memcpy(chunk + first, writeRing, count - first);
        writeHead = (writeHead + count) % kWriteBytes; writeSize -= count;
        unlock();
        return count;
    }
    bool writeAll(const uint8_t* bytes, size_t count, bool countData) {
        while (count) {
            const ssize_t n = ::write(recordFd, bytes, count);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            if (countData) written += static_cast<uint32_t>(n);
            bytes += n; count -= static_cast<size_t>(n);
        }
        return true;
    }
    bool writeRecordedPcm(const uint8_t* pcm, size_t bytes) {
        if (bytes & 1u) return false;
        if (!kStoreAdpcm) {
            return bytes <= UINT32_MAX - (kHeaderBytes - 8) - written && writeAll(pcm, bytes, true);
        }
        if (uint64_t(committedSamples) + encoder.pending() + bytes / 2 > UINT32_MAX / 2) return false;
        for (size_t i = 0; i < bytes; i += 2) {
            const int16_t sample = static_cast<int16_t>(get16(pcm + i));
            if (encoder.push(sample, encodedBlock)) {
                if (written > UINT32_MAX - 52 - recording_codec::kBlockBytes ||
                    !writeAll(encodedBlock, sizeof(encodedBlock), true)) return false;
                committedSamples += recording_codec::kSamplesPerBlock;
            }
        }
        return true;
    }
    void scan(bool selectExisting) {
        const unsigned next = 1 - activeCache;
        Cache& out = *cache[next];
        out.count[0] = out.count[1] = 0; out.truncated = false; out.listError = ESP_OK;
        uint32_t newestTime = 0;
        uint32_t newestSequence = 0;
        bool newestIsOwn = false;
        char newest[kPathBytes] = {};
        for (unsigned scope = 0; scope < 2; ++scope) {
            const char* directory = scope ? kMessages : kRecords;
            DIR* dir = opendir(directory);
            if (!dir) { out.listError = ESP_FAIL; continue; }
            for (;;) {
                // Other filesystem calls below can change errno. Clear it
                // before each readdir to distinguish EOF from a failed scan.
                errno = 0;
                dirent* entry = readdir(dir);
                if (!entry) {
                    if (errno) { out.listError = ESP_FAIL; out.truncated = true; }
                    break;
                }
                if (entry->d_name[0] == '.' || !endsWithWav(entry->d_name)) continue;
                uint32_t sequence = 0;
                const bool own = !scope && ownSequence(entry->d_name, sequence);
                if (own) maxSequence = std::max(maxSequence, sequence);
                char path[kPathBytes], relative[sizeof(Entry::name)];
                if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= int(sizeof(path))) continue;
                if (snprintf(relative, sizeof(relative), "%s%s", scope ? "messages/" : "", entry->d_name) >= int(sizeof(relative))) {
                    out.truncated = true; continue;
                }
                struct stat st = {};
                if (stat(path, &st) != 0) {
                    // A file removed during the scan is harmless. An I/O
                    // error must not be returned as a complete directory.
                    if (errno != ENOENT) { out.listError = ESP_FAIL; out.truncated = true; }
                    continue;
                }
                if (!S_ISREG(st.st_mode) || st.st_size < 0 || uint64_t(st.st_size) > UINT32_MAX) continue;
                WavInfo wav;
                const int fd = open(path, O_RDONLY);
                const bool valid = fd >= 0 && inspectWav(fd, static_cast<uint32_t>(st.st_size), wav);
                if (fd >= 0) close(fd);
                const uint32_t mtime = st.st_mtime > 0 ? static_cast<uint32_t>(st.st_mtime) : 0;
                const size_t relativeBytes = strlen(relative);
                const bool partial = relativeBytes >= 12 &&
                    strcasecmp(relative + relativeBytes - 12, ".partial.wav") == 0;
                if (selectExisting && !scope && !partial && valid && wav.playable && wav.bytes &&
                    ((own && (!newestIsOwn || sequence > newestSequence)) ||
                     (!own && !newestIsOwn && (!newest[0] || mtime > newestTime ||
                       (mtime == newestTime && strcmp(path, newest) > 0))))) {
                    snprintf(newest, sizeof(newest), "%s", path); newestTime = mtime;
                    newestIsOwn = own; newestSequence = sequence;
                }
                if (out.listError != ESP_OK || !growEntries(out, scope)) {
                    out.truncated = true; continue;
                }
                Entry& dst = out.entries[scope][out.count[scope]++];
                memcpy(dst.name, relative, strlen(relative) + 1);
                dst.bytes = static_cast<uint32_t>(st.st_size); dst.mtime = mtime;
                dst.durationMs = valid ? wav.durationMs : 0;
            }
            closedir(dir);
            if (out.count[scope] > 1)
                std::sort(out.entries[scope], out.entries[scope] + out.count[scope],
                          [](const Entry& a, const Entry& b) { return strcmp(a.name, b.name) < 0; });
        }
        lock(portMAX_DELAY);
        activeCache = next;
        if (selectExisting && newest[0]) snprintf(latestWorker, sizeof(latestWorker), "%s", newest);
        truncated.store(out.truncated);
        if (out.listError != ESP_OK) error.store(out.listError);
        unlock();
    }
    esp_err_t mount() {
        // Octal flash/PSRAM uses GPIO35 on S3. Never reconfigure an active
        // memory pin as SD MISO; preserve the board wiring and report conflict.
        const esp_err_t pins = storagePinsAvailable();
        if (pins != ESP_OK) return pins;
        spi_bus_config_t bus = {};
        bus.mosi_io_num = kMosi; bus.miso_io_num = kMiso; bus.sclk_io_num = kSck;
        bus.quadwp_io_num = -1; bus.quadhd_io_num = -1; bus.max_transfer_sz = kChunkBytes;
        esp_err_t result = spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO);
        if (result != ESP_OK) return result;
        busOwned = true;
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = kHost; host.max_freq_khz = 10000;
        sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
        device.host_id = kHost; device.gpio_cs = static_cast<gpio_num_t>(kCs);
        esp_vfs_fat_mount_config_t config = {};
        config.format_if_mount_failed = false; config.max_files = 4;
        config.allocation_unit_size = 16 * 1024; config.disk_status_check_enable = true;
        result = esp_vfs_fat_sdspi_mount(kMount, &host, &device, &config, &card);
        if (result == ESP_OK && (!mkdirIfMissing(kRecords) || !mkdirIfMissing(kMessages))) result = ESP_FAIL;
        if (result != ESP_OK) {
            if (card) { esp_vfs_fat_sdcard_unmount(kMount, card); card = nullptr; }
            spi_bus_free(kHost); busOwned = false;
            return result;
        }
        mounted.store(true);
        scan(true);
        return ESP_OK;
    }
    void startFile() {
        written = 0; committedSamples = 0; writeFailed = false; encoder.reset();
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            if (maxSequence == UINT32_MAX) break;
            ++maxSequence;
            snprintf(recordPath, sizeof(recordPath), "%s/rec-%08lx.partial.wav", kRecords,
                     static_cast<unsigned long>(maxSequence));
            recordFd = open(recordPath, O_CREAT | O_EXCL | O_WRONLY, 0664);
            if (recordFd >= 0 || errno != EEXIST) break;
        }
        uint8_t header[60];
        if (kStoreAdpcm) adpcmWavHeader(header, 0, 0); else wavHeader(header, 0);
        if (recordFd >= 0 && writeAll(header, kHeaderBytes, false)) return;
        if (recordFd >= 0) { close(recordFd); recordFd = -1; }
        lock(portMAX_DELAY);
        drops.fetch_add(static_cast<uint32_t>(writeSize)); writeHead = writeSize = 0;
        accepting.store(false); occupied.store(false); recordReserved.store(false); mode = Mode::Idle;
        stopRequested = false; error.store(ESP_FAIL);
        unlock();
    }
    void failWrite() {
        lock(portMAX_DELAY);
        writeFailed = true; error.store(ESP_FAIL); accepting.store(false);
        drops.fetch_add(static_cast<uint32_t>(writeSize)); writeHead = writeSize = 0;
        stopRequested = true; complete = false; finalDecision.store(1);
        unlock();
    }
    void finishFile() {
        if (kStoreAdpcm && !writeFailed) {
            const size_t realTail = encoder.pending();
            if (encoder.flush(encodedBlock)) {
                if (written > UINT32_MAX - 52 - sizeof(encodedBlock) ||
                    !writeAll(encodedBlock, sizeof(encodedBlock), true)) failWrite();
                else committedSamples += static_cast<uint32_t>(realTail);
            }
        }
        uint8_t header[60];
        // A failed partial block is excluded from the RIFF payload. Keep the
        // actual trailing file bytes for recovery, never expose them as audio.
        if (kStoreAdpcm) adpcmWavHeader(header, written - written % recording_codec::kBlockBytes, committedSamples);
        else wavHeader(header, written & ~1u);
        bool valid = lseek(recordFd, 0, SEEK_SET) >= 0 && writeAll(header, kHeaderBytes, false);
        if (fsync(recordFd) != 0) valid = false;
        if (close(recordFd) != 0) valid = false;
        recordFd = -1;
        if (!valid) error.store(ESP_FAIL);
        // This VFS is FatFS: f_rename fails with FR_EXIST if the destination
        // exists. Do not pre-create a placeholder (FatFS cannot replace it).
        // Incomplete recordings keep .partial.wav; existing user files survive.
        uint8_t undecided = 0;
        if (complete.load() && !writeFailed && valid && written &&
            finalDecision.compare_exchange_strong(undecided, uint8_t(2))) {
            recordReserved.store(false); // file committed; only cache refresh remains busy
            char finalPath[kPathBytes];
            snprintf(finalPath, sizeof(finalPath), "%s", recordPath);
            char* suffix = finalPath + strlen(finalPath) - 12;
            memcpy(suffix, ".wav", 5);
            const bool renamed = rename(recordPath, finalPath) == 0;
            if (renamed) {
                lock(portMAX_DELAY);
                snprintf(latestWorker, sizeof(latestWorker), "%s", finalPath);
                unlock();
            } else error.store(ESP_FAIL);
        }
        scan(false);
        lock(portMAX_DELAY);
        stopRequested = false; accepting.store(false); occupied.store(false); recordReserved.store(false); mode = Mode::Idle;
        unlock();
    }
    void openReader() {
        readFd = open(readPath, O_RDONLY);
        struct stat st = {};
        WavInfo wav;
        const bool valid = readFd >= 0 && fstat(readFd, &st) == 0 && st.st_size >= 0 &&
            uint64_t(st.st_size) <= UINT32_MAX && inspectWav(readFd, static_cast<uint32_t>(st.st_size), wav) &&
            wav.playable && lseek(readFd, static_cast<off_t>(wav.offset), SEEK_SET) >= 0;
        if (!valid) {
            if (readFd >= 0) { close(readFd); readFd = -1; }
            lock(portMAX_DELAY);
            readError.store(ESP_ERR_INVALID_RESPONSE); readReady.store(false);
            mode = Mode::Idle; occupied.store(false); readerReserved.store(false); closeRequested = false;
            unlock();
            return;
        }
        readRemaining = wav.pcmBytes; readStorageRemaining = wav.bytes;
        readFormat = wav.format; decodedOffset = decodedSize = 0;
        lock(portMAX_DELAY);
        readTotal.store(wav.pcmBytes);
        const char* basename = strrchr(readPath, '/');
        snprintf(nameWorker, sizeof(nameWorker), "%.255s", basename ? basename + 1 : readPath);
        if (!closeRequested) readReady.store(true);
        readFinished.store(wav.pcmBytes == 0);
        unlock();
    }
    void pumpReader() {
        lock(portMAX_DELAY);
        const size_t available = kReadBytes - readSize;
        const bool cancelled = closeRequested;
        unlock();
        if (cancelled || !available || !readRemaining || readFd < 0) return;
        const uint8_t* pcm = chunk;
        size_t count;
        if (readFormat == 0x11) {
            if (decodedOffset == decodedSize) {
                size_t received = 0;
                while (received < sizeof(encodedBlock)) {
                    const ssize_t n = ::read(readFd, encodedBlock + received, sizeof(encodedBlock) - received);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    received += static_cast<size_t>(n);
                }
                if (readStorageRemaining < sizeof(encodedBlock) || received != sizeof(encodedBlock) ||
                    !recording_codec::decodeBlock(encodedBlock, received, decoded, sizeof(decoded))) {
                    readError.store(ESP_ERR_INVALID_RESPONSE); readFinished.store(true); close(readFd); readFd = -1;
                    return;
                }
                readStorageRemaining -= sizeof(encodedBlock);
                decodedOffset = 0;
                decodedSize = std::min<size_t>(sizeof(decoded), readRemaining);
            }
            pcm = decoded + decodedOffset;
            count = std::min(available, decodedSize - decodedOffset) & ~size_t(1);
            decodedOffset += count;
        } else {
            count = std::min<size_t>(std::min(available, kChunkBytes), readRemaining) & ~size_t(1);
            size_t received = 0;
            while (received < count) {
                const ssize_t n = ::read(readFd, chunk + received, count - received);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                received += static_cast<size_t>(n);
            }
            if (received != count) {
                readError.store(ESP_FAIL); readFinished.store(true); close(readFd); readFd = -1;
                return;
            }
            readStorageRemaining -= static_cast<uint32_t>(count);
        }
        readRemaining -= static_cast<uint32_t>(count);
        lock(portMAX_DELAY);
        if (!closeRequested) {
            const size_t tail = (readHead + readSize) % kReadBytes;
            const size_t first = std::min(count, kReadBytes - tail);
            memcpy(readRing + tail, pcm, first); memcpy(readRing, pcm + first, count - first);
            readSize += count;
        }
        if (!readRemaining) readFinished.store(true);
        unlock();
        if (!readRemaining) { close(readFd); readFd = -1; }
    }
    void closeReadFile() {
        if (readFd >= 0) { close(readFd); readFd = -1; }
        lock(portMAX_DELAY);
        readHead = readSize = 0; readReady.store(false); closeRequested = false;
        readRemaining = readStorageRemaining = 0; decodedOffset = decodedSize = 0;
        mode = Mode::Idle; occupied.store(false); readerReserved.store(false);
        unlock();
    }
    void run() {
        const esp_err_t result = mount();
        beginResult.store(result); error.store(result); xSemaphoreGive(begun);
        while (!exiting.load()) {
            bool start, open, scanNow;
            lock(portMAX_DELAY);
            start = startPending; startPending = false;
            open = openPending; openPending = false;
            scanNow = scanPending; scanPending = false;
            unlock();
            if (start) startFile();
            if (open) openReader();
            if (readerReserved.load() && closeRequested.load()) closeReadFile();
            if (recordFd >= 0) {
                const size_t count = popWrite();
                if (count && !writeRecordedPcm(chunk, count)) failWrite();
                lock(portMAX_DELAY);
                const bool finish = stopRequested && writeSize == 0;
                unlock();
                if (finish) finishFile();
            }
            if (readFd >= 0) pumpReader();
            if (scanNow) {
                scan(false);
                lock(portMAX_DELAY); mode = Mode::Idle; occupied.store(false); unlock();
            }
            // Continue without a 20ms pause when another complete chunk is ready.
            lock(portMAX_DELAY);
            const bool more = (recordFd >= 0 && writeSize >= kChunkBytes) ||
                (readFd >= 0 && readSize <= kReadBytes - kChunkBytes);
            unlock();
            if (!more) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        }
        if (recordFd >= 0) {
            // Destruction is cancellation; preserve all already written PCM.
            lock(portMAX_DELAY); complete = false; finalDecision.store(1); writeSize = 0; unlock();
            finishFile();
        }
        if (readFd >= 0) { close(readFd); readFd = -1; }
        if (card) esp_vfs_fat_sdcard_unmount(kMount, card);
        if (busOwned) spi_bus_free(kHost);
        mounted.store(false);
        xSemaphoreGive(finished);
        vTaskDelete(nullptr);
    }
};

BoardRecording::~BoardRecording() {
    if (!impl_) return;
    Impl& state = *impl_;
    if (state.task) {
        state.exiting.store(true); state.wake(); xSemaphoreTake(state.finished, portMAX_DELAY);
    }
    if (state.mutex) vSemaphoreDelete(state.mutex);
    if (state.begun) vSemaphoreDelete(state.begun);
    if (state.finished) vSemaphoreDelete(state.finished);
    heap_caps_free(state.writeRing); heap_caps_free(state.readRing); heap_caps_free(state.chunk);
    state.releaseCaches();
    delete impl_;
}

esp_err_t BoardRecording::begin() {
    if (impl_) return impl_->beginResult.load();
    impl_ = new (std::nothrow) Impl;
    if (!impl_) return ESP_ERR_NO_MEM;
    Impl& state = *impl_;
    const esp_err_t pins = storagePinsAvailable();
    if (pins != ESP_OK) { state.beginResult.store(pins); state.error.store(pins); return pins; }
    state.mutex = xSemaphoreCreateMutex(); state.begun = xSemaphoreCreateBinary(); state.finished = xSemaphoreCreateBinary();
    state.writeRing = static_cast<uint8_t*>(heap_caps_malloc(kWriteBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    state.readRing = static_cast<uint8_t*>(heap_caps_malloc(kReadBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    state.chunk = static_cast<uint8_t*>(heap_caps_malloc(kChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    for (auto& cache : state.cache) {
        cache = static_cast<Impl::Cache*>(heap_caps_calloc(1, sizeof(Impl::Cache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    const auto failedAllocation = [&state](esp_err_t result) {
        // No task exists on this path. Return scarce memory immediately so the
        // remaining board/Agent services can still start without SD capability.
        heap_caps_free(state.writeRing); state.writeRing = nullptr;
        heap_caps_free(state.readRing); state.readRing = nullptr;
        heap_caps_free(state.chunk); state.chunk = nullptr;
        state.releaseCaches();
        if (state.mutex) { vSemaphoreDelete(state.mutex); state.mutex = nullptr; }
        if (state.begun) { vSemaphoreDelete(state.begun); state.begun = nullptr; }
        if (state.finished) { vSemaphoreDelete(state.finished); state.finished = nullptr; }
        state.beginResult.store(result); state.error.store(result);
        return result;
    };
    if (!state.mutex || !state.begun || !state.finished || !state.writeRing || !state.readRing ||
        !state.chunk || !state.cache[0] || !state.cache[1]) {
        return failedAllocation(ESP_ERR_NO_MEM);
    }
    if (xTaskCreate(Impl::runTask, "aiot-sd", 6144, &state, 2, &state.task) != pdPASS) {
        state.task = nullptr; return failedAllocation(ESP_ERR_NO_MEM);
    }
    // Setup only; card probing and the initial cache scan run on the worker.
    xSemaphoreTake(state.begun, portMAX_DELAY);
    const esp_err_t result = state.beginResult.load();
    if (result != ESP_OK) {
        state.exiting.store(true); state.wake(); xSemaphoreTake(state.finished, portMAX_DELAY);
        state.task = nullptr;
        return failedAllocation(result);
    }
    poll();
    return state.beginResult.load();
}

bool BoardRecording::ready() const { return impl_ && impl_->mounted.load(); }
bool BoardRecording::recording() const { return impl_ && impl_->accepting.load(); }
bool BoardRecording::busy() const { return impl_ && impl_->occupied.load(); }
esp_err_t BoardRecording::lastError() const { return impl_ ? impl_->error.load() : ESP_ERR_INVALID_STATE; }
const char* BoardRecording::latestPath() const { return impl_ ? impl_->latestView : ""; }
bool BoardRecording::listTruncated() const { return impl_ && impl_->truncated.load(); }

esp_err_t BoardRecording::startRecording() {
    if (!ready()) return ESP_ERR_INVALID_STATE;
    Impl& state = *impl_;
    if (!state.lock()) return ESP_ERR_TIMEOUT;
    if (state.mode != Impl::Mode::Idle) { state.unlock(); return ESP_ERR_INVALID_STATE; }
    state.mode = Impl::Mode::Record; state.writeHead = state.writeSize = 0;
    state.startPending = true; state.stopRequested = false; state.complete = true; state.finalDecision.store(0);
    state.error.store(ESP_OK); state.recordReserved.store(true); state.accepting.store(true); state.occupied.store(true);
    state.unlock(); state.wake(); return ESP_OK;
}
void BoardRecording::stopRecording(bool complete) {
    if (!impl_) return;
    Impl& state = *impl_;
    if (!state.recordReserved.load()) return;
    state.accepting.store(false);
    if (!complete) {
        uint8_t undecided = 0;
        if (!state.finalDecision.compare_exchange_strong(undecided, uint8_t(1)) && undecided == 2) return;
        state.complete.store(false);
    }
    state.stopRequested.store(true); state.wake();
}
size_t BoardRecording::enqueuePcm(const uint8_t* pcm, size_t bytes) {
    if (!impl_ || !pcm || !bytes) return 0;
    Impl& state = *impl_;
    if ((bytes & 1u) || !state.accepting.load() || !state.lock()) {
        state.drops.fetch_add(static_cast<uint32_t>(bytes)); return 0;
    }
    if (!state.accepting.load()) { state.unlock(); state.drops.fetch_add(static_cast<uint32_t>(bytes)); return 0; }
    const size_t count = std::min(bytes, kWriteBytes - state.writeSize) & ~size_t(1);
    const size_t tail = (state.writeHead + state.writeSize) % kWriteBytes;
    const size_t first = std::min(count, kWriteBytes - tail);
    memcpy(state.writeRing + tail, pcm, first); memcpy(state.writeRing, pcm + first, count - first);
    state.writeSize += count; state.drops.fetch_add(static_cast<uint32_t>(bytes - count));
    state.unlock(); state.wake(); return count;
}
uint32_t BoardRecording::takeDrops() { return impl_ ? impl_->drops.exchange(0) : 0; }
void BoardRecording::poll() {
    if (!impl_ || !impl_->mutex || !impl_->lock()) return;
    snprintf(impl_->latestView, sizeof(impl_->latestView), "%s", impl_->latestWorker);
    snprintf(impl_->nameView, sizeof(impl_->nameView), "%s", impl_->nameWorker);
    impl_->unlock();
}
esp_err_t BoardRecording::refreshList() {
    if (!ready()) return ESP_ERR_INVALID_STATE;
    Impl& state = *impl_;
    if (!state.lock()) return ESP_ERR_TIMEOUT;
    if (state.mode != Impl::Mode::Idle) { state.unlock(); return ESP_ERR_INVALID_STATE; }
    state.mode = Impl::Mode::Scan; state.scanPending = true; state.occupied.store(true);
    state.unlock(); state.wake(); return ESP_OK;
}
esp_err_t BoardRecording::handleList(const uint8_t* request, size_t length,
                                    uint8_t* response, size_t* responseBytes) {
    if (!response || !responseBytes || (length && !request)) return ESP_ERR_INVALID_ARG;
    const size_t capacity = *responseBytes; *responseBytes = 0;
    if ((length != 0 && length != 3 && length != 4) || (length == 4 && request[3] > 1)) return ESP_ERR_INVALID_ARG;
    if (capacity < 7) return ESP_ERR_INVALID_SIZE;
    if (!ready()) return ESP_ERR_INVALID_STATE;
    const uint16_t offset = length ? get16(request) : 0;
    const uint8_t maximum = length ? request[2] : 0;
    const unsigned scope = length == 4 ? request[3] : 0;
    Impl& state = *impl_;
    if (!state.lock()) return ESP_ERR_TIMEOUT;
    const Impl::Cache& cache = *state.cache[state.activeCache];
    if (cache.listError != ESP_OK) { state.unlock(); return cache.listError; }
    const uint16_t total = cache.count[scope];
    uint16_t position = std::min(offset, total), count = 0;
    size_t used = 2;
    while (position < total && (!maximum || count < maximum)) {
        const Impl::Entry& entry = cache.entries[scope][position];
        const size_t nameBytes = strlen(entry.name), need = 1 + nameBytes + 12;
        if (used + need + 5 > capacity) break;
        response[used++] = static_cast<uint8_t>(nameBytes);
        memcpy(response + used, entry.name, nameBytes); used += nameBytes;
        put32(response + used, entry.bytes); used += 4;
        put32(response + used, entry.mtime); used += 4;
        put32(response + used, entry.durationMs); used += 4;
        ++position; ++count;
    }
    put16(response, count);
    const bool more = position < total;
    response[used++] = more ? 1 : 0;
    put16(response + used, total); used += 2;
    put16(response + used, more ? position : 0); used += 2;
    state.unlock(); *responseBytes = used; return ESP_OK;
}
esp_err_t BoardRecording::openLatestPcm() {
    if (!ready()) return ESP_ERR_INVALID_STATE;
    Impl& state = *impl_;
    if (!state.lock()) return ESP_ERR_TIMEOUT;
    if (state.mode != Impl::Mode::Idle || !state.latestWorker[0]) {
        state.unlock(); return ESP_ERR_INVALID_STATE;
    }
    state.mode = Impl::Mode::Reader; state.openPending = true; state.closeRequested = false;
    state.readHead = state.readSize = 0; state.nameWorker[0] = 0;
    state.readReady.store(false); state.readFinished.store(false); state.readTotal.store(0);
    state.readError.store(ESP_OK); state.readerReserved.store(true); state.occupied.store(true);
    snprintf(state.readPath, sizeof(state.readPath), "%s", state.latestWorker);
    state.unlock(); state.wake(); return ESP_OK;
}
bool BoardRecording::readerReady() const { return impl_ && impl_->readReady.load(); }
uint32_t BoardRecording::readerBytes() const { return impl_ ? impl_->readTotal.load() : 0; }
const char* BoardRecording::readName() const { return impl_ ? impl_->nameView : ""; }
esp_err_t BoardRecording::readerError() const { return impl_ ? impl_->readError.load() : ESP_ERR_INVALID_STATE; }
size_t BoardRecording::readPcm(uint8_t* destination, size_t capacity) {
    if (!impl_ || !destination || capacity < 2 || !impl_->readReady.load() || !impl_->lock()) return 0;
    Impl& state = *impl_;
    const size_t count = std::min(capacity, state.readSize) & ~size_t(1);
    const size_t first = std::min(count, kReadBytes - state.readHead);
    memcpy(destination, state.readRing + state.readHead, first);
    memcpy(destination + first, state.readRing, count - first);
    state.readHead = (state.readHead + count) % kReadBytes; state.readSize -= count;
    state.unlock(); state.wake(); return count;
}
bool BoardRecording::readerEof() const {
    if (!impl_ || !impl_->readFinished.load() || !impl_->lock()) return false;
    const bool empty = impl_->readSize == 0;
    impl_->unlock(); return empty;
}
void BoardRecording::closeReader() {
    if (!impl_) return;
    Impl& state = *impl_;
    if (!state.readerReserved.load()) return;
    state.readReady.store(false); state.closeRequested.store(true); state.wake();
}
