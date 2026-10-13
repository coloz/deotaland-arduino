// ============================================================================
// agent_link — Connectivity Layer for Deotaland Agent Platform
// ============================================================================
// This module provides a reusable connectivity layer that allows devices
// to access the Deotaland Agent platform with minimal integration effort

// Design Philosophy:
//   - New hardware simply "declares capabilities + registers callbacks"
//   - Completely abstracts BLE/transport protocol details
//   - Agent → Device: via agent_output_cb_t (speech/display/vibration/actuators)
//   - Device → Agent: unified streams plus events, readings and battery reports
//   - Only handles "transport + protocol + capability routing"
//   - Hardware actions are implemented in device callbacks → naturally cross-platform
//
// See agent_link_caps.h for capability flags, agent_link_io.h for generic I/O, and
// agent_link_ota.h for opt-in App-driven firmware upgrades
// ============================================================================

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "agent_link_platform.h"
#include "agent_link_caps.h"
#include "agent_link_io.h"
#include "agent_link_stream.h"
#include "agent_link_transport.h"
#include "agent_link_playback.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol version identifier */
#define AGENT_LINK_PROTO_VERSION 1

/**
 * @brief Bytes of response data an agent_output_cb_t::on_command handler may return.
 * @note Deliberately small: a command response shares one control frame with its header, and the
 *       negotiated MTU is not known to the board. A command whose answer can grow past this must
 *       define paging in its own payload.
 */
#define AGENT_LINK_CMD_RESP_MAX 128

// Consistent identity limits across device-info, DIS and OTA (UTF-8 byte counts).
#define AGENT_LINK_MODEL_MAX 39u
#define AGENT_LINK_FIRMWARE_MAX 31u
#define AGENT_LINK_MANUFACTURER_MAX 63u

// Bounded manifest storage. Configure for all library translation units.
#ifndef AGENT_LINK_MANIFEST_BYTES
#define AGENT_LINK_MANIFEST_BYTES 8192u
#endif

/**
 * @brief Agent → Device output callbacks
 *
 * These callbacks are invoked by the SDK when the Agent platform sends
 * commands or data to the device. All hardware operations should be
 * implemented within these callbacks.
 *
 * @note All callbacks are optional — set to NULL if not applicable.
 * @note Callbacks are dispatched only by agent_link_poll() on the loop thread.
 *       Return quickly; no waits, recursive poll or lifecycle calls from callbacks.
 */
