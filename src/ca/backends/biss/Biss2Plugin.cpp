// V10.8.482: opt-in BISS2 Mode 1/E; DVB-CISSA AES-128-CBC.
// BISS2-CA requires EMM/ECM, RSA receiver entitlements and is NOT supported.
// Separate from BISS-1, Newcamd, TSDecrypt and OSCam-mini.
#include "../../CaBackendPluginApi.h"
#include "Biss2Key.h"
#include <jsoncpp/json/json.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
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
using dvbstreamer5::ca::biss2::parseHex128;
using dvbstreamer5::ca::biss2::nonzeroKey;
using dvbstreamer5::ca::biss2::decryptEsw;
using dvbstreamer5::ca::biss2::decryptTsPayload;

void fail(char* error, size_t length, const char* description) {
    if (error && length) std::snprintf(error, length, "%s", description);
}

struct Reader {
    std::array<uint8_t,16> sessionWord{};
    bool encryptedMode = false;
    ~Reader() { OPENSSL_cleanse(sessionWord.data(), sessionWord.size()); }
};

struct Service {
    std::shared_ptr<const Reader> reader;
    uint16_t sid = 0;
    std::atomic<uint64_t> packets{0}, changed{0}, malformed{0};
};

struct Backend {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const Reader>> readers;
    std::map<std::string, std::shared_ptr<Service>> services;
};

void* create(const dvbstreamer5_ca_host_api_v1*) { return new Backend; }
void destroy(void* p) { delete static_cast<Backend*>(p); }

