#pragma once
#include "ConfigManager.h"
#include <jsoncpp/json/json.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Off by default: independent UDP EIT data carousel for WISI Chameleon.
// All EPG is read via existing /api/epg resolution, no tuning/restarts.
class EpgUdpOutputManager {
public:
    using Snapshot = std::function<Json::Value(const EpgUdpServiceConfig&)>;
    explicit EpgUdpOutputManager(Snapshot snapshot);
    ~EpgUdpOutputManager();
    EpgUdpOutputManager(const EpgUdpOutputManager&) = delete;
    EpgUdpOutputManager& operator=(const EpgUdpOutputManager&) = delete;

    void configure(const std::vector<EpgUdpOutputConfig>& outputs);
    Json::Value status() const;
    static bool validDestination(const std::string& ip);
    static bool validInterface(const std::string& ip);

private:
    void run(std::vector<EpgUdpOutputConfig> outputs);
    Snapshot snapshot_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stop_ = false;
    std::vector<EpgUdpOutputConfig> outputs_;
};