typedef struct {
    /**
     * @brief Audio output: TTS stream from Agent
     * @param pcm16 PCM16 audio data (16kHz, mono)
     * @param bytes Size of data in bytes
     * @param ctx   User context pointer
     * @note Arrives in chunks; on_audio_end is called when stream finishes
     */
    void (*on_audio_out)(const uint8_t* pcm16, size_t bytes, void* ctx);

    /** @brief Called when audio stream ends */
    void (*on_audio_end)(void* ctx);

    /**
     * @brief Display text
     * @param utf8 UTF-8 encoded text string
     * @param ctx  User context
     */
    void (*on_show_text)(const char* utf8, void* ctx);
    /**
     * @brief Display image
     * @param rgb565_be Big-endian RGB565 image data
     * @param w        Image width in pixels
     * @param h        Image height in pixels
     * @param ctx      User context
     * @note Reserved by the upstream interface; no built-in image downlink decoder.
     */
    void (*on_show_image)(const uint8_t* rgb565_be, uint16_t w, uint16_t h, void* ctx);

    /**
     * @brief Video downlink: encoded video frame from Agent
     * @param frame Encoded video frame (format negotiated by capabilities)
     * @param bytes Frame size in bytes
     * @param ctx   User context
     * @note Reserved by the upstream interface; built-in video downlink is not implemented.
     */
    void (*on_video_out)(const uint8_t* frame, size_t bytes, void* ctx);

    /** @brief Haptic feedback (vibration motor) */
    void (*on_haptic)(uint32_t duration_ms, void* ctx);

    /** @brief LED control (RGB color) */
    void (*on_led)(uint32_t rgb, void* ctx);

    /** @brief Reserved actuator callback; use register_io for routed actuators. */
    void (*on_actuate)(uint16_t channel, int32_t value, void* ctx);

     /**
     * @brief Agent list from platform
     * @param json_utf8 JSON array of available Agents (UTF-8)
     * @param ctx       User context
     * @note Reserved by the upstream interface; agent-list dispatch is not implemented.
     */
    void (*on_agent_list)(const char* json_utf8, void* ctx);
    /**
     * @brief Command/query handler (for commands that require a response)
     * @param cmd     Command ID
     * @param payload Command payload
     * @param len     Payload length
     * @param resp    Response buffer to fill
     * @param resp_cap Response buffer capacity
     * @param resp_len Output: actual response length
     * @param ctx     User context
     * @return true if command recognized and handled; false otherwise
     * @note Return true → the SDK answers status=0 with your data.
     *       Return false → the SDK answers 1001 UnknownCommand. It never invents a success for a
     *       command nobody implemented: an App told "done" for something that did not happen is
     *       worse off than one told the device cannot do it, which it can degrade around.
     * @note Built-in protocol commands take priority. This callback handles other
     *       commands before the UnknownCommand fallback; it cannot override built-ins.
     * @note @p resp_cap is AGENT_LINK_CMD_RESP_MAX bytes. A larger answer must be paged by the
     *       command's own protocol.
     */
    bool (*on_command)(uint16_t cmd, const uint8_t* payload, size_t len,
                       uint8_t* resp, size_t resp_cap, size_t* resp_len, void* ctx);


    /**
     * @brief Start/stop microphone capture on request from the Agent (App-initiated "listen").
     * @param start  true = begin capturing and streaming mic audio; false = stop.
     * @param max_ms Suggested max capture duration in ms (0 = until stopped); board-specific.
     * @param ctx    User context.
     * @note Optional; set only if AGENT_CAP_MIC is advertised. On start, the board opens an
     *       AGENT_STREAM_AUDIO stream and pumps its mic into it. Triggered by command 0x3C/0x3D.
     */
    void (*on_listen)(bool start, uint32_t max_ms, void* ctx);

    void* ctx;  ///< Opaque pointer passed through to all callbacks
} agent_output_cb_t;

/**
 * @brief Connection state change callback
 * @param state New connection state
 * @param ctx   User context
 */
typedef void (*agent_state_cb_t)(agent_state_t state, void* ctx);

/**
 * @brief Transport backend selection
 *
 * Default is BLE (0). CUSTOM injects a board-specific implementation.
 */
typedef enum {
    AGENT_TRANSPORT_BLE  = 0,  // GATT control/VOICE, optional CoC AUDIO/IMAGE/TTS/OTA
    AGENT_TRANSPORT_CUSTOM = 3, // Injected backend; no ESP32 dependency
} agent_transport_kind_t;

typedef enum {
    AGENT_LINK_PHASE_IDLE = 0,
    AGENT_LINK_PHASE_SETUP,
    AGENT_LINK_PHASE_CONNECTING,
    AGENT_LINK_PHASE_PAIRING,
    AGENT_LINK_PHASE_BLOCKED,
    AGENT_LINK_PHASE_CONNECTED,
    AGENT_LINK_PHASE_READY,
} agent_link_phase_t;

typedef struct agent_link_status_s {
    agent_link_phase_t phase;
    agent_transport_kind_t transport;
    char title[32];
    char hint[112];
    int detail;
} agent_link_status_t;

/** Delivered by poll(). Copy the snapshot if needed later.
 * Intermediate display updates may be coalesced; connection events remain separate.
 */
typedef void (*agent_link_status_cb_t)(const agent_link_status_t* status, void* ctx);

/**
 * @brief Agent link configuration
 *
 * Passed to agent_link_init() to configure the device's connection.
 */
