#pragma once
#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include "agent_link_stream.h"

namespace agentlink { namespace blepolicy {
// Connection interval units are 1.25ms; supervision timeout units are 10ms.
// The central may reject this preference. A slow link remains usable for
// control traffic, and negotiated parameters are reported by GAP events.
constexpr uint16_t kVoiceIntervalMin = 12;  // 15ms
constexpr uint16_t kVoiceIntervalMax = 24;  // 30ms
constexpr uint16_t kVoiceLatency = 0;
constexpr uint16_t kVoiceSupervisionTimeout = 400;  // 4s

constexpr size_t kCocControlReserve = 6;
constexpr unsigned kCocBurstSdus = 2;
// Account for every K-frame before handing an SDU to NimBLE: a send can
// consume its SDU after emitting only part of it if a later allocation fails.
// The first frame also carries the two-byte SDU length. Each frame needs a
// packet-header mbuf; only continuation mbufs have the full data capacity.
inline size_t CocTxBlocks(size_t bytes, size_t mps, size_t first_capacity,
                          size_t continuation_capacity) {
    if (!mps || !first_capacity || !continuation_capacity || bytes > SIZE_MAX-2)
        return SIZE_MAX;
    size_t remaining = bytes+2, blocks = 0;
    while (remaining) {
        const size_t frame = remaining < mps ? remaining : mps;
        const size_t extra = frame > first_capacity ? frame-first_capacity : 0;
        const size_t needed = 1+extra/continuation_capacity+(extra%continuation_capacity != 0);
        if (blocks > SIZE_MAX-needed) return SIZE_MAX;
        blocks += needed;
        remaining -= frame;
    }
    return blocks;
}
inline bool CocPoolReady(size_t free_blocks, size_t needed) {
    return free_blocks >= kCocControlReserve && needed <= free_blocks-kCocControlReserve;
}
inline size_t CocSduLimit(size_t requested, size_t mps, size_t first_capacity,
                          size_t continuation_capacity, size_t total_blocks) {
    if (CocPoolReady(total_blocks, CocTxBlocks(requested,mps,first_capacity,continuation_capacity)))
        return requested;
    // A valid small MPS can make a 512-byte SDU's whole-send allocation budget
    // larger than the entire pool. One first-mbuf K-frame avoids that permanent
    // wait. Preserve even PCM slices while permitting a final odd image tail.
    if (total_blocks <= kCocControlReserve || mps <= 2 || first_capacity <= 2) return 0;
    size_t limit = mps-2 < first_capacity-2 ? mps-2 : first_capacity-2;
    if (limit > requested) limit = requested;
    return limit & ~size_t(1);
}

// Count accepted FIFO packets until their notification has been submitted or
// cancelled. Popping a packet must not release this priority: its worker may
// still be waiting for the same mbuf that a continuous VOICE producer wants.
class ControlPriority {
public:
    void Enqueued() { pending_.fetch_add(1, std::memory_order_release); }
    void Completed(size_t count = 1) { pending_.fetch_sub(count, std::memory_order_release); }
    bool Pending() const { return pending_.load(std::memory_order_acquire) != 0; }
private:
    std::atomic<size_t> pending_{0};
};
// Reserve FIFO capacity, rather than reordering packets: START/END and ACK
// retain their wire order even when telemetry/manifest traffic fills the queue.
inline bool CriticalControl(const uint8_t* frame, size_t size) {
    if (!frame || size < 6) return false;
    const uint8_t type = frame[1] & 0x7f;
    if (type == 2) return true;
    return type == 3 && (frame[2] == 0x20 ||
                         (frame[2] >= 0x52 && frame[2] <= 0x55) ||
                         (frame[2] == 0x38 && size >= 17 && frame[6] >= 2));
}
inline size_t ControlQueueLimit(size_t slots, const uint8_t* frame, size_t size) {
    return CriticalControl(frame, size) || slots < 2 ? slots : slots-2;
}
// Return true only when the producer must wake the worker. An unchanged sample
// must preserve a pending subscription sync or failed-notification retry.
inline bool UpdateBatteryLevel(std::atomic<uint8_t>& level, std::atomic<bool>& pending,
                               uint8_t percent) {
    const uint8_t next = percent > 100 ? 100 : percent;
    if (level.exchange(next, std::memory_order_acq_rel) == next) return false;
    pending.store(true, std::memory_order_release);
    return true;
}
inline size_t VoicePayload(size_t mtu) {
    const size_t payload = mtu > 18 ? mtu - 18 : 0;
    return (payload < 205 ? payload : 205) & ~size_t(1);
}
inline size_t VoiceDrainBytes(size_t available, size_t payload, size_t capacity) {
    if (!payload || capacity < payload) return 0;
    // Size full notifications across caller write boundaries. This primitive
    // also sizes a short tail; VoiceCoalescing decides when that tail is ready.
    if (available < payload) return available & ~size_t(1);
    const size_t count = available < capacity ? available : capacity;
    return count - count % payload;
}
constexpr uint32_t kVoiceTailCoalesceMs = 20;
// One instance per VOICE session. Leave a short tail in the ring long enough
// for the next producer block to fill an MTU-sized notification. The original
// tail arrival sets a fixed deadline; worker wakeups never extend it.
class VoiceCoalescing {
public:
    size_t DrainBytes(size_t available, size_t payload, size_t capacity,
                      bool end, uint32_t now) {
        const size_t ready = VoiceDrainBytes(available, payload, capacity);
        if (!ready || available >= payload || end) {
            Reset();
            return ready;
        }
        if (!waiting_) { waiting_ = true; started_ = now; }
        if (uint32_t(now-started_) >= kVoiceTailCoalesceMs) {
            Reset();
            return ready;
        }
        return 0;
    }
    bool Waiting() const { return waiting_; }
    uint32_t WaitMs(uint32_t now) const {
        const uint32_t elapsed = uint32_t(now-started_);
        return waiting_ && elapsed < kVoiceTailCoalesceMs ? kVoiceTailCoalesceMs-elapsed : 0;
    }
    void Reset() { waiting_ = false; }
private:
    uint32_t started_ = 0;
    bool waiting_ = false;
};
template<class State>
void StreamStatus(const State& stream, size_t capacity, bool ready, uint32_t session,
                   agent_stream_status_t* out) {
    const bool active = stream.active.load(std::memory_order_acquire);
    const bool draining = stream.end_req.load(std::memory_order_acquire);
    *out = {};
    out->accepted_bytes = stream.accepted_bytes.load();
    out->sent_bytes = stream.sent_bytes.load();
    out->rejected_writes = stream.rejected_writes.load();
    out->last_error = stream.last_error.load();
    if (active) {
        out->state = draining ? AGENT_STREAM_DRAINING : AGENT_STREAM_OPEN;
        // Include the worker's in-flight chunk as well as bytes in the ring.
        out->queued_bytes = out->accepted_bytes >= out->sent_bytes
                              ? out->accepted_bytes-out->sent_bytes : 0;
        const size_t queued = stream.queued_bytes.load();
        if (!draining && queued <= capacity) out->writable_bytes = capacity-queued;
    } else if (session) {
        out->state = out->last_error == ESP_OK ? AGENT_STREAM_COMPLETED : AGENT_STREAM_FAILED;
    } else out->state = ready ? AGENT_STREAM_IDLE : AGENT_STREAM_UNAVAILABLE;
}
} }