int openReader(void* p, const dvbstreamer5_ca_reader_info_v1* info,
               char* error, size_t errorSize) {
    if (!p || !info || !info->reader_key || !*info->reader_key ||
        !info->backend_config_json) {
        fail(error, errorSize, "invalid BISS2 reader configuration");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    const char* raw = info->backend_config_json;
    Json::Value cfg;
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::unique_ptr<Json::CharReader> parser(builder.newCharReader());
    std::string parseError;
    if (!parser->parse(raw, raw + std::strlen(raw), &cfg, &parseError) ||
        !cfg.isObject() || !cfg["mode"].isString()) {
        fail(error, errorSize, "BISS2 requires mode biss2-1 or biss2-e");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    const std::string mode = cfg["mode"].asString();
    if (mode != "biss2-1" && mode != "biss2-e") {
        fail(error, errorSize, "supported BISS2 modes: 1 and E; BISS-CA unavailable");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }

    auto reader = std::make_shared<Reader>();
    if (mode == "biss2-1") {
        if (!cfg["key"].isString() ||
            !parseHex128(cfg["key"].asString(), reader->sessionWord) ||
            !nonzeroKey(reader->sessionWord)) {
            fail(error, errorSize, "BISS2 Mode 1 requires 32 hexadecimal SW digits");
            return DVBSTREAMER5_CA_RESULT_ERROR;
        }
    } else {
        reader->encryptedMode = true;
        std::array<uint8_t,16> esw{}, receiverId{};
        const bool valid = cfg["esw"].isString() &&
            cfg["injected_id"].isString() &&
            parseHex128(cfg["esw"].asString(), esw) &&
            parseHex128(cfg["injected_id"].asString(), receiverId) &&
            decryptEsw(esw, receiverId, reader->sessionWord) &&
            nonzeroKey(reader->sessionWord);
        OPENSSL_cleanse(esw.data(), esw.size());
        OPENSSL_cleanse(receiverId.data(), receiverId.size());
        if (!valid) {
            fail(error, errorSize,
                 "BISS2 Mode E requires 32-hex ESW and 32-hex receiver ID");
            return DVBSTREAMER5_CA_RESULT_ERROR;
        }
    }
    auto* backend = static_cast<Backend*>(p);
    std::lock_guard<std::mutex> lock(backend->mutex);
    backend->readers[info->reader_key] = std::move(reader);
    return DVBSTREAMER5_CA_RESULT_OK;
}

void closeReader(void* p, const char* readerKey) {
    if (!p || !readerKey) return;
    auto* backend = static_cast<Backend*>(p);
    std::lock_guard<std::mutex> lock(backend->mutex);
    backend->readers.erase(readerKey);
}

int startService(void* p, const char* readerKey,
                 const dvbstreamer5_ca_service_info_v1* info,
                 char* error, size_t errorSize) {
    if (!p || !readerKey || !info || !info->stream_id || !*info->stream_id) {
        fail(error, errorSize, "invalid BISS2 service");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    auto* backend = static_cast<Backend*>(p);
    std::lock_guard<std::mutex> lock(backend->mutex);
    const auto found = backend->readers.find(readerKey);
    if (found == backend->readers.end()) {
        fail(error, errorSize, "BISS2 reader not configured");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    if (backend->services.count(info->stream_id))
        return DVBSTREAMER5_CA_RESULT_OK;
    auto service = std::make_shared<Service>();
    service->reader = found->second;
    service->sid = static_cast<uint16_t>(info->service_id);
    backend->services.emplace(info->stream_id, std::move(service));
    return DVBSTREAMER5_CA_RESULT_OK;
}

void stopService(void* p, const char* streamId) {
    if (!p || !streamId) return;
    auto* backend = static_cast<Backend*>(p);
    std::lock_guard<std::mutex> lock(backend->mutex);
    backend->services.erase(streamId);
}

int processTs(void* p, const char* streamId, uint8_t* data, size_t bytes,
              dvbstreamer5_ca_ts_result_v1* result) {
    if (!p || !streamId || !data || bytes % 188)
        return DVBSTREAMER5_CA_RESULT_ERROR;
    if (result) {
        std::memset(result, 0, sizeof(*result));
        result->packets_seen = bytes / 188;
    }
    auto* backend = static_cast<Backend*>(p);
    std::shared_ptr<Service> service;
    {
        std::lock_guard<std::mutex> lock(backend->mutex);
        const auto it = backend->services.find(streamId);
        if (it == backend->services.end())
            return DVBSTREAMER5_CA_RESULT_PASSTHROUGH;
        service = it->second;
    }
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        fail(result ? result->status : nullptr,
             result ? sizeof(result->status) : 0, "BISS2_CIPHER_UNAVAILABLE");
        return DVBSTREAMER5_CA_RESULT_ERROR;
    }
    uint64_t changed = 0, clear = 0, scrambled = 0, invalid = 0;
    for (size_t i = 0; i < bytes; i += 188) {
        uint8_t* packet = data + i;
        if (packet[0] != 0x47 || (packet[1] & 0x80)) {
            ++invalid; continue;
        }
        const unsigned parity = (packet[3] >> 6) & 3;
        if (parity == 0) { ++clear; continue; }
        ++scrambled;
        if (parity == 1 ||
            !decryptTsPayload(ctx, service->reader->sessionWord, packet)) {
            ++invalid; continue;
        }
        ++changed;
    }
    EVP_CIPHER_CTX_free(ctx);
    service->packets.fetch_add(bytes/188, std::memory_order_relaxed);
    service->changed.fetch_add(changed, std::memory_order_relaxed);
    service->malformed.fetch_add(invalid, std::memory_order_relaxed);
    if (result) {
        result->packets_changed = changed;
        result->packets_clear = clear;
        result->packets_scrambled = scrambled;
        std::snprintf(result->status, sizeof(result->status),
            "%s", invalid ? "BISS2_PARTIAL_INVALID_TS" :
            service->reader->encryptedMode ? "BISS2_E_ACTIVE" : "BISS2_MODE1_ACTIVE");
    }
    return DVBSTREAMER5_CA_RESULT_OK;
}

const char* statusJson(void* p) {
    static thread_local std::string snapshot;
    if (!p) { snapshot = "{}"; return snapshot.c_str(); }
    auto* backend = static_cast<Backend*>(p);
    std::lock_guard<std::mutex> lock(backend->mutex);
    uint64_t packets=0, changed=0, malformed=0;
    for (const auto& item : backend->services) {
        packets += item.second->packets.load(std::memory_order_relaxed);
        changed += item.second->changed.load(std::memory_order_relaxed);
        malformed += item.second->malformed.load(std::memory_order_relaxed);
    }
    snapshot = std::string("{\"backend\":\"biss2\",\"cipher\":\"DVB-CISSA\",") +
        "\"mode_support\":\"1,E\",\"biss_ca_supported\":false," +
        "\"readers\":" + std::to_string(backend->readers.size()) +
        ",\"services\":" + std::to_string(backend->services.size()) +
        ",\"packets\":" + std::to_string(packets) +
        ",\"descrambled_packets\":" + std::to_string(changed) +
        ",\"invalid_ts_packets\":" + std::to_string(malformed) +
        ",\"key_material_exposed\":false}";
    return snapshot.c_str();
}

const dvbstreamer5_ca_backend_api_v1 api {
    DVBSTREAMER5_CA_BACKEND_ABI_V1,
    "biss2", "BISS-2 (AES-128, Modes 1/E)", "DVBStreamer5",
    DVBSTREAMER5_CA_CAP_TS_INPLACE | DVBSTREAMER5_CA_CAP_MULTI_SERVICE,
    &create, &destroy, &openReader, &closeReader,
    &startService, &stopService, &processTs, &statusJson
};
} // namespace

extern "C" __attribute__((visibility("default")))
const dvbstreamer5_ca_backend_api_v1* dvbstreamer5_ca_backend_get_api_v1() {
    return &api;
}