typedef struct {
    const char*              device_name; ///< BLE advertising name (required)
    uint32_t                 caps;         ///< agent_cap_t bitmask (required)
    const agent_output_cb_t* output;       ///< Agent→Device callbacks (NULL for pure sensor devices)
    agent_state_cb_t         on_state;     ///< Connection state callback (can be NULL)
    void*                    state_ctx;    ///< Context passed to on_state
    agent_transport_kind_t   transport;    ///< Transport backend (default 0 = BLE)
    const char*              manufacturer;  ///< Manufacturer (max 63 UTF-8 bytes); NULL → "Deotaland"
    /**
     * @brief Hardware model (max 39 UTF-8 bytes); NULL → device_name.
     * @note This is the string an incoming OTA image must claim, so the App cannot flash a build
     *       for other hardware onto this board. Boards that share a PCB must share this value.
     */
    const char*              model;
    /**
     * @brief Firmware revision (max 31 UTF-8 bytes); NULL → "2.0.0".
     *        Products using OTA must explicitly set their real firmware revision.
     */
    const char*              firmware_rev;
    agent_transport_t* custom_transport; ///< Required for CUSTOM, borrowed for the lifetime.
    bool enable_ota; ///< Opt-in ESP32 OTA; requires a CoC-capable BLE backend and OTA partitions.
    const uint8_t* device_id; ///< Optional stable 16-byte ID, copied. Supply for custom hardware.
    agent_link_status_cb_t on_status;
    void* status_ctx;
} agent_link_config_t;

//=================================================================
// Lifecycle Management
//=================================================================

/** Initialize the agent link module */
esp_err_t     agent_link_init(const agent_link_config_t* cfg);

/** Start the connection (BLE advertising or the custom backend) */
esp_err_t     agent_link_start(void);
/** True after asynchronous backend teardown and any cancelled OTA flash work
 * finish, so begin may be retried. An idle preallocated OTA task is allowed. */
bool agent_link_can_begin(void);

/** Stop the connection and release resources */
void          agent_link_stop(void);

/** Get current connection state */
agent_state_t agent_link_state(void);

/** Snapshot; IDLE before init. Does not invoke callbacks. ESP32 access is locked;
 * other platforms follow the library's single-loop-thread contract. */
void agent_link_get_status(agent_link_status_t* out);
/** Canonical UUID string; NULL until a persisted or explicitly supplied ID exists.
 * The returned string lasts until reconfiguration.
 * New ESP32 identities are minted only after RF startup and successful NVS storage.
 */
const char* agent_link_device_id(void);
/** Forget BLE bonds, keeping the device UUID.
 * Call from the loop after start. BLE writes NVS synchronously (like lifecycle
 * calls). CUSTOM returns NOT_SUPPORTED.
 */
esp_err_t agent_link_forget(void);

/** Process at most max_events RX work units using AGENT_LINK_POLL_BUDGET_US.
 * Large audio events may span multiple work units. Service/status callbacks are
 * additional bounded steps within the cooperative time budget.
 * Call on every loop iteration (usually max_events=4). No sleep or I/O wait.
 * Single instance: lifecycle, registration and push APIs share this loop thread.
 * begin/end initialize or tear down platform drivers and are not real-time calls.
 */
size_t agent_link_poll(size_t max_events);
/** Cooperative time budget, checked between work units; 0 disables time limit.
 * A single callback/backend call cannot be preempted. max_events also bounds
 * audio slices (at most 512 PCM bytes or one 256-byte ADPCM block per slice).
 */
size_t agent_link_poll_budget(size_t max_events, uint32_t max_us);
#ifndef AGENT_LINK_POLL_BUDGET_US
#define AGENT_LINK_POLL_BUDGET_US 2000u
#endif
/** Stop and clear all borrowed I/O descriptors, callbacks and transport bindings. */
void agent_link_deinit(void);
typedef struct {
    uint32_t rx_dropped;
    uint32_t tx_retries;
    uint32_t rx_high_water;
    esp_err_t last_error;
    uint32_t poll_calls;
    uint32_t poll_last_us;
    uint32_t poll_max_us;
    uint32_t poll_budget_exhaustions;
    uint32_t control_backpressure;
    uint32_t playback_backpressure;
    uint32_t reading_coalesces;
    uint32_t manifest_build_steps;
    uint32_t manifest_fragments;
} agent_link_stats_t;
void agent_link_get_stats(agent_link_stats_t* out);



