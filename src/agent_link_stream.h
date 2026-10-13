#pragma once
/**
 * @file agent_link_stream.h
 * @brief Device → Agent data-plane streams: one API for every kind of bulk payload.
 *
 * @details The control plane (commands, events, small readings) moves a few hundred bytes at a
 *          time and is covered by agent_link.h / agent_link_io.h. Anything bulk — speech, an
 *          audio recording, a snapshot, video, a file — goes through this one interface instead:
 *
 *              open(kind, opts) -> write(...) x N -> close(complete)
 *
 *          The @ref agent_stream_t kind is the only thing that varies. The transport decides how
 *          that kind travels (BLE uses GATT notifications for one kind and separate L2CAP
 *          channels for the others), and it owns
 *          session ids, framing, slicing and backpressure. Callers just hand over bytes.
 *
 *          The SDK does not interpret those bytes. It will not encode, transcode or compress —
 *          whatever a caller writes is what the App receives, so the payload format is an
 *          agreement between the board and the App. Built-in BLE VOICE requires
 *          PCM16 and accepts only even byte lengths; IMAGE metadata is validated.
 *
 * @note Streams of different kinds are independent and may run concurrently. Only one stream of
 *       any given kind may be open at a time; opening a second returns the same handle.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "agent_link_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What a stream carries.
 *
 * @note The distinction between VOICE and AUDIO is where the audio is *going*, not what it sounds
 *       like or what anyone does with it downstream. Both are plain PCM as far as this layer is
 *       concerned.
 */
typedef enum {
    /**
     * Speech meant for the Agent. The App relays it on as if the user had spoken, so this is the
     * channel behind "talk to the agent". Sized for utterances: it rides the low-latency control
     * path (BLE: GATT notifications), which needs no side channel to be open but is not built for
     * sustained throughput.
     */
    AGENT_STREAM_VOICE = 0,

    /**
     * Audio for the App itself — of any length, and whatever the App wants to do with it is the
     * App's business. Rides the bulk data path (BLE: an L2CAP channel the App must have opened),
     * so it sustains real-time capture where VOICE would not.
     */
    AGENT_STREAM_AUDIO,

    /** A still image. */
    AGENT_STREAM_IMAGE,

    /** Encoded video frames. Unsupported by the built-in BLE backend. */
    AGENT_STREAM_VIDEO,

    /** An arbitrary file from the device's storage. */
    AGENT_STREAM_FILE,

    AGENT_STREAM_KIND_COUNT,
} agent_stream_t;


/** @brief How the bytes in a stream are encoded, when the kind allows a choice. */
typedef enum {
    AGENT_ENC_RAW  = 0,  ///< Uncompressed: PCM16 for audio, big-endian RGB565 for images
    AGENT_ENC_JPEG = 1,  ///< JPEG (images)
} agent_encoding_t;

/**
 * @brief Optional description of a stream, passed at open time.
 *
 * @note Zero-initialize and set only what the kind needs; everything here is optional and the
 *       fields a kind does not use are ignored. IMAGE requires opts and nonzero width/height.
 *       JPEG requires total_bytes; RAW with zero total_bytes uses width*height*2.
 *       A caller that knows @ref total_bytes up front
 *       should say so — it is the only way the App can show progress.
 */
typedef struct {
    const char*      name;         ///< Short label carried to the App (may be NULL)
    agent_encoding_t encoding;     ///< Payload encoding (default AGENT_ENC_RAW)
    uint16_t         width;        ///< Pixel width, for IMAGE / VIDEO (0 if not applicable)
    uint16_t         height;       ///< Pixel height, for IMAGE / VIDEO
    uint32_t         total_bytes;  ///< Total payload size when known in advance; 0 = unknown
} agent_stream_opts_t;

/** @brief Session token; never dereference. A closed token cannot operate a newly
 * opened session, even after reconnect or reset. Obtain it from stream_open(). */
typedef struct agent_stream_session* agent_stream_handle_t;

typedef enum {
    AGENT_STREAM_UNAVAILABLE = 0,
    AGENT_STREAM_IDLE,
    AGENT_STREAM_OPEN,
    AGENT_STREAM_DRAINING,
    AGENT_STREAM_COMPLETED,
    AGENT_STREAM_FAILED,
} agent_stream_state_t;

