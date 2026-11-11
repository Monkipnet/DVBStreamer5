// V10.8.475: BISS-1 static-key, opt-in local MPEG-TS CSA backend.
// No network client, ECM, EMM, key search or key export.
#include "../../CaBackendPluginApi.h"
#include "Biss1Key.h"
extern "C" {
#include <dvbcsa/dvbcsa.h>
}
#include <jsoncpp/json/json.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {
using namespace dvbstreamer5::ca::biss;
constexpr size_t kTs = 188;

void writeError(char* output, size_t size, const char* message) {
    if (output && size) std::snprintf(output, size, "%s", message);
}

struct Reader {
    dvbcsa_key_t* even = nullptr;
    dvbcsa_key_t* odd = nullptr;
    ~Reader() {
        if (even) dvbcsa_key_free(even);
        if (odd) dvbcsa_key_free(odd);
    }
    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
};

struct Service {
    std::shared_ptr<const Reader> reader;
    uint16_t sid = 0;
    std::atomic<uint64_t> packets{0}, changed{0}, malformed{0};
};
struct State {
    std::mutex mutex;
    std::map<std::string,std::shared_ptr<const Reader>> readers;
    std::map<std::string,std::shared_ptr<Service>> services;
    std::string snapshot = "{}";
};

void* create(const dvbstreamer5_ca_host_api_v1*) { return new State; }
void destroy(void* p) { delete static_cast<State*>(p); }