// ============================================================================
// Device → Agent: Input / Event / Status
// ============================================================================
// These functions are called by the device to report data to the Agent platform.
// They are safely ignored if the connection is not ready.
// ============================================================================


/**
 * @brief Declare how many milliseconds of downlink audio this board can hold for playback.
 *
 * The App sends a spoken reply as fast as the link allows and only stops when the device says
 * so, so the SDK has to throttle it to whatever the board can actually buffer.
 * configurePlayback's queued callback supplies actual unplayed bytes; without
 * that callback, the SDK estimates queue occupancy from elapsed time.
 *
 * Call once during board setup with the size of your play buffer. The default is deliberately
 * small (400ms). The application must still consume PCM promptly and size its
 * own queue for transport latency.
 *
 * @param playable_ms Milliseconds of PCM16 the board's play buffer holds (e.g. a 16KB buffer
 *                    at 16kHz/16-bit/mono = 512).
 * @note This does not allocate a playback buffer or guarantee lossless playback.
 */
void agent_link_playback_set_buffer_ms(uint32_t playable_ms);

/**
 * @brief Push a device→Agent event
 *
 * The event_id on the wire is the agent_event_t value. Use AGENT_EVT_CUSTOM (0x64) for a board-private packet
 * the board defines its own payload and the App matches on event_id 0x64
 * a dropped frame is not resent, so don't rely on it for state that must never desync
 * carry the explicit state in the payload rather than a bare toggle.
 * @param type Event type (from agent_event_t; AGENT_EVT_CUSTOM for board-private packets)
 * @param data Event payload (can be NULL if len == 0)
 * @param len  Payload length
 */
esp_err_t agent_link_push_event(agent_event_t type, const uint8_t* data, size_t len);

/**
 * @brief Push firmware-authored text for the Agent to treat as a prompt
 *
 * Sends AGENT_EVT_PROMPT (event 0x04) as a single control-plane event (Notify 0xFFC4); the
 * App forwards the text verbatim to the Agent, as if the user had said/typed it. For short,
 * discrete strings the firmware composes itself — canned phrases, sensor-triggered context,
 * button macros. A prompt must fit in one control frame, so oversized text is
 * rejected rather than truncated. Each call is a separate prompt; fragments
 * from separate calls are not automatically reassembled by this protocol.
 *
 * @param utf8 NUL-terminated UTF-8 text (the terminator itself is not sent).
 * @return ESP_OK once queued for send.
 *         ESP_ERR_INVALID_ARG if utf8 is NULL or empty.
 *         ESP_ERR_INVALID_SIZE if it exceeds the current single-frame budget: BLE = negotiated
 *         ATT_MTU − 9, capped at 480B (≈238B at the recommended MTU 247; only ≈14B if the App
 *         has not yet performed MTU exchange). CUSTOM uses its control MTU.
 * @note a dropped frame is not resent.
 */
esp_err_t agent_link_push_prompt(const char* utf8);

/** Maximum bytes in one event/prompt at the CURRENT control MTU; 0 unless READY.
 * This is a size limit, not free queue space or a delivery guarantee. UTF-8 uses
 * bytes, not characters. For readings subtract strlen(id)+2 from this capacity. */
size_t agent_link_event_payload_capacity(void);

/**
 * @brief Report battery status
 * @param percent  Battery percentage (0-100)
 * @param charging true if charging
 * @note SDK internally deduplicates — only pushes when value changes /
 *       charging state changes / low-battery edge (<5%) is detected.
 *       If send fails, the successful-send cache stays unchanged so the next
 *       call retries. Current measurement and BAS cache update independently.
 *       First call after connection forces a retransmission for initial sync.
 */
esp_err_t agent_link_report_battery(uint8_t percent, bool charging);

/**
 * @brief Report the user's selected Agent ID
 * @param agent_id The selected Agent ID
 * @note Reserved by upstream. A ready link returns NOT_SUPPORTED, without
 *       emitting an event; an empty ID is INVALID_ARG and an offline link is
 *       INVALID_STATE. This call does not select an Agent on the platform.
 */
esp_err_t agent_link_report_selected_agent(const char* agent_id);

#ifdef __cplusplus
}
#endif
