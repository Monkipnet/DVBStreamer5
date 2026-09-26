#pragma once

#include <jsoncpp/json/json.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ConfigManager.h"

// MPTS output is intentionally isolated from the normal per-channel output
// branches. StreamManager only mirrors the already-normalized outgoing SPTS
// buffers into this module; the existing UDP/SRT/HTTP/HLS paths are unchanged.
class MptsOutputManager {
public:
    MptsOutputManager();
    ~MptsOutputManager();

    void configure(const std::vector<MptsOutputConfig>& outputs,
                   const std::vector<StreamConfig>& streams);

    bool start(const std::string& id, std::string* error = nullptr);
    bool stop(const std::string& id);
    void stopAll();

    // Called by any transport pipeline after its final SPTS normalization.
    // This method performs only a bounded buffer copy and never blocks on
    // network I/O or PSI work.
    void pushBytes(const std::string& streamId,
                   const std::uint8_t* data,
                   std::size_t size);

    Json::Value snapshot() const;

private:
    struct Runtime;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Runtime>> outputs_;
    std::unordered_map<std::string, std::vector<std::weak_ptr<Runtime>>> targets_;
};
