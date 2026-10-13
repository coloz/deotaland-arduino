// Device identity. See device_identity.h.

#include "device_identity.h"

#include <cstdio>
#include <cstring>
#if defined(ARDUINO_ARCH_ESP32)
#include <mutex>
#endif

#include "agent_link_log.h"
#if defined(ARDUINO_ARCH_ESP32)
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace {
constexpr const char* TAG = "agent_link.ident";

// Canonical location, the one the BLE path has always used.
constexpr const char* kNs      = "agent_link";
constexpr const char* kKeyUuid = "dev_uuid";

#if defined(ARDUINO_ARCH_ESP32)
std::mutex s_mtx;
struct Guard { std::lock_guard<std::mutex> lock; Guard(): lock(s_mtx) {} };
#else
struct Guard {};
#endif
bool       s_loaded = false;
bool       s_override = false;
uint8_t    s_uuid[16] = {};
char       s_sn[37] = {};

void Format(const uint8_t b[16], char* out) {
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

#if defined(ARDUINO_ARCH_ESP32)
esp_err_t ReadCanonical(uint8_t out[16]) {
    nvs_handle_t h;
    esp_err_t result = nvs_open(kNs,NVS_READONLY,&h);
    if (result != ESP_OK) return result;
    size_t len = 16;
    result = nvs_get_blob(h,kKeyUuid,out,&len);
    nvs_close(h);
    return result == ESP_OK && len != 16 ? ESP_ERR_INVALID_SIZE : result;
}

esp_err_t WriteCanonical(const uint8_t b[16]) {
    nvs_handle_t h;
    esp_err_t r = nvs_open(kNs, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_blob(h, kKeyUuid, b, 16);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

#endif
void Take(const uint8_t b[16]) {
    memcpy(s_uuid, b, 16);
    Format(s_uuid, s_sn);
    s_loaded = true;
}
}  // namespace

void dev_identity_override(const uint8_t* uuid) {
    Guard guard; (void)guard;
    if (uuid) { Take(uuid); s_override = true; }
    else if (s_override) { s_loaded = false; s_override = false; s_sn[0] = 0; }
}

esp_err_t dev_identity_load(bool allow_mint) {
    Guard guard; (void)guard;
    if (s_loaded) return ESP_OK;

    #if defined(ARDUINO_ARCH_ESP32)
    // Preserve application data: propagate NVS recovery errors, never erase it.
    esp_err_t r = nvs_flash_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "nvs init: %s", esp_err_to_name(r));
        return r;
    }

    uint8_t canon[16] = {};
    const esp_err_t canonical_result = ReadCanonical(canon);
    if (canonical_result != ESP_OK && canonical_result != ESP_ERR_NVS_NOT_FOUND) return canonical_result;
    const bool have_canon = canonical_result == ESP_OK;
    if (have_canon) {
        Take(canon);
        ESP_LOGI(TAG, "device identity %s", s_sn);
        return ESP_OK;
    }

    if (!allow_mint) return ESP_ERR_NOT_FOUND;

    uint8_t fresh[16];
    esp_fill_random(fresh, sizeof fresh);
    fresh[6] = static_cast<uint8_t>((fresh[6] & 0x0F) | 0x40);   // version 4
    fresh[8] = static_cast<uint8_t>((fresh[8] & 0x3F) | 0x80);   // variant RFC 4122
    r = WriteCanonical(fresh);
    if (r != ESP_OK) {
        // Don't use an identity the next boot would not reproduce.
        ESP_LOGE(TAG, "first boot: identity could not be persisted (%s) — not using a throwaway one",
                 esp_err_to_name(r));
        return r;
    }
    Take(fresh);
    ESP_LOGI(TAG, "first boot: minted device identity %s", s_sn);
    return ESP_OK;
    #else
    (void)allow_mint;
    return ESP_ERR_NOT_FOUND;
    #endif
}

const char* dev_identity_sn(void) {
    Guard guard; (void)guard;
    return s_loaded ? s_sn : "";
}

bool dev_identity_try_sn(char out[37]) {
    if (!out) return false;
#if defined(ARDUINO_ARCH_ESP32)
    std::unique_lock<std::mutex> lock(s_mtx,std::try_to_lock);
    if (!lock.owns_lock()) return false;
#endif
    if (!s_loaded) return false;
    memcpy(out,s_sn,37); return true;
}

bool dev_identity_uuid(uint8_t out[16]) {
    Guard guard; (void)guard;
    if (!s_loaded || !out) return false;
    memcpy(out, s_uuid, 16);
    return true;
}
