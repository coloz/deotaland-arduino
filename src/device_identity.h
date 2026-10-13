#pragma once
// Device identity: BLE sends this UUIDv4 in the 0x01 RequestDeviceInfo response.
//
// Stored in NVS "agent_link"/"dev_uuid". It survives OTA and factory reset: agent_link_forget()
// clears bonds but not this, so the App keeps recognising the unit.
//
// esp_fill_random() requires active BT RF for this library; before that, identical
// units generate identical values. Only mint once a transport has started.

#include <stdbool.h>
#include <stdint.h>
#include "agent_link_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load the identity from NVS, creating it if needed. Idempotent and thread-safe.
 * @param allow_mint Create one if none exists. Pass true only when RF is running.
 * @return ESP_OK when loaded; ESP_ERR_NOT_FOUND if none exists and @p allow_mint is false; an NVS
 *         error if a new identity could not be stored (it is then not used).
 */
void dev_identity_override(const uint8_t* uuid);
esp_err_t dev_identity_load(bool allow_mint);

/** @brief Canonical 36-char lowercase string, or "" if not loaded. */
const char* dev_identity_sn(void);
/** Copy only an already-loaded identity. Never opens NVS or waits for its mutex.
 * Caller retains its previous snapshot when false (unloaded or currently busy). */
bool dev_identity_try_sn(char out[37]);

/** @brief The 16 raw bytes, RFC 4122 order. Returns false if not loaded. */
bool dev_identity_uuid(uint8_t out[16]);

#ifdef __cplusplus
}
#endif
