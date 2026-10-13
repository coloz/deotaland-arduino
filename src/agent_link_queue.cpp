#include "agent_link_queue.h"
#include <cstring>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif
namespace agentlink {
namespace {
static_assert(AGENT_LINK_RX_SLOTS >= 4, "Reserve space for connection transitions");
static_assert(AGENT_LINK_RX_CONTROL_RESERVE >= 0 && AGENT_LINK_RX_CONTROL_RESERVE < AGENT_LINK_RX_SLOTS-2,
              "Leave at least one media slot plus two connection slots");
RxEvent queue[AGENT_LINK_RX_SLOTS];
size_t head = 0, count = 0;
bool accepting = false;
uint32_t dropped = 0, high_water = 0;
uint32_t generation = 0;
uint32_t deferred_ticket = 0, deferred_generation = 0, deferred_done = 0;
agent_link_status_t status = {};
bool status_pending = false;
#if defined(ARDUINO_ARCH_ESP32)
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
struct Guard { Guard() { portENTER_CRITICAL(&mux); } ~Guard() { portEXIT_CRITICAL(&mux); } };
#else
struct Guard {};
#endif
}
void RxEnable(bool enabled) {
    Guard guard;
    (void)guard;
    accepting = enabled;
    ++generation;
    count = head = 0;
    deferred_ticket = deferred_generation = deferred_done = 0;
    if (enabled) dropped = high_water = 0;
}
esp_err_t RxPush(RxKind kind, agent_stream_t stream, const uint8_t* data, size_t n) {
    if ((n && !data) || n > AGENT_LINK_RX_BYTES) return ESP_ERR_INVALID_SIZE;
    Guard guard;
    (void)guard;
    if (!accepting) return ESP_ERR_INVALID_STATE;
    // Every connection announcement starts a new control session, including
    // loss/recovery of a GATT subscription on the same physical link. Collapse
    // obsolete work but retain a pending disconnect so the application sees it.
    if (kind == RxKind::Connection && n == 1) {
        const bool keep_disconnect = data[0] != 0 && count &&
            queue[head].kind == RxKind::Connection && queue[head].length == 1 &&
            queue[head].data[0] == 0;
        ++generation;
        deferred_ticket = deferred_generation = deferred_done = 0;
        if (keep_disconnect) { count = 1; queue[head].generation = generation; }
        else count = head = 0;
    }
    const bool state = kind == RxKind::Connection || kind == RxKind::Ready;
    const size_t reserve=state?0:2+(kind==RxKind::Stream?AGENT_LINK_RX_CONTROL_RESERVE:0);
    if (count >= AGENT_LINK_RX_SLOTS - reserve) {
        ++dropped;
        return ESP_ERR_NO_MEM;
    }
    RxEvent& e = queue[(head + count) % AGENT_LINK_RX_SLOTS];
    e.kind = kind; e.stream = stream; e.length = n;
    e.generation = generation;
    if (n) memcpy(e.data, data, n);
    if (++count > high_water) high_water = static_cast<uint32_t>(count);
    return ESP_OK;
}
bool RxPop(RxEvent& out) {
    Guard guard;
    (void)guard;
    if (!count) return false;
    const RxEvent& e = queue[head];
    out.kind = e.kind; out.stream = e.stream; out.length = e.length;
    out.generation = e.generation;
    if (e.length) memcpy(out.data, e.data, e.length);
    head = (head + 1) % AGENT_LINK_RX_SLOTS;
    --count;
    return true;
}
bool RxHasSpace() { Guard guard; (void)guard; return accepting && count < AGENT_LINK_RX_SLOTS - 2 - AGENT_LINK_RX_CONTROL_RESERVE; }
uint32_t RxGeneration() { Guard guard; (void)guard; return generation; }
esp_err_t RxPushDeferredStream(agent_stream_t stream, uint32_t ticket, uint32_t origin_generation) {
    if (!ticket) return ESP_ERR_INVALID_ARG;
    uint8_t marker[8];
    for (unsigned i=0;i<4;++i) {
        marker[i]=static_cast<uint8_t>(ticket>>(i*8));
        marker[i+4]=static_cast<uint8_t>(origin_generation>>(i*8));
    }
    // A retained SDU uses a control-reserved slot so its later END still fits.
    return RxPush(RxKind::DeferredStream,stream,marker,sizeof(marker));
}
bool RxDeferredBegin(uint32_t ticket, uint32_t expected_generation) {
    Guard guard; (void)guard;
    if (!accepting || !ticket || generation != expected_generation) return false;
    deferred_ticket=ticket; deferred_generation=generation; deferred_done=0;
    return true;
}
uint32_t RxDeferredTicket() {
    Guard guard; (void)guard;
    return accepting && deferred_generation==generation ? deferred_ticket : 0;
}
bool RxDeferredActive(uint32_t ticket) { return ticket && RxDeferredTicket()==ticket; }
bool RxDeferredHasSpace() {
    Guard guard; (void)guard;
    return accepting && count < AGENT_LINK_RX_SLOTS-2;
}
void RxDeferredComplete(uint32_t ticket) {
    Guard guard; (void)guard;
    if (accepting && ticket && deferred_ticket==ticket && deferred_generation==generation) deferred_done=ticket;
}
bool RxDeferredDone(uint32_t ticket) {
    Guard guard; (void)guard;
    return accepting && ticket && deferred_ticket==ticket && deferred_done==ticket && deferred_generation==generation;
}
void RxDeferredEnd(uint32_t ticket) {
    Guard guard; (void)guard;
    if (deferred_ticket==ticket) deferred_ticket=deferred_generation=deferred_done=0;
}
void RxStats(agent_link_stats_t* out) {
    if (!out) return;
    Guard guard; (void)guard;
    out->rx_dropped = dropped; out->rx_high_water = high_water;
}
void StatusReset(agent_transport_kind_t transport) {
    Guard guard; (void)guard;
    status = {}; status.transport = transport; status_pending = false;
}
void StatusPush(const agent_link_status_t* value) {
    if (!value) return;
    Guard guard; (void)guard;
    if (!accepting) return;
    status = *value;
    status.title[sizeof(status.title)-1] = 0;
    status.hint[sizeof(status.hint)-1] = 0;
    status_pending = true;
}
void StatusGet(agent_link_status_t* out) {
    if (!out) return;
    Guard guard; (void)guard; *out = status;
}
bool StatusTake(agent_link_status_t* out) {
    Guard guard; (void)guard;
    if (!accepting || !status_pending || !out) return false;
    *out = status; status_pending = false; return true;
}
}
