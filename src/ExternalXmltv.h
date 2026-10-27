#pragma once

#include "ConfigManager.h"

#include <jsoncpp/json/json.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Downloads external XMLTV in a separate low-priority worker. Per-source
// snapshots are replaced only after a completely successful download/parse;
// network errors never clear last-good EPG or interrupt live media.
class ExternalXmltvManager {
public:
    explicit ExternalXmltvManager(const std::vector<XmltvSourceConfig>& sources);
    ~ExternalXmltvManager();
    ExternalXmltvManager(const ExternalXmltvManager&) = delete;
    ExternalXmltvManager& operator=(const ExternalXmltvManager&) = delete;

    void configure(const std::vector<XmltvSourceConfig>& sources);
    bool refresh(const std::string& sourceId); // empty = all
    Json::Value status() const;
    Json::Value channels() const;
    Json::Value snapshot(const std::string& sourceId,
                         const std::string& channelId, unsigned days) const;

private:
    struct Event {
        std::int64_t start = 0;
        std::int64_t stop = 0;
        std::string title;
        std::string description;
    };
    struct Source {
        XmltvSourceConfig settings;
        std::map<std::string, std::string> channels;
        std::map<std::string, std::vector<Event>> programmes;
        std::int64_t lastSuccess = 0;
        std::int64_t lastAttempt = 0;
        int lastSuccessDay = -1;
        std::string error;
        bool queued = false;
        bool downloading = false;
    };
    static bool download(const std::string& url, std::string& payload, std::string& error);
    static bool parse(const std::string& payload, Source& result, std::string& error);
    void run();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::string, Source> sources_;
    std::thread thread_;
    bool stop_ = false;
};
