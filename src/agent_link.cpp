// Portable Agent Link protocol and lifecycle. Backends only enqueue receive data;
// poll() dispatches commands, hardware callbacks, and bounded service progress.
// Hardware drivers belong to the application. Bulk payloads use stream sessions.
#include "agent_link.h"
#include "agent_link_transport.h"
#include "protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "agent_link_buffer.h"
#include "agent_link_task_storage.h"
#include "agent_link_log.h"
#include "agent_link_queue.h"
#include "audio_downlink.h"
#include "ota_service.h"
#include "agent_link_identity.h"
#include "device_identity.h"
#include <algorithm>
#include <array>
#include <cmath>

struct agent_stream_session { agent_stream_t kind; bool open; agent_stream_handle_t handle; };


namespace {
constexpr const char* TAG = "agent_link";

agent_link_config_t s_cfg   = {};
agent_output_cb_t   s_out   = {};
agent_playback_config_t s_playback = {};
bool                s_have_out = false;
agent_state_t       s_state = AGENT_STATE_DISCONNECTED;
agent_transport_t*  s_tx    = nullptr;
bool s_running = false;
bool s_initialized = false;
bool s_polling = false;
unsigned s_service_cursor = 0;
unsigned s_end_cursor = 0;
unsigned s_reading_cursor = 0;
bool s_audio_end_pending = false;
bool s_audio_finished = false;
bool s_direct_bulk = false; // poll-only while dispatching a retained FIFO stream marker
uint32_t s_direct_bulk_generation = 0;
// A retry of the most recent successful VoiceReply must ACK without replaying
// Arm/Finish. Those side effects clear staged PCM or call the board's END twice.
// Only exact consecutive requests are deduplicated: another command invalidates
// this slot, so sequence wrap and later session-ID reuse remain valid.
bool s_voice_reply_seen = false;
uint8_t s_voice_reply_sequence = 0;
uint8_t s_voice_reply_payload[6] = {};
size_t s_voice_reply_length = 0;
uint32_t s_generation = 0;
agent_link_stats_t s_stats = {};
agent_stream_session s_streams[AGENT_STREAM_KIND_COUNT] = {
    {AGENT_STREAM_VOICE, false, nullptr}, {AGENT_STREAM_AUDIO, false, nullptr},
    {AGENT_STREAM_IMAGE, false, nullptr}, {AGENT_STREAM_VIDEO, false, nullptr}, {AGENT_STREAM_FILE, false, nullptr}
};
uintptr_t s_handle_serial = 0; // Never reset: old tokens must not alias after end/begin.
bool s_pending_stream_end[AGENT_STREAM_KIND_COUNT] = {};
bool s_pending_stream_complete[AGENT_STREAM_KIND_COUNT] = {};
uint8_t s_response[10 + AGENT_LINK_CMD_RESP_MAX];
size_t s_response_size = 0;
agentlink::RxEvent s_rx_event;
size_t s_rx_offset = 0;
bool s_rx_pending = false;
agentlink::TaskStorage<agentlink::FixedText<AGENT_LINK_MANIFEST_BYTES>> s_manifest_storage;
agentlink::FixedText<AGENT_LINK_MANIFEST_BYTES>& Manifest() { return s_manifest_storage.get(); }
int s_manifest_build_index = -1;
bool s_manifest_building = false;
size_t s_manifest_offset = 0, s_manifest_budget = 0;
uint16_t s_manifest_index = 0;
std::string s_name, s_manufacturer, s_model, s_firmware;
uint8_t s_device_id[16] = {};
char s_identity_text[37] = {};

size_t BoundedLength(const char* text, size_t max) {
    if (!text) return 0;
    size_t n=0; while(n<=max && text[n]) ++n;
    return n;
}

size_t BuildDeviceInfo(uint8_t* out, size_t capacity) {
    if (!dev_identity_uuid(s_device_id)) return 0;
    const size_t vn = s_firmware.size(), mn = s_model.size();
    if (vn > 255 || mn > 255 || 29 + vn + mn > capacity) return 0;
    size_t p = 0;
    memcpy(out, s_device_id, 16); p += 16;
    uint8_t mac[6] = {};
    if (s_cfg.transport == AGENT_TRANSPORT_BLE) agent_transport_ble_get_mac(mac);
    memcpy(out + p, mac, 6); p += 6;
    // Battery is populated by the caller below; other reserved power fields stay zero.
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 0;
    out[p++] = static_cast<uint8_t>(vn);
    memcpy(out+p,s_firmware.data(),vn); p += vn;
    out[p++] = 0; // profile length
    out[p++] = static_cast<uint8_t>(mn);
    memcpy(out+p,s_model.data(),mn); p += mn;
    return p;
}

bool Ready() { return s_running && s_generation == agentlink::RxGeneration() && s_tx && s_tx->is_ready && s_tx->is_ready(s_tx->impl) && s_state == AGENT_STATE_READY; }
bool ControlReady() {
    return Ready();
}
size_t ControlMtu() {
    size_t n = 20;
    if (s_tx && s_tx->ctrl_mtu) n = s_tx->ctrl_mtu(s_tx->impl);
    else if (s_cfg.transport == AGENT_TRANSPORT_BLE) {
        const size_t att = agent_transport_ble_att_mtu();
        n = att >= 23 ? att - 3 : 20;
    }
    return std::min(n, size_t(1030));
}
size_t PayloadBudget() {
    const size_t n=ControlMtu();
    const size_t payload=n>6?n-6:0;
    return s_cfg.transport==AGENT_TRANSPORT_BLE ? std::min(payload,size_t(480)) : payload;
}
esp_err_t SendControl(const uint8_t* p, size_t n) {
    if (!ControlReady() || !s_tx || !s_tx->send_ctrl) return ESP_ERR_INVALID_STATE;
    if (!p || n < 6 || n > ControlMtu()) return ESP_ERR_INVALID_SIZE;
    const esp_err_t e = s_tx->send_ctrl(s_tx->impl, p, n);
    if (e != ESP_OK) s_stats.last_error = e;
    return e;
}
void SendServiceEvent(uint8_t id, const uint8_t* p, size_t n) {
    uint8_t frame[1030];
    (void)SendControl(frame, agentlink::EncodeEvent(frame,sizeof(frame),id,p,n));
}
esp_err_t SendReliableServiceEvent(uint8_t id, const uint8_t* p, size_t n) {
    uint8_t frame[1030];
    return SendControl(frame,agentlink::EncodeEvent(frame,sizeof(frame),id,p,n));
}
void OnDecodedPcm(const uint8_t* p, size_t n) {
    if (Ready() && s_have_out && s_out.on_audio_out) s_out.on_audio_out(p, n, s_out.ctx);
}
size_t AcceptPcm(const uint8_t* p,size_t n,void*) {
    return Ready() && s_playback.write ? s_playback.write(p,n,s_playback.ctx) : 0;
}
size_t PlaybackQueued(void*) { return Ready() && s_playback.queued ? s_playback.queued(s_playback.ctx) : 0; }
esp_err_t QueueCtrl(const uint8_t* p, size_t n) {
    return agentlink::RxPush(agentlink::RxKind::Control, AGENT_STREAM_VOICE, p, n);
}
esp_err_t QueueStream(agent_stream_t k, const uint8_t* p, size_t n) {
    return agentlink::RxPush(agentlink::RxKind::Stream, k, p, n);
}
void QueueCtrlLegacy(const uint8_t* p, size_t n) { (void)QueueCtrl(p, n); }
void QueueStreamLegacy(agent_stream_t k, const uint8_t* p, size_t n) { (void)QueueStream(k,p,n); }
void QueueConnection(bool connected) {
    // OTA ownership must end immediately, before a fast reconnect can deliver data.
    if (!connected) agentlink::ota::OnLinkState(false);
    const uint8_t b = connected ? 1 : 0;
    (void)agentlink::RxPush(agentlink::RxKind::Connection, AGENT_STREAM_VOICE, &b, 1);
}
void QueueReady() { (void)agentlink::RxPush(agentlink::RxKind::Ready, AGENT_STREAM_VOICE, nullptr, 0); }
size_t OnStreamData(agent_stream_t type, const uint8_t* data, size_t len);
size_t ReceiveBulk(const uint8_t* p, size_t n) {
    if (agentlink::ota::OwnsDataChannel()) return agentlink::ota::FeedData(p,n);
    n = std::min(n, size_t(AGENT_LINK_RX_BYTES));
    if (s_direct_bulk) {
        // Subscription changes can retire control without retiring the physical
        // CoC channel. Discard that old TTS, while OTA above keeps its ownership.
        if (s_direct_bulk_generation != agentlink::RxGeneration() || !Ready()) return n;
        return OnStreamData(AGENT_STREAM_VOICE,p,n);
    }
    return QueueStream(AGENT_STREAM_VOICE,p,n) == ESP_OK ? n : 0;
}
bool ReceiveSpace() {
    if (agentlink::ota::OwnsDataChannel()) return agentlink::ota::WantsMore();
    return s_direct_bulk || agentlink::RxHasSpace();
}
const agent_transport_callbacks_t kCallbacks = {QueueCtrl, QueueStream, QueueConnection, QueueReady, agentlink::StatusPush};

void LinkStatus(agent_link_phase_t phase, const char* title, const char* hint) {
    agent_link_status_t status = {};
    status.transport = s_cfg.transport; status.phase = phase;
    snprintf(status.title, sizeof(status.title), "%s", title);
    snprintf(status.hint, sizeof(status.hint), "%s", hint);
    agentlink::StatusPush(&status);
}

// The SDK only notifies when the battery level changes / charging state changes / a
// low-battery edge occurs. The cache is updated only on a successful send; a failed
// send is retried on the next call.
int  s_bat_pct = -1;          // last successfully pushed battery level (-1 = unknown: force a resend after connect for initial sync)
int  s_bat_chg = -1;          // last successfully pushed charging state (-1 = unknown; otherwise 0/1)
int s_bat_current_pct = -1, s_bat_current_chg = -1;
bool s_bat_low_armed = true;  // low-battery edge armed (true = not yet triggered)
constexpr uint8_t kBatLowThreshold = 5;  // low-battery threshold (%)

// Generic I/O endpoint registry (manifest + reading reports + downlink actuators).
// Downlink actuator commands are routed by id to the matching cb.
constexpr int kMaxIo = 32;
static_assert(AGENT_LINK_READING_CACHE_BYTES >= 16 && AGENT_LINK_READING_CACHE_BYTES <= AGENT_LINK_CMD_RESP_MAX-3,
              "Reading cache must fit a GetReading response (16..125 bytes)");
struct IoEntry {
    const agent_link_io_desc_t* desc;
    agent_io_actuate_cb_t       cb;
    void*                       ctx;
    uint8_t  policy_mode;      // 0=passthrough, 1=off, 2=periodic, 3=on-change
    uint16_t policy_rate_hz;
    uint32_t last_sent_ms;
    uint32_t last_ts_ms;
    uint8_t sent_val[AGENT_LINK_READING_CACHE_BYTES];
    uint8_t sent_len;
    bool has_sent;
    uint8_t  last_val[AGENT_LINK_READING_CACHE_BYTES];
    uint8_t  last_len;
    bool     has_last;
    bool pending;
};

agentlink::TaskStorage<std::array<IoEntry,kMaxIo>> s_io_storage;
std::array<IoEntry,kMaxIo>& IoEntries() { return s_io_storage.get(); }
int     s_io_count = 0;
bool    s_manifest_sent = false;  // whether the manifest has been sent in this connection
uint32_t s_manifest_rev = 1;
uint32_t s_manifest_build_revision = 0;
void PumpReading();

enum {
    IO_POLICY_PASSTHROUGH = 0,
    IO_POLICY_OFF = 1,
    IO_POLICY_PERIODIC = 2,
    IO_POLICY_ONCHANGE = 3,
};

// Single exit point for APIs that the selected transport/upstream protocol does not support.
esp_err_t not_ready(const char* what) {
    (void)what;
    if (Ready()) {
        ESP_LOGD(TAG, "%s: not supported by the selected transport", what);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGD(TAG, "%s: link not ready — ignored", what);
    return ESP_ERR_INVALID_STATE;
}

// ── Generic I/O: manifest serialization + reading/actuator routing ──────────────
// Look up an endpoint by id.
const IoEntry* FindIo(const char* id) {
    if (!id) return nullptr;
    for (int i = 0; i < s_io_count; ++i)
        if (IoEntries()[i].desc && IoEntries()[i].desc->id && strcmp(IoEntries()[i].desc->id, id) == 0)
            return &IoEntries()[i];
    return nullptr;
}

int FindIoIndex(const char* id) {
    if (!id) return -1;
    for (int i = 0; i < s_io_count; ++i) {
        if (IoEntries()[i].desc && IoEntries()[i].desc->id && strcmp(IoEntries()[i].desc->id, id) == 0) return i;
    }
    return -1;
}

// Fixed byte size of a value type; BLOB is variable-length and returns -1. Used for push_reading length checks.
int IoValueSize(agent_val_t t) {
    switch (t) {
    case AGENT_VAL_BOOL: return 1;
    case AGENT_VAL_U16:  return 2;
    case AGENT_VAL_I32:  return 4;
    case AGENT_VAL_F32:  return 4;
    case AGENT_VAL_RGB:  return 4;
    case AGENT_VAL_VEC2: return 8;
    case AGENT_VAL_VEC3: return 12;
    case AGENT_VAL_BLOB: return -1;
    case AGENT_VAL_STR:  return -1;
    }
    return -1;
}

// Use the same wire-value validation for incoming actuators and cached readings.
// memcpy avoids alignment/aliasing traps; floats on the wire are little-endian.
esp_err_t ValidateIoValue(agent_val_t type, const void* value, size_t len) {
    const int width = IoValueSize(type);
    if (width >= 0 && len != static_cast<size_t>(width)) return ESP_ERR_INVALID_SIZE;
    const auto* bytes = static_cast<const uint8_t*>(value);
    // STR travels as UTF-8 and is exposed to C-string display callbacks. Reject
    // embedded NUL and malformed scalar encodings instead of silently truncating
    // the actuator or forwarding unreadable sensor text. BLOB remains binary.
    if (type == AGENT_VAL_STR && !agentlink::IsUtf8Text(bytes,len)) return ESP_ERR_INVALID_ARG;
    if (type == AGENT_VAL_BOOL && bytes[0] > 1) return ESP_ERR_INVALID_ARG;
    if (type == AGENT_VAL_F32 || type == AGENT_VAL_VEC2 || type == AGENT_VAL_VEC3) {
        for (size_t offset = 0; offset < len; offset += 4) {
            const uint32_t bits = uint32_t(bytes[offset]) | (uint32_t(bytes[offset+1]) << 8) |
                (uint32_t(bytes[offset+2]) << 16) | (uint32_t(bytes[offset+3]) << 24);
            float number;
            static_assert(sizeof(number) == sizeof(bits), "Agent Link requires 32-bit float");
            memcpy(&number, &bits, sizeof(number));
            if (!std::isfinite(number)) return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

// Value type -> the type string used in the manifest.
const char* ValTypeName(agent_val_t t) {
    switch (t) {
    case AGENT_VAL_BOOL: return "bool";
    case AGENT_VAL_U16:  return "u16";
    case AGENT_VAL_I32:  return "i32";
    case AGENT_VAL_F32:  return "f32";
    case AGENT_VAL_RGB:  return "rgb";
    case AGENT_VAL_VEC2: return "vec2";
    case AGENT_VAL_VEC3: return "vec3";
    case AGENT_VAL_BLOB: return "blob";
    case AGENT_VAL_STR:  return "str";
    }
    return "blob";
}

// Borrowed token cursors let serialization yield in the middle of a long field,
// including the middle of a JSON escape. Numeric tokens live until consumed.
struct ManifestToken { const char* text; bool escape; };
ManifestToken s_manifest_tokens[40];
size_t s_manifest_token_count=0, s_manifest_token_index=0;
char s_manifest_number[96], s_manifest_rate[32], s_manifest_escape[6];
size_t s_manifest_escape_size=0, s_manifest_escape_offset=0;
bool s_manifest_final_tokens=false;
#ifndef AGENT_LINK_MANIFEST_STEP_BYTES
#define AGENT_LINK_MANIFEST_STEP_BYTES 256u
#endif
static_assert(AGENT_LINK_MANIFEST_STEP_BYTES > 0, "Manifest work budget cannot be zero");
void ManifestTokenAdd(const char* text, bool escape=false) {
    if (text && s_manifest_token_count < sizeof(s_manifest_tokens)/sizeof(s_manifest_tokens[0]))
        s_manifest_tokens[s_manifest_token_count++]={text,escape};
}
void ManifestString(const char* prefix, const char* value) {
    if (!value || !*value) return;
    ManifestTokenAdd(prefix); ManifestTokenAdd(value,true); ManifestTokenAdd("\"");
}

void SynthLedCb(const char*, const uint8_t* args, size_t len, void*) {
    if (len < 4 || !(s_have_out && s_out.on_led)) return;
    const uint32_t rgb = static_cast<uint32_t>(args[0]) |
                         (static_cast<uint32_t>(args[1]) << 8) |
                         (static_cast<uint32_t>(args[2]) << 16) |
                         (static_cast<uint32_t>(args[3]) << 24);
    s_out.on_led(rgb, s_out.ctx);
}

void SynthHapticCb(const char*, const uint8_t* args, size_t len, void*) {
    if (len < 2 || !(s_have_out && s_out.on_haptic)) return;
    const uint32_t ms = static_cast<uint32_t>(args[0]) |
                        (static_cast<uint32_t>(args[1]) << 8);
    s_out.on_haptic(ms, s_out.ctx);
}

void SynthScreenCb(const char*, const uint8_t* args, size_t len, void*) {
    if (!(s_have_out && s_out.on_show_text)) return;
    char buf[256];
    const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    if (n && args) memcpy(buf, args, n);
    buf[n] = '\0';
    s_out.on_show_text(buf, s_out.ctx);
}

const agent_link_io_desc_t kSynthLed = {
    "led0", AGENT_IO_OUT, "led", AGENT_VAL_RGB, nullptr,
    "status LED color, 0x00RRGGBB", 0, 0, 0, nullptr,
    "Status LED", AGENT_AUD_AI, nullptr, nullptr, AGENT_EVT_PERIODIC,
};
const agent_link_io_desc_t kSynthHaptic = {
    "motor0", AGENT_IO_OUT, "haptic", AGENT_VAL_U16, "ms",
    "vibration motor duration", 0, 65535, 0, nullptr,
    "Vibration", AGENT_AUD_AI, nullptr, nullptr, AGENT_EVT_PERIODIC,
};
const agent_link_io_desc_t kSynthScreen = {
    "screen0", AGENT_IO_OUT, "screen.text", AGENT_VAL_STR, nullptr,
    "text display", 0, 0, 0, nullptr,
    "Screen text", AGENT_AUD_AI, nullptr, nullptr, AGENT_EVT_PERIODIC,
};

void RegisterSyntheticEndpoints() {
    if (!s_have_out) return;
    if ((s_cfg.caps & AGENT_CAP_LED) && s_out.on_led && !FindIo("led0"))
        (void)agent_link_register_io(&kSynthLed, SynthLedCb, nullptr);
    if ((s_cfg.caps & AGENT_CAP_HAPTIC) && s_out.on_haptic && !FindIo("motor0"))
        (void)agent_link_register_io(&kSynthHaptic, SynthHapticCb, nullptr);
    if ((s_cfg.caps & AGENT_CAP_SCREEN) && s_out.on_show_text && !FindIo("screen0"))
        (void)agent_link_register_io(&kSynthScreen, SynthScreenCb, nullptr);
}

// Check before replacing the previous configuration, including its synthetic I/O.
bool CanRegisterSyntheticEndpoints(const agent_link_config_t& cfg) {
    int occupied = 0;
    bool led = false, haptic = false, screen = false;
    for (int i=0; i<s_io_count; ++i) {
        const auto* d=IoEntries()[i].desc;
        if (d==&kSynthLed || d==&kSynthHaptic || d==&kSynthScreen) continue;
        ++occupied;
        led |= strcmp(d->id,"led0")==0;
        haptic |= strcmp(d->id,"motor0")==0;
        screen |= strcmp(d->id,"screen0")==0;
    }
    if (cfg.output) {
        occupied += !led && (cfg.caps & AGENT_CAP_LED) && cfg.output->on_led;
        occupied += !haptic && (cfg.caps & AGENT_CAP_HAPTIC) && cfg.output->on_haptic;
        occupied += !screen && (cfg.caps & AGENT_CAP_SCREEN) && cfg.output->on_show_text;
    }
    return occupied <= kMaxIo;
}

void NextManifestTokens() {
    s_manifest_token_count=s_manifest_token_index=0;
    if (s_manifest_build_index < 0) {
        snprintf(s_manifest_number,sizeof(s_manifest_number),"{\"proto\":%u,\"rev\":%u,\"caps\":%u,\"io\":[",
            unsigned(AGENT_LINK_PROTO_VERSION),unsigned(s_manifest_rev),unsigned(s_cfg.caps));
        ManifestTokenAdd(s_manifest_number); s_manifest_build_index=0;
    } else if (s_manifest_build_index < s_io_count) {
        const auto* d=IoEntries()[s_manifest_build_index].desc;
        if (s_manifest_build_index++) ManifestTokenAdd(",");
        ManifestTokenAdd("{\"id\":\""); ManifestTokenAdd(d->id,true);
        ManifestTokenAdd("\",\"dir\":\""); ManifestTokenAdd(d->dir==AGENT_IO_OUT?"out":"in");
        ManifestTokenAdd("\",\"kind\":\""); ManifestTokenAdd(d->kind,true);
        ManifestTokenAdd("\",\"value\":\""); ManifestTokenAdd(ValTypeName(d->value)); ManifestTokenAdd("\"");
        ManifestString(",\"unit\":\"",d->unit);
        ManifestString(",\"desc\":\"",d->desc);
        ManifestString(",\"name\":\"",d->display_name);
        if (d->range_min != d->range_max) {
            snprintf(s_manifest_number,sizeof(s_manifest_number),",\"range\":[%g,%g]",
                     static_cast<double>(d->range_min), static_cast<double>(d->range_max));
            ManifestTokenAdd(s_manifest_number);
        }
        if (d->rate_hz) {
            snprintf(s_manifest_rate,sizeof(s_manifest_rate),",\"rate_hz\":%u",static_cast<unsigned>(d->rate_hz));
            ManifestTokenAdd(s_manifest_rate);
        }
        if (d->event==AGENT_EVT_ON_CHANGE) ManifestTokenAdd(",\"event\":\"change\"");
        else if(d->event==AGENT_EVT_THRESHOLD) ManifestTokenAdd(",\"event\":\"threshold\"");
        if (d->audience==AGENT_AUD_USER) ManifestTokenAdd(",\"audience\":\"user\"");
        if (d->enum_json && d->enum_json[0]) { ManifestTokenAdd(",\"enum\":"); ManifestTokenAdd(d->enum_json); }
        if (d->default_json && d->default_json[0]) { ManifestTokenAdd(",\"default\":"); ManifestTokenAdd(d->default_json); }
        if (d->dir == AGENT_IO_OUT && d->args_schema && d->args_schema[0]) {
            ManifestTokenAdd(",\"args\":"); ManifestTokenAdd(d->args_schema);
        }
        ManifestTokenAdd("}");
    } else {
        ManifestTokenAdd("]}"); s_manifest_final_tokens=true;
    }
}
// At most STEP_BYTES output bytes; descriptor length cannot monopolize loop().
void BuildManifestStep() {
    for(size_t work=0;work<AGENT_LINK_MANIFEST_STEP_BYTES && Manifest().good();) {
        if(s_manifest_escape_offset<s_manifest_escape_size) {
            Manifest()+=s_manifest_escape[s_manifest_escape_offset++]; ++work; continue;
        }
        if(s_manifest_token_index==s_manifest_token_count) {
            if(s_manifest_final_tokens) { s_manifest_building=false; break; }
            NextManifestTokens();
        }
        auto& token=s_manifest_tokens[s_manifest_token_index];
        if(!*token.text) { ++s_manifest_token_index; continue; }
        const unsigned char c=static_cast<unsigned char>(*token.text++);
        s_manifest_escape_offset=s_manifest_escape_size=0;
        if(token.escape && (c=='"' || c=='\\')) {
            s_manifest_escape[0]='\\'; s_manifest_escape[1]=static_cast<char>(c); s_manifest_escape_size=2;
        } else if(token.escape && c<0x20) {
            const char hex[]="0123456789abcdef";
            memcpy(s_manifest_escape,"\\u00",4);
            s_manifest_escape[4]=hex[c>>4]; s_manifest_escape[5]=hex[c&15]; s_manifest_escape_size=6;
        } else { Manifest()+=static_cast<char>(c); ++work; }
    }
    if (!Manifest().good()) {
        s_stats.last_error=ESP_ERR_INVALID_SIZE;
        Manifest().clear(); s_manifest_building=false;
    }
}

// Send the manifest, fragmented into 0x18 IoManifest events over the control plane (send_ctrl).
//   Each chunk payload = [chunk_idx(1)] [last(1: 0/1)] [json fragment...]; the App concatenates
//   chunks in order until last=1, then parses the whole JSON.
// Triggered: after the BLE peer subscribes to 0xFFC4 (OnLinkReady) /
//   on the App's 0x34 fetch command.
void SendManifest(bool force) {
    if (!s_tx || (!force && s_manifest_sent)) return;
    // Repeated fetches of the same revision must not restart an in-flight build.
    if ((s_manifest_building || !Manifest().empty()) && s_manifest_build_revision==s_manifest_rev) return;
    s_manifest_build_revision=s_manifest_rev;
    Manifest().clear(); s_manifest_build_index=-1; s_manifest_building=true;
    s_manifest_token_count=s_manifest_token_index=0;
    s_manifest_escape_size=s_manifest_escape_offset=0; s_manifest_final_tokens=false;
    s_manifest_sent = false;
    s_manifest_offset = s_manifest_index = 0;
    s_manifest_budget = 0;
}

// Exactly one fragment attempt per poll; preserve offset on congestion.
void PumpManifest() {
    if (!ControlReady()) return;
    if (s_manifest_building) { ++s_stats.manifest_build_steps; BuildManifestStep(); return; }
    if (Manifest().empty()) return;
    const size_t payload = PayloadBudget();
    if (payload <= 2) { s_stats.last_error = ESP_ERR_INVALID_SIZE; return; }
    const size_t budget = std::min(payload - 2, size_t(480));
    if (budget != s_manifest_budget) {
        s_manifest_budget = budget;
        s_manifest_offset = s_manifest_index = 0;
    }
    if (Manifest().size() > budget * 256) {
        s_stats.last_error = ESP_ERR_INVALID_SIZE;
        return; // A later MTU exchange can make the manifest fit.
    }
    const size_t n = std::min(budget, Manifest().size() - s_manifest_offset);
    uint8_t chunk[482];
    chunk[0] = static_cast<uint8_t>(s_manifest_index);
    chunk[1] = s_manifest_offset + n == Manifest().size();
    memcpy(chunk + 2, Manifest().data() + s_manifest_offset, n);
    const esp_err_t result=SendReliableServiceEvent(0x18,chunk,n+2);
    if (result != ESP_OK) {
        if (result==ESP_ERR_NO_MEM || result==ESP_ERR_TIMEOUT) ++s_stats.tx_retries;
        else Manifest().clear(); // A permanent backend rejection must not monopolize future polls.
        return;
    }
    s_manifest_offset += n;
    ++s_manifest_index;
    ++s_stats.manifest_fragments;
    if (s_manifest_offset == Manifest().size()) { s_manifest_sent = true; Manifest().clear(); }
}

// BLE: fired after the peer subscribes to the event channel 0xFFC4 (notifications are only
// delivered from that point on) -> send the manifest.
void OnLinkReady() {
    // A BLE ACL connection alone cannot carry device events: READY begins when
    // the peer enables the 0xFFC4 notification channel.
    if (s_state != AGENT_STATE_READY) {
        s_state = AGENT_STATE_READY;
        LinkStatus(AGENT_LINK_PHASE_READY, "Ready", "Connected to the agent");
        if (s_cfg.on_state) s_cfg.on_state(s_state, s_cfg.state_ctx);
    }
    SendManifest(/*force=*/false);
}

// Transport -> core: connection state change.
void OnConn(bool connected) {
    s_state = connected ? AGENT_STATE_CONNECTED : AGENT_STATE_DISCONNECTED;
    LinkStatus(connected ? AGENT_LINK_PHASE_CONNECTED : AGENT_LINK_PHASE_SETUP,
               connected ? "Connected" : "Set up link",
               connected ? "Waiting for the event channel" : "Open the App to connect");
    Manifest().clear(); s_manifest_building=false;
    s_response_size = 0;
    for (auto& stream : s_streams) stream.open = false;
    for (auto& pending : s_pending_stream_end) pending=false;
    for (auto& io : IoEntries()) {
        io.has_sent = false; io.last_sent_ms = 0;
        io.policy_mode=IO_POLICY_PASSTHROUGH; io.policy_rate_hz=0;
        io.pending=io.has_last;
    }
    agentlink::ota::OnLinkState(connected);
    // A fresh control session must not inherit a partial PCM/ADPCM block.
    agentlink::audio::OnLinkState(false);
    s_audio_end_pending=s_audio_finished=false;
    s_voice_reply_seen=false;
    s_manifest_sent = false;      // Re-send after the event channel becomes ready.
    if (!connected) {

    } else {
        // Clear the battery cache on connect so the next report_battery force-resends the current value (initial sync for the App).
        s_bat_pct = -1; s_bat_chg = -1; s_bat_low_armed = true;
        // Manifest waits for a real data-plane readiness notification.
    }
    if (s_cfg.on_state) s_cfg.on_state(s_state, s_cfg.state_ctx);
}

// Transport -> core: a control frame arrived (peer wrote the command channel 0xFFC1).
void OnCtrlFrame(const uint8_t* data, size_t len) {
    agentlink::FrameView f;
    if (!agentlink::ParseFrameView(data, len, f)) {
        ESP_LOGW(TAG, "bad ctrl frame (%u bytes)", static_cast<unsigned>(len));
        return;
    }
    if (f.encrypted || f.msg_type != agentlink::kMsgCommand) return;  // the device side only handles "commands"

    const bool voice_replay = f.command_id==0x05 && s_voice_reply_seen &&
        f.sequence==s_voice_reply_sequence && f.payload.size()==s_voice_reply_length &&
        memcmp(f.payload.data(),s_voice_reply_payload,s_voice_reply_length)==0;
    s_voice_reply_seen=false;
    if (voice_replay) {
        s_voice_reply_seen=true;
        s_response_size=agentlink::EncodeResponse(s_response,sizeof(s_response),0x05,
            f.sequence,0,0,nullptr,0);
        return;
    }

    ESP_LOGD(TAG, "cmd 0x%02X seq=%u payload=%uB",
             f.command_id, f.sequence, static_cast<unsigned>(f.payload.size()));

    // ── Command dispatch + response (may carry data) ────────────────────────────
    // Priority: built-in protocol commands, then on_command, then UnknownCommand.
    uint8_t  extra[AGENT_LINK_CMD_RESP_MAX] = {};
    size_t   extra_len = 0;
    uint8_t  status = 0;   // 0 = success
    uint16_t error  = 0;

    if (f.command_id == 0x01) {
        if (!f.payload.empty()) { status=1; error=1004; }
        else extra_len = BuildDeviceInfo(extra,sizeof(extra));
        if (!status && !extra_len) { status=1; error=1005; }
        else if (!status) extra[22] = s_bat_current_pct >= 0 ? static_cast<uint8_t>(s_bat_current_pct) : 0;
    } else if (f.command_id == 0x37) {
        const auto& p = f.payload;
        if (!s_cfg.enable_ota || s_cfg.transport != AGENT_TRANSPORT_BLE || !agent_transport_ble_l2cap_ready()) {
            status = 1; error = 1003;
        } else if (p.size() < 38 || p[36] == 0 || p[36] > 31 || p.size() < size_t(37 + p[36])) {
            status = 1; error = 1004;
        } else {
            const size_t end = 37 + p[36];
            char ver[32]={}, model[40]={};
            memcpy(ver,p.data()+37,p[36]);
            if (p.size() > end) {
                if (!p[end] || p[end] > 39 || p.size() != end+1+p[end]) { status=1; error=1004; }
                else memcpy(model,p.data()+end+1,p[end]);
            }
            const uint32_t bytes = uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
            if (!status && (memchr(p.data()+37,0,p[36]) || (p.size()>end && memchr(p.data()+end+1,0,p[end])))) { status=1; error=1004; }
            if (!status && !agentlink::ota::Start(bytes,p.data()+4,ver,model[0]?model:nullptr,&error)) status=1;
        }
    } else if (f.command_id == 0x56) {
        if (!f.payload.empty()) { status=1; error=1004; }
        else agentlink::ota::Abort("peer");
    } else if (f.command_id == 0x03) {
        // 0x03 GetChargingStatus: answered by the SDK from cached battery. extra = [charging_state(0/1), level(0-100)].
        // The battery cache comes from report_battery (the board polls every 5s); payload must be empty, otherwise 1004.
        if (f.payload.empty()) {
            extra[0] = (s_bat_current_chg == 1) ? 0x01 : 0x00;
            extra[1] = (s_bat_current_pct >= 0) ? static_cast<uint8_t>(s_bat_current_pct) : 0;
            extra_len = 2;
        } else {
            status = 1; error = 1004;  // InvalidPayload
        }
    } else if (f.command_id == 0x33) {
        // 0x33 IoActuate: payload = [id_len(1)][id(UTF-8)][args...], routed by id to the registered actuator cb.
        const auto& pl = f.payload;
        char io_id[64];
        if (pl.empty() || pl[0] == 0 || pl[0] >= sizeof(io_id) || 1u + pl[0] > pl.size() ||
            memchr(pl.data()+1,0,pl[0])) {
            status = 1; error = 1004;  // InvalidPayload
        } else {
            const uint8_t id_len = pl[0];
            memcpy(io_id, pl.data() + 1, id_len);
            io_id[id_len] = '\0';
            const IoEntry* e = FindIo(io_id);
            if (e && e->desc->dir == AGENT_IO_OUT && e->cb) {
                const uint8_t* args     = pl.data() + 1 + id_len;
                const size_t   args_len = pl.size() - 1 - id_len;
                if (ValidateIoValue(e->desc->value, args, args_len) != ESP_OK ||
                    (e->cb == SynthScreenCb && args_len > 255)) {
                    status=1; error=1004;
                } else e->cb(io_id, args, args_len, e->ctx);
                ESP_LOGI(TAG, "actuate '%s' (%uB args)", io_id, static_cast<unsigned>(args_len));
            } else {
                status = 1; error = 1003;  // BusinessFailed: unknown or non-actuatable id
                ESP_LOGW(TAG, "actuate '%s': no matching OUT endpoint/callback", io_id);
            }
        }
    } else if (f.command_id == 0x34) {
        // 0x34 GetIoManifest: App-initiated fetch -> (re)send the manifest (0x18), then reply with the usual ACK.
        if (!f.payload.empty()) { status=1; error=1004; }
        else {
            SendManifest(/*force=*/true);
        }
    } else if (f.command_id == 0x35) {
        // GetReading: [id_len][id] -> [type][age_ms LE][cached value].
        const auto& pl = f.payload;
        char io_id[64];
        if (pl.empty() || pl[0] == 0 || pl[0] >= sizeof(io_id) || 1u + pl[0] != pl.size() ||
            memchr(pl.data()+1,0,pl[0])) {
            status = 1; error = 1004;
        } else {
            memcpy(io_id, pl.data() + 1, pl[0]);
            io_id[pl[0]] = '\0';
            const int idx = FindIoIndex(io_id);
            if (idx < 0 || IoEntries()[idx].desc->dir != AGENT_IO_IN) {
                status = 1; error = 1003;
            } else if (!IoEntries()[idx].has_last) {
                status = 1; error = 1005;
            } else {
                const uint32_t age = agent_link_now_ms() - IoEntries()[idx].last_ts_ms;
                const uint16_t age_ms = age > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(age);
                size_t k = 0;
                extra[k++] = static_cast<uint8_t>(IoEntries()[idx].desc->value);
                extra[k++] = static_cast<uint8_t>(age_ms & 0xFF);
                extra[k++] = static_cast<uint8_t>((age_ms >> 8) & 0xFF);
                memcpy(extra + k, IoEntries()[idx].last_val, IoEntries()[idx].last_len);
                extra_len = k + IoEntries()[idx].last_len;
            }
        }
    } else if (f.command_id == 0x36) {
        // SetReadingConfig: [id_len][id][mode][rate_hz LE].
        const auto& pl = f.payload;
        char io_id[64];
        if (pl.empty() || pl[0] == 0 || pl[0] >= sizeof(io_id) ||
            (pl.size() != static_cast<size_t>(2 + pl[0]) && pl.size() != static_cast<size_t>(4 + pl[0])) ||
            memchr(pl.data()+1,0,pl[0])) {
            status = 1; error = 1004;
        } else {
            const uint8_t id_len = pl[0];
            memcpy(io_id, pl.data() + 1, id_len);
            io_id[id_len] = '\0';
            const uint8_t mode = pl[1 + id_len];
            uint16_t rate = 0;
            if (static_cast<size_t>(4 + id_len) <= pl.size()) {
                rate = static_cast<uint16_t>(pl[2 + id_len]) |
                       (static_cast<uint16_t>(pl[3 + id_len]) << 8);
            }
            const int idx = FindIoIndex(io_id);
            if (idx < 0 || IoEntries()[idx].desc->dir != AGENT_IO_IN) {
                status = 1; error = 1003;
            } else if (mode > IO_POLICY_ONCHANGE || (mode == IO_POLICY_PERIODIC && rate == 0)) {
                status = 1; error = 1004;
            } else {
                IoEntries()[idx].policy_mode = mode;
                IoEntries()[idx].policy_rate_hz = rate;
                IoEntries()[idx].last_sent_ms = 0;
                IoEntries()[idx].has_sent = false;
                IoEntries()[idx].pending = IoEntries()[idx].has_last;
                ESP_LOGI(TAG, "reading config '%s': mode=%u rate=%uHz", io_id, mode,
                         static_cast<unsigned>(rate));
            }
        }
    } else if (f.command_id == 0x3C) {
        // StartCapture: [mode][max_ms LE]. The board owns microphone capture.
        if (f.payload.size() != 3) { status=1; error=1004; }
        else if (!(s_cfg.caps & AGENT_CAP_MIC) || !(s_have_out && s_out.on_listen)) {
            status = 1; error = 1002;
        } else {
            uint32_t max_ms = 0;
            if (f.payload.size() >= 3) {
                max_ms = static_cast<uint32_t>(f.payload[1]) |
                         (static_cast<uint32_t>(f.payload[2]) << 8);
            }
            s_out.on_listen(true, max_ms, s_out.ctx);
        }
    } else if (f.command_id == 0x3D) {
        if (!f.payload.empty()) { status=1; error=1004; }
        else if (s_have_out && s_out.on_listen) s_out.on_listen(false, 0, s_out.ctx);
    } else if (f.command_id == 0x05) {
        // VoiceReply is handled below after its fixed payload has been checked.
        // Keep it out of on_command: it is an SDK protocol command.
        if (f.payload.size() < 5 || f.payload.size() > 6) { status = 1; error = 1004; }
        else if (f.payload.size() == 6 && !agentlink::audio::SetCodec(f.payload[5])) { status=1; error=1004; }
    } else if (s_have_out && s_out.on_command &&
               s_out.on_command(f.command_id, f.payload.data(), f.payload.size(),
                                extra, sizeof(extra), &extra_len, s_out.ctx)) {
        // handled by the device (extra may carry response data); status stays 0.
    } else { status=1; error=1001; extra_len=0; }

    // 0x05 VoiceReply: payload = session_id(4) + status(1). status=3 -> playback finished -> on_audio_end.
    // (status=2 carries an audio reply: the TTS then arrives over L2CAP, going through OnStreamData -> on_audio_out.)
    if (!status && f.command_id == 0x05 && f.payload.size() >= 5) {
        if (f.payload[4] == 2) { s_audio_end_pending=s_audio_finished=false; agentlink::audio::Arm(); }
        if (f.payload[4] == 3) {
            agentlink::audio::Finish();
            s_audio_end_pending=true;
            s_audio_finished=true;
            if (agentlink::audio::IsDrained()) {
                s_audio_end_pending=false;
                if (s_have_out && s_out.on_audio_end) s_out.on_audio_end(s_out.ctx);
            }
        }
        s_voice_reply_seen=true;
        s_voice_reply_sequence=f.sequence;
        s_voice_reply_length=f.payload.size();
        memcpy(s_voice_reply_payload,f.payload.data(),s_voice_reply_length);
    }
    if (extra_len > sizeof(extra) || extra_len + 10 > ControlMtu()) {
        extra_len=0; status=1; error=1004;
    }

    // Reply (request-response pairing); a query command carrying extra_data returns its data to the App here.
    s_response_size = agentlink::EncodeResponse(s_response,sizeof(s_response),f.command_id,
        f.sequence,status,error,extra_len ? extra : nullptr,extra_len);
}

// Transport -> core: a data-plane chunk arrived (e.g. downlink TTS voice over L2CAP).
size_t OnStreamData(agent_stream_t type, const uint8_t* data, size_t len) {
    if (type == AGENT_STREAM_VOICE) {
        // Enqueue quickly for playback; on_audio_out runs in poll().
        if (s_audio_finished) return len; // Late bytes after protocol END cannot hold up the next START.
        return agentlink::audio::Feed(data,len);
    }
    // Unsupported media are discarded rather than blocking later control.
    return len;
}
}  // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────────
esp_err_t agent_link_init(const agent_link_config_t* cfg) {
    if (s_running || s_polling) return ESP_ERR_INVALID_STATE;
    if (agent_transport_ble_active() || agentlink::ota::WorkerActive()) return ESP_ERR_INVALID_STATE;
    if (!cfg || !cfg->device_name || !cfg->device_name[0]) return ESP_ERR_INVALID_ARG;
    const char* model=cfg->model && *cfg->model ? cfg->model : cfg->device_name;
    const char* firmware=cfg->firmware_rev && *cfg->firmware_rev ? cfg->firmware_rev : "2.0.0";
    if (BoundedLength(cfg->device_name,127)>127 ||
        BoundedLength(model,AGENT_LINK_MODEL_MAX)>AGENT_LINK_MODEL_MAX ||
        BoundedLength(firmware,AGENT_LINK_FIRMWARE_MAX)>AGENT_LINK_FIRMWARE_MAX ||
        BoundedLength(cfg->manufacturer,AGENT_LINK_MANUFACTURER_MAX)>AGENT_LINK_MANUFACTURER_MAX)
        return ESP_ERR_INVALID_SIZE;
    if (cfg->transport != AGENT_TRANSPORT_BLE && cfg->transport != AGENT_TRANSPORT_CUSTOM) return ESP_ERR_INVALID_ARG;
    if (cfg->transport == AGENT_TRANSPORT_CUSTOM && (!cfg->custom_transport || !cfg->custom_transport->start || !cfg->custom_transport->stop || !cfg->custom_transport->send_ctrl || !cfg->custom_transport->is_ready || !cfg->custom_transport->bind)) return ESP_ERR_INVALID_ARG;
    if (!CanRegisterSyntheticEndpoints(*cfg)) return ESP_ERR_NO_MEM;
    // These buffers are used only by the application's poll/setup thread.
    // Reserve before replacing configuration; allocation failure leaves the
    // previous registry and transport bindings intact. ISR/RX storage stays internal.
    if (!s_io_storage.prepare() || !s_manifest_storage.prepare()) return ESP_ERR_NO_MEM;
    if (s_initialized && s_cfg.transport == AGENT_TRANSPORT_CUSTOM && s_tx->bind) s_tx->bind(s_tx->impl,nullptr);
    // Synthetic endpoints belong to the configuration being replaced. User I/O survives stop.
    for (int i=0; i<s_io_count;) {
        const auto* d=IoEntries()[i].desc;
        if (d==&kSynthLed || d==&kSynthHaptic || d==&kSynthScreen) {
            for (int j=i+1; j<s_io_count; ++j) IoEntries()[j-1]=IoEntries()[j];
            IoEntries()[--s_io_count]={};
        } else ++i;
    }
    s_cfg = *cfg;
    s_name = cfg->device_name;
    s_manufacturer = cfg->manufacturer && *cfg->manufacturer ? cfg->manufacturer : "Deotaland";
    s_model = cfg->model && *cfg->model ? cfg->model : s_name;
    s_firmware = cfg->firmware_rev && *cfg->firmware_rev ? cfg->firmware_rev : "2.0.0";
    s_cfg.device_name=s_name.c_str(); s_cfg.manufacturer=s_manufacturer.c_str();
    s_cfg.model=s_model.c_str(); s_cfg.firmware_rev=s_firmware.c_str();
    dev_identity_override(cfg->device_id);
    s_identity_text[0]=0;
    (void)dev_identity_load(false);
    s_cfg.device_id=dev_identity_uuid(s_device_id)?s_device_id:nullptr;
    agentlink::StatusReset(s_cfg.transport);
    s_out = cfg->output ? *cfg->output : agent_output_cb_t{};
    s_have_out = cfg->output != nullptr;
    s_cfg.output = s_have_out ? &s_out : nullptr;
    s_state=AGENT_STATE_DISCONNECTED;
    s_stats={};
    s_service_cursor=s_end_cursor=s_reading_cursor=0; s_audio_end_pending=s_audio_finished=false;
    s_voice_reply_seen=false;
    s_bat_current_pct=s_bat_current_chg=-1;
    agentlink::audio::SetSinks(OnDecodedPcm,SendServiceEvent);
    s_playback={};
    agentlink::audio::SetReliableEventSink(SendReliableServiceEvent);
    agentlink::ota::SetEventSink(SendServiceEvent);
    agentlink::ota::SetReliableEventSink(SendReliableServiceEvent);
    agentlink::ota::SetDeviceModel(s_cfg.model);
    agentlink::ota::Configure(s_cfg.enable_ota,s_cfg.transport==AGENT_TRANSPORT_BLE);
    switch (s_cfg.transport) {
    case AGENT_TRANSPORT_CUSTOM:
        s_tx=s_cfg.custom_transport;
        s_tx->bind(s_tx->impl,&kCallbacks);
        break;
    default:
        s_tx=agent_transport_ble();
        agent_transport_ble_set_name(s_cfg.device_name);
        agent_transport_ble_set_recv(QueueCtrlLegacy);
        agent_transport_ble_set_conn(QueueConnection);
        agent_transport_ble_set_ready(QueueReady);
        agent_transport_ble_set_stream_recv(QueueStreamLegacy);
        agent_transport_ble_set_data_receiver(ReceiveBulk,ReceiveSpace);
        agent_transport_ble_set_device_info(s_cfg.manufacturer,s_cfg.model,s_cfg.firmware_rev);
        if (s_tx->bind) s_tx->bind(s_tx->impl,&kCallbacks);
    }
    RegisterSyntheticEndpoints();
    s_initialized=true;
    return ESP_OK;
}

esp_err_t agent_link_start(void) {
    if (!s_initialized || s_running || s_polling || !s_tx || !s_tx->start) return ESP_ERR_INVALID_STATE;
    agentlink::RxEnable(true);
    s_generation=agentlink::RxGeneration();
    s_running=true;
    LinkStatus(AGENT_LINK_PHASE_SETUP, "Set up link", "Open the App to connect");
    const esp_err_t e=s_tx->start(s_tx->impl);
    if (e!=ESP_OK) { s_running=false; agentlink::RxEnable(false); agentlink::StatusReset(s_cfg.transport); }
    return e;
}
bool agent_link_can_begin(void) {
    return !s_running && !s_polling && !agent_transport_ble_active() && !agentlink::ota::WorkerActive();
}
void agent_link_stop(void) {
    if (s_polling) { s_stats.last_error=ESP_ERR_INVALID_STATE; return; }
    agentlink::RxEnable(false);
    s_rx_pending=false;
    if (s_initialized && s_tx && s_tx->stop) s_tx->stop(s_tx->impl);
    s_running=false;
    s_state=AGENT_STATE_DISCONNECTED;
    agentlink::StatusReset(s_cfg.transport);
    for(auto& stream:s_streams) stream.open=false;
    for(auto& pending:s_pending_stream_end) pending=false;
    if (s_manifest_storage.prepared()) Manifest().clear();
    s_manifest_building=false; s_manifest_sent=false; s_response_size=0;
    agentlink::audio::OnLinkState(false);
    s_audio_end_pending=false;
    s_voice_reply_seen=false;
    agentlink::ota::OnLinkState(false);
}
void agent_link_deinit(void) {
    if (s_polling) { s_stats.last_error=ESP_ERR_INVALID_STATE; return; }
    agent_link_stop();
    if(s_tx && s_tx->bind) s_tx->bind(s_tx->impl,nullptr);
    agent_transport_ble_set_recv(nullptr);
    agent_transport_ble_set_conn(nullptr);
    agent_transport_ble_set_ready(nullptr);
    agent_transport_ble_set_stream_recv(nullptr);
    agent_transport_ble_set_data_receiver(nullptr,nullptr);
    agent_link_ota_set_callback(nullptr,nullptr);
    agentlink::ota::SetReliableEventSink(nullptr);
    agentlink::ota::SetEventSink(nullptr);
    agentlink::audio::SetSinks(nullptr,nullptr);
    s_playback={};
    s_tx=nullptr; s_initialized=false; s_cfg={}; s_out={}; s_have_out=false;
    if (s_io_storage.prepared()) for(auto& io:IoEntries()) io={};
    s_io_count=0; s_manifest_rev=1;
    s_manifest_token_count=s_manifest_token_index=0;
    for(auto& token:s_manifest_tokens) token={};
    s_manifest_escape_size=s_manifest_escape_offset=0;
    s_manifest_final_tokens=false;
    s_manifest_storage.release(); s_io_storage.release();
}
agent_state_t agent_link_state(void) {
    if (!s_running || s_generation != agentlink::RxGeneration()) return AGENT_STATE_DISCONNECTED;
    if (s_state == AGENT_STATE_READY && !Ready()) return AGENT_STATE_CONNECTED;
    return s_state;
}
void agent_link_get_status(agent_link_status_t* out) { agentlink::StatusGet(out); }
const char* agent_link_device_id(void) {
    if (!s_initialized) return nullptr;
    (void)dev_identity_try_sn(s_identity_text);
    return s_identity_text[0] ? s_identity_text : nullptr;
}
esp_err_t agent_link_playback_configure(const agent_playback_config_t* config) {
    if (!s_initialized || s_polling || agentlink::audio::IsArmed() ||
        agentlink::audio::PendingBytes() || s_audio_end_pending) return ESP_ERR_INVALID_STATE;
    if (config && !config->write) return ESP_ERR_INVALID_ARG;
    s_playback=config?*config:agent_playback_config_t{};
    agentlink::audio::SetPlaybackSink(config?AcceptPcm:nullptr,config && config->queued?PlaybackQueued:nullptr,nullptr);
    return ESP_OK;
}
void agent_link_playback_get_stats(agent_playback_stats_t* out) { agentlink::audio::GetStats(out); }
bool agent_link_playback_active(void) { return Ready() && (agentlink::audio::IsArmed() || s_audio_end_pending); }
esp_err_t agent_link_forget(void) {
    if (!s_running || s_polling) return ESP_ERR_INVALID_STATE;
    if (s_cfg.transport == AGENT_TRANSPORT_BLE) return agent_transport_ble_forget();
    return ESP_ERR_NOT_SUPPORTED;
}
void agent_link_get_stats(agent_link_stats_t* out) {
    if(out) { *out=s_stats; agentlink::RxStats(out); }
}
size_t agent_link_poll(size_t budget) {
    return agent_link_poll_budget(budget,AGENT_LINK_POLL_BUDGET_US);
}
namespace {
void RetryResponse(bool& blocked) {
    if(s_generation!=agentlink::RxGeneration()) { s_response_size=0; return; }
    if(!s_response_size || blocked) return;
    const esp_err_t e=SendControl(s_response,s_response_size);
    if(e==ESP_ERR_NO_MEM || e==ESP_ERR_TIMEOUT) {
        ++s_stats.tx_retries; ++s_stats.control_backpressure; blocked=true;
    } else s_response_size=0;
}
bool PumpRx(bool& response_blocked) {
    RetryResponse(response_blocked);
    const uint32_t generation=agentlink::RxGeneration();
    if(s_rx_pending && s_rx_event.generation!=generation) s_rx_pending=false;
    if(!s_rx_pending) {
        if(!agentlink::RxPop(s_rx_event)) return false;
        s_rx_offset=0; s_rx_pending=true;
    }
    auto& event=s_rx_event;
    if(event.generation!=agentlink::RxGeneration()) { s_rx_pending=false; return true; }
    // A response fences the NEXT command, not independent media already queued
    // after the command we handled. Never overtake an unhandled command or END.
    if(event.kind==agentlink::RxKind::Control) {
        if(s_response_size) return false;
        if(s_audio_end_pending) {
            agentlink::FrameView f;
            const bool start=agentlink::ParseFrameView(event.data,event.length,f) &&
                !f.encrypted && f.msg_type==agentlink::kMsgCommand && f.command_id==0x05 &&
                (f.payload.size()==5 || (f.payload.size()==6 && f.payload[5]<=agentlink::audio::kCodecImaAdpcm)) &&
                f.payload[4]==2;
            if(start) return false; // Other control remains usable while the speaker drains.
        }
    }
    s_rx_pending=false;
    switch(event.kind) {
    case agentlink::RxKind::Connection: s_generation=event.generation; OnConn(event.data[0]!=0); break;
    case agentlink::RxKind::Ready: if(s_state!=AGENT_STATE_DISCONNECTED) OnLinkReady(); break;
    case agentlink::RxKind::Control: if(ControlReady()) OnCtrlFrame(event.data,event.length); break;
    case agentlink::RxKind::Stream:
        if(Ready()) {
            const size_t limit=agentlink::audio::CodecValue()==agentlink::audio::kCodecPcm16?512:256;
            const size_t n=std::min(limit,event.length-s_rx_offset);
            const size_t accepted=OnStreamData(event.stream,event.data+s_rx_offset,n);
            s_rx_offset+=std::min(n,accepted);
            s_rx_pending=s_rx_offset<event.length;
            if(!accepted && n) { ++s_stats.playback_backpressure; return false; }
        }
        break;
    case agentlink::RxKind::DeferredStream: {
        if(event.length!=8) break;
        uint32_t ticket=0, origin=0;
        for(unsigned i=0;i<4;++i) {
            ticket |= uint32_t(event.data[i])<<(i*8);
            origin |= uint32_t(event.data[i+4])<<(i*8);
        }
        if(!agentlink::RxDeferredBegin(ticket,event.generation)) break;
        s_direct_bulk=true; s_direct_bulk_generation=origin;
        if(s_tx && s_tx->poll) s_tx->poll(s_tx->impl);
        s_direct_bulk=false;
        const bool finished=agentlink::RxDeferredDone(ticket);
        agentlink::RxDeferredEnd(ticket);
        s_rx_pending=!finished && event.generation==agentlink::RxGeneration();
        if(s_rx_pending) return false; // END/next START must remain behind this SDU.
        break;
    }
    }
    return true;
}
void PumpAudio() {
    agentlink::audio::Tick();
    if(s_audio_end_pending && agentlink::audio::IsDrained()) {
        s_audio_end_pending=false;
        if(Ready() && s_have_out && s_out.on_audio_end) s_out.on_audio_end(s_out.ctx);
    }
}
}
size_t agent_link_poll_budget(size_t budget, uint32_t max_us) {
    if (!s_running || s_polling || !budget) return 0;
    s_polling=true;
    const uint32_t started=agent_link_now_us();
    const auto expired=[&]() { return max_us && uint32_t(agent_link_now_us()-started)>=max_us; };
    if(s_generation!=agentlink::RxGeneration()) {
        s_response_size=0; s_rx_pending=false;
        agentlink::audio::OnLinkState(false);
        s_audio_end_pending=s_audio_finished=false;
        s_voice_reply_seen=false;
    }
    size_t done=0;
    bool response_blocked=false;
    bool rx_available=true;
    // Resume BEFORE the next service after exhaustion. Even a slow backend or
    // application callback cannot monopolize every subsequent poll invocation.
    constexpr unsigned kServices=9;
    for(unsigned work=0;work<kServices && !expired();++work) {
        const unsigned service=s_service_cursor;
        s_service_cursor=(s_service_cursor+1)%kServices;
        switch(service) {
        case 0: RetryResponse(response_blocked); break;
        case 1: if(s_tx->poll) s_tx->poll(s_tx->impl); break;
        case 2: PumpAudio(); break;
        case 3: agentlink::ota::Poll(); break;
        case 4:
            for(unsigned i=0;i<AGENT_STREAM_KIND_COUNT;++i) {
                const unsigned index=s_end_cursor++%AGENT_STREAM_KIND_COUNT;
                if(!s_pending_stream_end[index]) continue;
                const esp_err_t e=agent_link_stream_close(s_streams[index].handle,s_pending_stream_complete[index]);
                if(e!=ESP_OK) { ++s_stats.tx_retries; s_stats.last_error=e; }
                break;
            }
            break;
        case 5: {
            agent_link_status_t status;
            if(agentlink::StatusTake(&status) && s_cfg.on_status) s_cfg.on_status(&status,s_cfg.status_ctx);
            break;
        }
        case 6: if(!s_response_size || s_manifest_building) PumpManifest(); break;
        case 7: PumpReading(); break;
        case 8: rx_available=PumpRx(response_blocked); if(rx_available) ++done; break;
        }
    }
    while(rx_available && done<budget && !expired()) {
        rx_available=PumpRx(response_blocked);
        if(rx_available) ++done;
    }
    if(!expired()) RetryResponse(response_blocked);
    const uint32_t elapsed=agent_link_now_us()-started;
    ++s_stats.poll_calls;
    s_stats.poll_last_us=elapsed;
    s_stats.poll_max_us=std::max(s_stats.poll_max_us,elapsed);
    if(max_us && elapsed>=max_us) ++s_stats.poll_budget_exhaustions;
    s_polling=false;
    return done;
}

// ── Uplink: events / status (control plane, wired) ───────────────────────────────
esp_err_t agent_link_report_battery(uint8_t percent, bool charging) {
    // Event 0x14 (PowerStatus). payload = {charging_state, level}:
    //   charging_state: 0x00 = not charging, 0x01 = charging, 0x02 = low-battery alert
    //   (below kBatLowThreshold, edge-triggered once).
    // Pushed only on change: the device may call this periodically; the SDK de-duplicates internally.
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    s_bat_current_pct=percent; s_bat_current_chg=charging?1:0;
    if(s_initialized && s_cfg.transport==AGENT_TRANSPORT_BLE)
        agent_transport_ble_update_battery(percent);
    if (!Ready() || !s_tx->send_ctrl) return ESP_ERR_INVALID_STATE;
    const int chg = charging ? 1 : 0;
    const bool changed  = (static_cast<int>(percent) != s_bat_pct) || (chg != s_bat_chg);
    const bool low_edge = (percent < kBatLowThreshold) && s_bat_low_armed;
    if (!changed && !low_edge) return ESP_OK;  // no change: do not push

    const uint8_t state = low_edge ? 0x02 : static_cast<uint8_t>(chg);
    uint8_t p[2] = { state, percent };
    esp_err_t r = SendReliableServiceEvent(0x14,p,sizeof(p));
    if (r != ESP_OK) return r;  // send failed (not connected / congested): cache not updated, next poll retries so it eventually lands

    // Update the cache / re-arm the alert only after a successful send.
    s_bat_pct = percent;
    s_bat_chg = chg;
    s_bat_low_armed = (percent >= kBatLowThreshold);  // re-arm once back above the threshold; disarm below (already alerted)
    ESP_LOGD(TAG, "battery event 0x14: state=%u level=%u%%", state, percent);
    return ESP_OK;
}
esp_err_t agent_link_report_selected_agent(const char* agent_id) {
    if (!agent_id || !agent_id[0]) return ESP_ERR_INVALID_ARG;
    if (!Ready()) return not_ready("report_selected_agent");
    // The upstream 0x16 selection event has no completed mapping from Agent ID
    // to its index/name payload. Report support honestly once the link is ready.
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t agent_link_push_event(agent_event_t type, const uint8_t* data, size_t len) {
    if (len > 0 && !data) return ESP_ERR_INVALID_ARG;
    if (len > UINT16_MAX || len > PayloadBudget()) return ESP_ERR_INVALID_SIZE;
    if (type != AGENT_EVT_PROMPT && type != AGENT_EVT_BUTTON && type != AGENT_EVT_SENSOR &&
        type != AGENT_EVT_WAKEWORD && type != AGENT_EVT_CUSTOM) return ESP_ERR_INVALID_ARG;
    if (!s_tx || !s_tx->send_ctrl) return ESP_ERR_INVALID_STATE;
    if (!(s_tx->is_ready && s_tx->is_ready(s_tx->impl))) return not_ready("push_event");
    return SendReliableServiceEvent(static_cast<uint8_t>(type),data,len);
}

// ── Generic I/O: sensors / actuators ──
esp_err_t agent_link_register_io(const agent_link_io_desc_t* desc,
                                 agent_io_actuate_cb_t cb, void* ctx) {
    if (!desc || !desc->id || !desc->kind) return ESP_ERR_INVALID_ARG;
    if (desc->id[0] == '\0' || BoundedLength(desc->id,63) > 63 || desc->kind[0] == '\0' ||
        !std::isfinite(desc->range_min) || !std::isfinite(desc->range_max) ||
        desc->range_min > desc->range_max ||
        (desc->dir != AGENT_IO_IN && desc->dir != AGENT_IO_OUT) ||
        desc->value < AGENT_VAL_BOOL || desc->value > AGENT_VAL_STR ||
        desc->audience < AGENT_AUD_AI || desc->audience > AGENT_AUD_USER ||
        desc->event < AGENT_EVT_PERIODIC || desc->event > AGENT_EVT_THRESHOLD) {
        return ESP_ERR_INVALID_ARG;
    }
    if (FindIo(desc->id)) {
        ESP_LOGW(TAG, "register_io: duplicate id '%s'", desc->id);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_io_count >= kMaxIo) {
        ESP_LOGE(TAG, "register_io: registry full (max %d)", kMaxIo);
        return ESP_ERR_NO_MEM;
    }
    if (!s_io_storage.prepare()) return ESP_ERR_NO_MEM;
    IoEntries()[s_io_count] = {};
    IoEntries()[s_io_count].desc = desc;
    IoEntries()[s_io_count].cb = cb;
    IoEntries()[s_io_count].ctx = ctx;
    ESP_LOGI(TAG, "register_io[%d]: id='%s' kind='%s' dir=%s",
             s_io_count, desc->id, desc->kind, desc->dir == AGENT_IO_OUT ? "out" : "in");
    s_io_count++;
    s_manifest_sent=false;
    if (s_running) (void)agent_link_notify_manifest_changed();
    // The manifest is serialized and sent during the connection handshake (BLE: after the 0xFFC4
    // subscribe), see SendManifest. Downlink actuator commands (0x33) are
    // matched by id to IoEntries()[].cb, see OnCtrlFrame.
    return ESP_OK;
}

namespace {
void CacheReading(IoEntry& e,const void* value,size_t len) {
    e.last_ts_ms=agent_link_now_ms();
    if (len <= sizeof(e.last_val)) {
        memcpy(e.last_val, value, len);
        e.last_len = static_cast<uint8_t>(len);
        e.has_last = true;
    } else { e.has_last = false; }
}
esp_err_t SendReading(IoEntry& e,const void* value,size_t len) {
    const uint32_t now_ms=agent_link_now_ms();
    switch (e.policy_mode) {
    case IO_POLICY_OFF:
        return ESP_OK;
    case IO_POLICY_PERIODIC: {
        const uint32_t period_ms = e.policy_rate_hz ? (1000u + e.policy_rate_hz - 1u) / e.policy_rate_hz : 0;
        if (period_ms && e.has_sent && uint32_t(now_ms - e.last_sent_ms) < period_ms) return ESP_OK;
        break;
    }
    case IO_POLICY_ONCHANGE:
        if (e.has_sent && e.sent_len==len && memcmp(e.sent_val,value,len)==0) return ESP_OK;
        break;
    default:
        break;
    }

    const char* id=e.desc->id;
    const size_t id_len=BoundedLength(id,63);
    if(!id_len || id_len>63 || len>UINT16_MAX-id_len-2 || id_len+2+len>PayloadBudget()) return ESP_ERR_INVALID_SIZE;
    uint8_t frame[1030];
    uint8_t* p=frame+agentlink::kHeaderSize;
    p[0]=static_cast<uint8_t>(id_len);
    memcpy(p+1,id,id_len);
    p[id_len+1]=static_cast<uint8_t>(e.desc->value);
    memcpy(p+id_len+2,value,len);
    const size_t bytes=agentlink::EncodeEvent(frame,sizeof(frame),0x19,p,id_len+2+len);
    const esp_err_t result=SendControl(frame,bytes);
    if (result == ESP_OK) {
        e.last_sent_ms=now_ms;
        e.has_sent=true;
        e.sent_len=0;
        if(len<=sizeof(e.sent_val)) { memcpy(e.sent_val,value,len); e.sent_len=static_cast<uint8_t>(len); }
    }
    return result;
}
void PumpReading() {
    if(!Ready() || s_response_size) return;
    // Bounded scan; select at most one endpoint, preserving fairness on retry.
    for(int i=0;i<s_io_count;++i) {
        s_reading_cursor%=static_cast<unsigned>(s_io_count);
        auto& e=IoEntries()[s_reading_cursor++];
        if(!e.has_last || e.desc->dir!=AGENT_IO_IN || e.policy_mode==IO_POLICY_OFF) continue;
        if(e.policy_mode==IO_POLICY_PERIODIC) {
            const uint32_t period=(1000u+e.policy_rate_hz-1u)/e.policy_rate_hz;
            if(e.has_sent && uint32_t(agent_link_now_ms()-e.last_sent_ms)<period) continue;
        } else if(!e.pending) continue;
        const esp_err_t result=SendReading(e,e.last_val,e.last_len);
        if(result==ESP_OK) e.pending=false;
        else {
            // Keep the sample for a later MTU change/reconnect, but expose size
            // failures too: SendControl is never reached for oversized readings.
            s_stats.last_error=result;
            if(result==ESP_ERR_NO_MEM || result==ESP_ERR_TIMEOUT) ++s_stats.tx_retries;
        }
        return;
    }
}
esp_err_t ValidateReading(const char* id,const void* value,size_t len,int& idx) {
    if(!id || !value) return ESP_ERR_INVALID_ARG;
    if(BoundedLength(id,63)>63) return ESP_ERR_INVALID_ARG;
    idx=FindIoIndex(id);
    if(idx<0 || IoEntries()[idx].desc->dir!=AGENT_IO_IN) return ESP_ERR_NOT_FOUND;
    const auto type=IoEntries()[idx].desc->value;
    if(!len && type!=AGENT_VAL_STR) return ESP_ERR_INVALID_ARG;
    return ValidateIoValue(type,value,len);
}
}
esp_err_t agent_link_push_reading(const char* id,const void* value,size_t len) {
    if(!id || !value) return ESP_ERR_INVALID_ARG;
    if(!Ready() || !s_tx->send_ctrl) return ESP_ERR_INVALID_STATE;
    // Reject an impossible variable-length value before traversing UTF-8. The
    // control budget is also the bound on validation work in this immediate API.
    if(len>PayloadBudget()) return ESP_ERR_INVALID_SIZE;
    int idx=0; const esp_err_t valid=ValidateReading(id,value,len,idx);
    if(valid!=ESP_OK) return valid;
    auto& e=IoEntries()[idx];
    const size_t id_len=BoundedLength(id,63);
    if(len>UINT16_MAX-id_len-2 || id_len+2+len>PayloadBudget()) return ESP_ERR_INVALID_SIZE;
    CacheReading(e,value,len);
    e.pending=false; // Legacy immediate API: caller owns retries on rejection.
    return SendReading(e,value,len);
}
esp_err_t agent_link_update_reading(const char* id,const void* value,size_t len) {
    if(!id || !value) return ESP_ERR_INVALID_ARG;
    // Check capacity before validating bytes: this API never retains a larger
    // sample, so walking an arbitrarily long STR would only delay loop().
    if(len>AGENT_LINK_READING_CACHE_BYTES) return ESP_ERR_INVALID_SIZE;
    int idx=0; const esp_err_t valid=ValidateReading(id,value,len,idx);
    if(valid!=ESP_OK) return valid;
    auto& e=IoEntries()[idx];
    if(e.pending) ++s_stats.reading_coalesces;
    CacheReading(e,value,len); e.pending=true;
    return ESP_OK;
}

esp_err_t agent_link_update_reading_typed(const char* id,agent_val_t type,const void* value,size_t len) {
    if(!id || !value || BoundedLength(id,63)>63) return ESP_ERR_INVALID_ARG;
    const IoEntry* entry=FindIo(id);
    if(!entry || entry->desc->dir!=AGENT_IO_IN) return ESP_ERR_NOT_FOUND;
    if(entry->desc->value!=type) return ESP_ERR_INVALID_ARG;
    return agent_link_update_reading(id,value,len);
}

esp_err_t agent_link_notify_manifest_changed(void) {
    if (!s_tx || !s_tx->send_ctrl) return ESP_ERR_INVALID_STATE;
    ++s_manifest_rev;
    s_manifest_sent = false;
    const uint8_t p[4] = {
        static_cast<uint8_t>(s_manifest_rev),
        static_cast<uint8_t>(s_manifest_rev >> 8),
        static_cast<uint8_t>(s_manifest_rev >> 16),
        static_cast<uint8_t>(s_manifest_rev >> 24),
    };
    if (ControlReady()) {
        const esp_err_t result = SendReliableServiceEvent(0x1A,p,sizeof(p));
        (void)result;
    }
    SendManifest(/*force=*/true);
    ESP_LOGI(TAG, "manifest changed -> rev=%u", static_cast<unsigned>(s_manifest_rev));
    return ESP_OK;
}

esp_err_t agent_link_push_prompt(const char* text) {
    if (!text || !*text) return ESP_ERR_INVALID_ARG;
    const size_t limit=PayloadBudget();
    const size_t size=BoundedLength(text,limit);
    if(size>limit) return ESP_ERR_INVALID_SIZE;
    return agent_link_push_event(AGENT_EVT_PROMPT,reinterpret_cast<const uint8_t*>(text),size);
}

size_t agent_link_event_payload_capacity(void) {
    return Ready() && s_tx->send_ctrl ? PayloadBudget() : 0;
}

namespace {
agent_stream_session* FindStream(agent_stream_handle_t h) {
    // Validate before dereferencing a caller-supplied handle.
    for (auto& stream : s_streams) if (h && stream.handle == h) return &stream;
    return nullptr;
}
esp_err_t CloseStream(agent_stream_handle_t h, bool complete, const uint8_t* meta, size_t n) {
    auto* stream = FindStream(h);
    if (!stream) return ESP_ERR_INVALID_ARG;
    if (!stream->open) return ESP_OK;
    if (!Ready() || !s_tx->stream_end) return ESP_ERR_INVALID_STATE;
    const esp_err_t e = s_tx->stream_end(s_tx->impl, stream->kind, complete, meta, n);
    // Backpressure is retryable: don't lose the session until END is accepted.
    if (e == ESP_OK) { stream->open = false; s_pending_stream_end[stream->kind]=false; }
    return e;
}
}

esp_err_t agent_link_stream_open(agent_stream_t kind, const agent_stream_opts_t* opts,
                                agent_stream_handle_t* out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = nullptr;
    if (static_cast<unsigned>(kind) >= AGENT_STREAM_KIND_COUNT) return ESP_ERR_INVALID_ARG;
    if (!Ready()) return ESP_ERR_INVALID_STATE;
    if (!s_tx->stream_start || !s_tx->send_stream || !s_tx->stream_end) return ESP_ERR_NOT_SUPPORTED;
    auto& stream = s_streams[kind];
    if (s_pending_stream_end[kind]) return ESP_ERR_INVALID_STATE;
    if (stream.open) { *out = stream.handle; return ESP_OK; }
    if (s_handle_serial == UINTPTR_MAX) return ESP_ERR_INVALID_STATE;
    uint8_t meta[32];
    size_t n = 0;
    if (opts) {
        if ((kind == AGENT_STREAM_AUDIO || kind == AGENT_STREAM_FILE) && opts->name) {
            n = BoundedLength(opts->name,sizeof(meta));
            if (n > sizeof(meta)) return ESP_ERR_INVALID_SIZE;
            memcpy(meta, opts->name, n);
        } else if (kind == AGENT_STREAM_IMAGE) {
            if (opts->encoding != AGENT_ENC_RAW && opts->encoding != AGENT_ENC_JPEG) return ESP_ERR_INVALID_ARG;
            if (!opts->width || !opts->height) return ESP_ERR_INVALID_ARG;
            // IMAGE's wire header requires the final length; zero previously
            // opened a BLE session that rejected every nonempty write.
            uint32_t total=opts->total_bytes;
            if (opts->encoding==AGENT_ENC_RAW) {
                const uint64_t raw=uint64_t(opts->width)*opts->height*2u;
                if (raw>UINT32_MAX) return ESP_ERR_INVALID_SIZE;
                if(total && total!=raw) return ESP_ERR_INVALID_SIZE;
                total=static_cast<uint32_t>(raw);
            }
            if (!total) return ESP_ERR_INVALID_ARG;
            n = 9;
            meta[0] = static_cast<uint8_t>(opts->encoding);
            meta[1] = static_cast<uint8_t>(opts->width);
            meta[2] = static_cast<uint8_t>(opts->width >> 8);
            meta[3] = static_cast<uint8_t>(opts->height);
            meta[4] = static_cast<uint8_t>(opts->height >> 8);
            for (unsigned i=0; i<4; ++i) meta[5+i] = static_cast<uint8_t>(total >> (i*8));
        }
    }
    if (kind == AGENT_STREAM_IMAGE && !opts) return ESP_ERR_INVALID_ARG;
    const uint32_t generation=s_generation;
    const esp_err_t e = s_tx->stream_start(s_tx->impl, kind, n ? meta : nullptr, n);
    if (e == ESP_OK) {
        if(generation!=agentlink::RxGeneration()) return ESP_ERR_INVALID_STATE;
        stream.open = true;
        // Opaque integer token, never dereferenced; no heap allocation or slot reuse.
        stream.handle = reinterpret_cast<agent_stream_handle_t>(++s_handle_serial);
        *out = stream.handle;
    }
    return e;
}
esp_err_t agent_link_stream_write(agent_stream_handle_t handle, const void* data, size_t n) {
    auto* stream = FindStream(handle);
    if (!stream || !data || !n) return ESP_ERR_INVALID_ARG;
    if (!stream->open || s_pending_stream_end[stream->kind] || !Ready()) return ESP_ERR_INVALID_STATE;
    return s_tx->send_stream(s_tx->impl, stream->kind, static_cast<const uint8_t*>(data), n);
}
esp_err_t agent_link_stream_close(agent_stream_handle_t h, bool complete) {
    return CloseStream(h, complete, nullptr, 0);
}
bool agent_link_stream_is_open(agent_stream_t kind) {
    return s_running && s_generation == agentlink::RxGeneration() && static_cast<unsigned>(kind) < AGENT_STREAM_KIND_COUNT && s_streams[kind].open;
}
esp_err_t agent_link_prepare_streams(uint32_t mask) {
    if(!s_initialized || !s_running || s_polling) return ESP_ERR_INVALID_STATE;
    if(mask & ~((1u<<AGENT_STREAM_KIND_COUNT)-1u)) return ESP_ERR_INVALID_ARG;
    if(!mask) return ESP_OK;
    return s_tx->prepare_streams?s_tx->prepare_streams(s_tx->impl,mask):ESP_ERR_NOT_SUPPORTED;
}
esp_err_t agent_link_stream_get_status(agent_stream_t kind,agent_stream_status_t* out) {
    if(!out || static_cast<unsigned>(kind)>=AGENT_STREAM_KIND_COUNT) return ESP_ERR_INVALID_ARG;
    *out={};
    if(!s_initialized || !s_running) return ESP_ERR_INVALID_STATE;
    return s_tx->stream_status?s_tx->stream_status(s_tx->impl,kind,out):ESP_ERR_NOT_SUPPORTED;
}
esp_err_t agent_link_stream_send(agent_stream_t kind, const agent_stream_opts_t* opts,
                                const void* data, size_t n) {
    if (!data || !n || n > UINT32_MAX) return ESP_ERR_INVALID_ARG;
    // A one-shot transfer must not append to and close somebody else's stream.
    if (agent_link_stream_is_open(kind)) return ESP_ERR_INVALID_STATE;
    agent_stream_opts_t o = opts ? *opts : agent_stream_opts_t{};
    if (!o.total_bytes) o.total_bytes = static_cast<uint32_t>(n);
    agent_stream_handle_t handle = nullptr;
    esp_err_t e = agent_link_stream_open(kind, &o, &handle);
    if (e != ESP_OK) return e;
    e = agent_link_stream_write(handle, data, n);
    const esp_err_t end = agent_link_stream_close(handle, e == ESP_OK);
    if (end==ESP_ERR_NO_MEM || end==ESP_ERR_TIMEOUT) {
        s_pending_stream_end[kind]=true;
        s_pending_stream_complete[kind]=e==ESP_OK;
        // The payload and deferred END are accepted; do not prompt the caller
        // to resend already-accepted bytes just because END is congested.
        return e;
    }
    return e == ESP_OK ? end : e;
}
