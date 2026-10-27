#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dvbstreamer5::media::epg {

struct DvbEpgEvent {
    std::uint16_t serviceId = 0;
    std::uint16_t eventId = 0;
    std::int64_t startUtc = 0;
    std::uint32_t durationSeconds = 0;
    std::uint8_t runningStatus = 0;
    bool freeCa = false;
    std::string title;
    std::string description;
};

struct DvbEpgSnapshot {
    std::uint16_t serviceId = 0;
    std::uint64_t sectionsParsed = 0;
    std::vector<DvbEpgEvent> events;
};

class DvbEpgCollector {
public:
    explicit DvbEpgCollector(std::uint16_t serviceId = 0,
                             unsigned scheduleDays = 3);

    void push(const std::uint8_t* data, std::size_t size);
    DvbEpgSnapshot snapshot(std::size_t maximumEvents = 256) const;
    // Called outside the MPEG-TS input hot path. Cache is keyed by stream ID
    // and input identity; corrupt/outdated data is ignored.
    bool restoreFromDisk(const std::string& cacheKey);
    bool persistToDisk(const std::string& cacheKey) const;

private:
    void pushPacket(const std::uint8_t* packet);
    void resetSection() noexcept;
    void appendSectionBytes(const std::uint8_t* data, std::size_t size);
    void consumePayload(const std::uint8_t* data, std::size_t size);
    void parseSection(const std::vector<std::uint8_t>& section);
    void commitEvents(std::vector<DvbEpgEvent>& events);

    std::uint16_t configuredServiceId_ = 0;
    std::uint16_t resolvedServiceId_ = 0;
    unsigned scheduleDays_ = 3;

    bool continuityValid_ = false;
    std::uint8_t continuity_ = 0;
    std::vector<std::uint8_t> sectionBytes_;
    std::size_t sectionExpected_ = 0;

    mutable std::mutex mutex_;
    std::vector<DvbEpgEvent> events_;
    std::uint64_t sectionsParsed_ = 0;
};

} // namespace dvbstreamer5::media::epg
