#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "agent_link_platform.h"
#include "agent_link_stream.h"
#ifdef __cplusplus
extern "C" {
#endif
struct agent_link_status_s;

// RX data is copied before returning. Full queue -> ESP_ERR_NO_MEM; retry later.
// Non-ESP32 adapters call these from poll(), not from interrupts or other threads.
// Serialize connection/data callbacks: report disconnect before a replacement peer
// can submit data. Discard old outbound work on disconnect; queued sends belong to
// the peer that accepted them and must never be replayed to a later connection.
typedef struct {
    esp_err_t (*on_ctrl)(const uint8_t* data, size_t len);
    esp_err_t (*on_stream)(agent_stream_t type, const uint8_t* data, size_t len);
    void (*on_connection)(bool connected);
    void (*on_ready)(void);
    void (*on_status)(const struct agent_link_status_s* status);
} agent_transport_callbacks_t;

/**
 * @brief Transport backend operation table
 * @note start/stop/send_ctrl/is_ready/bind are required for custom backends.
 *       Stream functions are optional as a complete set; NULL means unsupported.
 *       BLE control plane uses GATT; bulk media use L2CAP CoC.
 */
typedef struct agent_transport_s {
    /**
     * @brief Start the transport (advertise/connect)
     * @param impl Backend private context
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t (*start)(void* impl);

    /**
     * @brief Stop the transport (disconnect/release)
     * @param impl Backend private context
     */
    void      (*stop)(void* impl);

    /**
     * @brief Send a control frame (command/response/event)
     * @param impl  Backend private context
     * @param frame Frame buffer (header + payload)
     * @param len   Frame length
     * @return ESP_OK on success, error code otherwise
     * @note The control channel is reliable and ordered
     */
    esp_err_t (*send_ctrl)(void* impl, const uint8_t* frame, size_t len);

    /**
     * @brief Start a data‑plane stream session
     * @param impl     Backend private context
     * @param type     Stream type (voice/audio/image/video/file)
     * @param meta     Optional metadata (e.g., codec, sample rate)
     * @param meta_len Length of meta
     * @return ESP_OK on success, error code otherwise
     * @note The backend allocates a session ID and resets sequence numbers
     */
    esp_err_t (*stream_start)(void* impl, agent_stream_t type, const uint8_t* meta, size_t meta_len);

    /**
     * @brief Send a chunk of data over an active stream
     * @param impl Backend private context
     * @param type Stream type
     * @param data Data buffer
     * @param len  Data length
     * @return ESP_OK on success, error code otherwise
     * @note For BLE voice, this uses GATT Notify 0xFFA1 (event 0x40 VoiceChunk)
     *       AUDIO and IMAGE use separate L2CAP CoC channels on BLE.
     */
    esp_err_t (*send_stream)(void* impl, agent_stream_t type, const uint8_t* data, size_t len);

    /**
     * @brief End a data‑plane stream session
     * @param impl     Backend private context
     * @param type     Stream type
     * @param complete true if the stream ended normally, false if aborted
     * @return ESP_OK on success, error code otherwise
     */
    esp_err_t (*stream_end)(void* impl, agent_stream_t type, bool complete,
                            const uint8_t* meta, size_t meta_len);

    /**
     * @brief Check if the transport is ready for application traffic
     * @param impl Backend private context
     * @return true if ready, false otherwise
     */
    bool      (*is_ready)(void* impl);

    void*     impl;  // Backend private context

    // Optional bounded service step, called once per agent_link_poll(). No waits.
    void (*poll)(void* impl);
    // Required for custom backends. Copy callbacks; NULL unbinds on deinit.
    void (*bind)(void* impl, const agent_transport_callbacks_t* callbacks);
    // Maximum complete control frame, including its 6-byte header. Default 20.
    size_t (*ctrl_mtu)(void* impl);
    // Optional setup-time resource preparation; never called automatically by poll.
    esp_err_t (*prepare_streams)(void* impl, uint32_t stream_mask);
    // Optional local transport progress, including drain after stream_end returns.
    esp_err_t (*stream_status)(void* impl, agent_stream_t kind, agent_stream_status_t* out);
} agent_transport_t;

/**
 * @brief Get the BLE transport instance
 * @return Pointer to the BLE transport operation table
 * @note Control plane is fully implemented.
 *       Data plane (send_stream) is ready for voice via GATT Notify,
 *       AUDIO/IMAGE via L2CAP CoC (bundled NimBLE on Arduino ESP32); FILE/VIDEO are unsupported
 */
agent_transport_t* agent_transport_ble(void);
bool agent_transport_ble_get_mac(uint8_t out[6]);
esp_err_t agent_transport_ble_forget(void);
bool agent_transport_ble_active(void);
bool agent_transport_ble_l2cap_ready(void);
void agent_transport_ble_set_data_receiver(size_t (*receive)(const uint8_t*, size_t),
                                           bool (*ready)(void));

/**
 * @brief Set BLE advertising name
 * @param name Device name (used in advertising and GAP)
 * @note Called by agent_link_init() with the device name from config
 */
void agent_transport_ble_set_name(const char* name);

/**
 * @brief Register callback for incoming control frames
 * @param cb Function called when a control frame is received (0xFFC1 writes)
 * @note Called by agent_link_init() to wire the core's OnCtrlFrame
 */
void agent_transport_ble_set_recv(void (*cb)(const uint8_t* data, size_t len));

/**
 * @brief Register callback for connection state changes
 * @param cb Function called when BLE connects or disconnects
 * @note Called by agent_link_init() to wire the core's OnConn
 */
void agent_transport_ble_set_conn(void (*cb)(bool connected));

/**
 * @brief Register callback for incoming data frames
 * @param cb Function called when a data frame is received
 * @note Called by agent_link_init() to wire the core's OnStreamRecv
 */
void agent_transport_ble_set_stream_recv(void (*cb)(agent_stream_t type, const uint8_t* data, size_t len));

/**
 * @brief Register callback for transport readiness
 * @param cb Function called when the transport becomes ready
 * @note Called by agent_link_init() to wire the core's OnReady
 */
void agent_transport_ble_set_ready(void (*cb)(void));

/**
 * @brief Get the current ATT MTU.
 * @return MTU size in bytes, or 0 if not connected.
 * @note Used by the core to adapt manifest fragment size.
 */
uint16_t agent_transport_ble_att_mtu(void);

/**
 * @brief Set device information for the standard Device Information Service (0x180A).
 * @param manufacturer Manufacturer name (0x2A29), NULL/empty keeps default.
 * @param model        Model number (0x2A24), NULL/empty keeps default.
 * @param firmware_rev Firmware revision (0x2A26), NULL/empty keeps default.
 * @note Called once during init.
 */
void agent_transport_ble_set_device_info(const char* manufacturer, const char* model, const char* firmware_rev);

/**
 * @brief Update the battery level in the standard Battery Service (0x180F, 0x2A19).
 * @param percent Battery percentage (0–100).
 * @note Stores the value for read and notifies if connected.
 *       Called by the core when battery changes.
 */
void agent_transport_ble_update_battery(uint8_t percent);

#ifdef __cplusplus
}
#endif
