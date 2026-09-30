#pragma once

#include "ConfigManager.h"

#include <string>
#include <vector>

#include <sys/types.h>

// Legacy compatibility shim.
//
// Stage 1 retires the standalone gst-launch transcoder process. StreamManager
// still references this type while the remaining in-process GStreamer media
// path is migrated to the native media core in later stages. isAvailable()
// intentionally returns false so new streams use TranscoderModule::createBin().
class GstTranscoderProcess {
public:
    GstTranscoderProcess() = default;
    ~GstTranscoderProcess() = default;

    GstTranscoderProcess(const GstTranscoderProcess&) = delete;
    GstTranscoderProcess& operator=(const GstTranscoderProcess&) = delete;

    static bool isAvailable(std::string* error = nullptr);

    bool start(const StreamConfig& config, std::string& error);
    void stop();
    bool isRunning();
    std::string description() const;
    std::vector<pid_t> childPids() const;
};