int openReader(void* p, const dvbstreamer5_ca_reader_info_v1* info,
               char* error, size_t errorSize) {
    if (!p || !info || !info->reader_key || !*info->reader_key) {
        writeError(error, errorSize, "invalid BISS-1 reader"); return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    Json::Value json;
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::unique_ptr<Json::CharReader> parser(builder.newCharReader());
    const char* raw = info->backend_config_json;
    std::string parseError;
    if (!raw || !parser->parse(raw, raw+std::strlen(raw), &json, &parseError) ||
        !json.isObject() || !json["key"].isString()) {
        writeError(error, errorSize, "BISS-1 backend_config requires a key");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    std::array<uint8_t,8> cw{};
    if (!parseBiss1Key(json["key"].asString(), cw)) {
        writeError(error, errorSize, "BISS-1 key must be 12 or 16 hex digits with valid checksums");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    const std::string mode = json.get("mode", "biss1").asString();
    if (mode != "biss1") {
        cw.fill(0);
        writeError(error, errorSize, "only BISS-1 (Mode 1) is supported");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    auto reader = std::make_shared<Reader>();
    reader->even = dvbcsa_key_alloc();
    reader->odd = dvbcsa_key_alloc();
    if (!reader->even || !reader->odd) {
        cw.fill(0);
        writeError(error, errorSize, "BISS-1 CSA key allocation failed");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    dvbcsa_key_set(cw.data(), reader->even);
    dvbcsa_key_set(cw.data(), reader->odd);
    volatile uint8_t* bytes = cw.data();
    for (size_t i = 0; i < cw.size(); ++i) bytes[i] = 0;

    auto* state = static_cast<State*>(p);
    std::lock_guard<std::mutex> guard(state->mutex);
    state->readers[info->reader_key] = std::move(reader);
    return DVBSTREAMER5_CA_RESULT_OK;
}
void closeReader(void* p, const char* key) {
    if (!p || !key) return;
    auto* state = static_cast<State*>(p);
    std::lock_guard<std::mutex> guard(state->mutex);
    state->readers.erase(key);
}
int startService(void* p, const char* readerKey,
                 const dvbstreamer5_ca_service_info_v1* info,
                 char* error, size_t errorSize) {
    if (!p || !readerKey || !info || !info->stream_id || !*info->stream_id) {
        writeError(error, errorSize, "invalid BISS-1 service");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    auto* state = static_cast<State*>(p);
    std::lock_guard<std::mutex> guard(state->mutex);
    const auto found = state->readers.find(readerKey);
    if (found == state->readers.end()) {
        writeError(error, errorSize, "BISS-1 reader not configured");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    if (state->services.count(info->stream_id)) return DVBSTREAMER5_CA_RESULT_OK;
    auto service = std::make_shared<Service>();
    service->reader = found->second;
    service->sid = static_cast<uint16_t>(info->service_id);
    state->services.emplace(info->stream_id, std::move(service));
    return DVBSTREAMER5_CA_RESULT_OK;
}
void stopService(void* p, const char* stream) {
    if (!p || !stream) return;
    auto* state = static_cast<State*>(p);
    std::lock_guard<std::mutex> guard(state->mutex);
    state->services.erase(stream);
}

int processTs(void* p, const char* stream, uint8_t* data, size_t size,
              dvbstreamer5_ca_ts_result_v1* result) {
    if (!p || !stream || !data || size % kTs) return DVBSTREAMER5_CA_RESULT_ERROR;
    if (result) {
        std::memset(result, 0, sizeof(*result));
        result->packets_seen = size / kTs;
        std::snprintf(result->status, sizeof(result->status), "BISS1_ACTIVE");
    }
    auto* state = static_cast<State*>(p);
    std::shared_ptr<Service> service;
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        const auto found = state->services.find(stream);
        if (found == state->services.end()) return DVBSTREAMER5_CA_RESULT_PASSTHROUGH;
        service = found->second;
    }
    size_t changed = 0, clear = 0, scrambled = 0, malformed = 0;
    for (size_t offset=0; offset<size; offset+=kTs) {
        uint8_t* packet = data+offset;
        if (packet[0] != 0x47) { ++malformed; continue; }
        const uint8_t scrambling = static_cast<uint8_t>((packet[3] >> 6) & 3);
        if (scrambling == 0) { ++clear; continue; }
        ++scrambled;
        if (scrambling == 1) { ++malformed; continue; } // DVB reserved
        const size_t payload = tsPayloadOffset(packet);
        if (payload >= kTs || kTs - payload < 8) {
            // Scalar DVB-CSA deliberately does not decrypt fragments <8 B.
            // Do not falsely clear the scrambling flag for such packets.
            ++malformed; continue;
        }
        const dvbcsa_key_t* key = scrambling == 2 ?
            service->reader->even : service->reader->odd;
        dvbcsa_decrypt(key, packet + payload, static_cast<unsigned int>(kTs-payload));
        packet[3] &= 0x3f; // clear scrambling bits only after successful processing
        ++changed;
    }
    service->packets.fetch_add(size/kTs, std::memory_order_relaxed);
    service->changed.fetch_add(changed, std::memory_order_relaxed);
    service->malformed.fetch_add(malformed, std::memory_order_relaxed);
    if (result) {
        result->packets_changed = changed;
        result->packets_clear = clear;
        result->packets_scrambled = scrambled;
        if (malformed) std::snprintf(result->status, sizeof(result->status),
                                     "BISS1_PARTIAL_INVALID_TS");
    }
    return DVBSTREAMER5_CA_RESULT_OK;
}
const char* statusJson(void* p) {
    static thread_local std::string snapshot;
    if (!p) { snapshot = "{}"; return snapshot.c_str(); }
    auto* state = static_cast<State*>(p);
    std::lock_guard<std::mutex> guard(state->mutex);
    uint64_t packets=0, changed=0, malformed=0;
    for (const auto& entry : state->services) {
        packets += entry.second->packets.load(std::memory_order_relaxed);
        changed += entry.second->changed.load(std::memory_order_relaxed);
        malformed += entry.second->malformed.load(std::memory_order_relaxed);
    }
    snapshot = std::string("{\"backend\":\"biss1\",\"mode\":\"BISS-1\",\"readers\":") +
        std::to_string(state->readers.size()) +
        ",\"services\":" + std::to_string(state->services.size()) +
        ",\"packets\":" + std::to_string(packets) +
        ",\"descrambled_packets\":" + std::to_string(changed) +
        ",\"invalid_ts_packets\":" + std::to_string(malformed) +
        ",\"key_material_exposed\":false}";
    return snapshot.c_str();
}
const dvbstreamer5_ca_backend_api_v1 api {
    DVBSTREAMER5_CA_BACKEND_ABI_V1,
    "biss1", "BISS-1 (fixed CW)", "DVBStreamer5",
    DVBSTREAMER5_CA_CAP_TS_INPLACE | DVBSTREAMER5_CA_CAP_MULTI_SERVICE,
    &create, &destroy, &openReader, &closeReader,
    &startService, &stopService, &processTs, &statusJson
};
} // namespace

extern "C" __attribute__((visibility("default")))
const dvbstreamer5_ca_backend_api_v1* dvbstreamer5_ca_backend_get_api_v1() {
    return &api;
}