/** Local transport snapshot; sent means submitted to the local stack, not peer ACK.
 * Writable capacity is advisory: a subsequent write can still return NO_MEM. */
typedef struct {
    agent_stream_state_t state;
    size_t queued_bytes;
    size_t writable_bytes;
    uint32_t accepted_bytes;
    uint32_t sent_bytes;
    uint32_t rejected_writes;
    esp_err_t last_error;
} agent_stream_status_t;

/** Optional setup-time preallocation: mask = (1u << AGENT_STREAM_VOICE) | ... .
 * Call after begin, before latency-sensitive capture. BLE retains selected rings
 * and worker stacks until end, so later open/write/close need no task creation.
 * May allocate, returns NOT_SUPPORTED for an unsupported kind/backend. */
esp_err_t agent_link_prepare_streams(uint32_t stream_mask);
/** Status by kind includes draining after the core's handle has been closed.
 * CUSTOM without the optional status operation returns NOT_SUPPORTED. */
esp_err_t agent_link_stream_get_status(agent_stream_t kind, agent_stream_status_t* out);

/**
 * @brief Open a stream.
 *
 * @param kind What the stream will carry.
 * @param opts Optional description (may be NULL for kinds that need none, such as VOICE).
 * @param out  Receives the handle. Required.
 * @return ESP_OK on success;
 *         ESP_ERR_INVALID_ARG if @p out is NULL or @p kind is out of range;
 *         ESP_ERR_INVALID_STATE if the link is not ready;
 *         ESP_ERR_NOT_SUPPORTED if the active transport cannot carry this kind.
 * @note Opening a kind that is already open succeeds and returns the existing handle, so a caller
 *       that cannot easily track state may simply open before every write.
 */
esp_err_t agent_link_stream_open(agent_stream_t kind, const agent_stream_opts_t* opts,
                                 agent_stream_handle_t* out);

/**
 * @brief Append bytes to an open stream.
 *
 * @return ESP_OK on success;
 *         ESP_ERR_INVALID_STATE if the stream is not open;
 *         ESP_ERR_NO_MEM if the transport's queue is full.
 * @note ESP_ERR_NO_MEM / TIMEOUT accept none of this chunk: retain and retry on a
 *       later loop iteration. For a per-write capacity limit, INVALID_SIZE means
 *       split into smaller writes; for a total-length overrun, fix the length or
 *       offset. Built-in BLE VOICE rejects odd byte lengths with INVALID_ARG.
 *       Built-in BLE queues reserve bounded byte rings on first open; no per-write
 *       allocation. Prefer 512-byte chunks to bound copy time and improve fairness.
 */
esp_err_t agent_link_stream_write(agent_stream_handle_t handle, const void* data, size_t bytes);

/**
 * @brief Close a stream.
 *
 * @param complete true if everything intended was written; false if the stream was cut short.
 *                 AUDIO/IMAGE signal the result to the App. Legacy BLE VOICE has
 *                 no terminal marker: false records local failure and drains
 *                 accepted bytes; it cannot tell the peer that capture truncated.
 * @note Closing an already-closed stream is a no-op returning ESP_OK.
 *       Success accepts END locally; use stream_get_status to observe draining.
 */
esp_err_t agent_link_stream_close(agent_stream_handle_t handle, bool complete);

/**
 * @brief Open, write and close in one call — for a payload already complete in memory.
 *
 * @note Returns once the payload is queued, not once it has reached the App; the transport
 *       drains it in the background. An already-open kind returns INVALID_STATE.
 *       If the payload is accepted but END is congested, poll retries END internally;
 *       ESP_OK means the caller must not resend the accepted payload.
 */
esp_err_t agent_link_stream_send(agent_stream_t kind, const agent_stream_opts_t* opts,
                                 const void* data, size_t bytes);

/** @brief Whether a stream of this kind is currently open. */
bool agent_link_stream_is_open(agent_stream_t kind);

#ifdef __cplusplus
}
#endif
