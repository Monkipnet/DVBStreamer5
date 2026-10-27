#pragma once
#include "media/TransportStream.h"
#include <jsoncpp/json/json.h>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

// An opt-in, per-service DVB EIT output adapter for MPEG-TS clients (not CMAF).
// The relay owns it on one worker thread. It never changes video/audio/PCR and
// emits bounded, CRC-valid EIT packets after any SID-remap/transcoding.
class DvbEitOutput {
public:
    using Snapshot = std::function<Json::Value()>;
    explicit DvbEitOutput(Snapshot snapshot);
    void process(std::vector<dvbstreamer5::media::mpegts::Packet>& packets);

private:
    using Packet = dvbstreamer5::media::mpegts::Packet;
    void inspectAndUpdateTables(Packet& packet);
    void buildSchedule();
    void enqueueSection(const std::vector<std::uint8_t>& section,
                        std::deque<Packet>& queue);
    Snapshot snapshot_;
    std::uint16_t serviceId_ = 0;
    std::uint16_t tsid_ = 0;
    std::uint16_t onid_ = 0;
    std::uint8_t continuity_ = 0;
    std::uint8_t version_ = 0;
    std::uint64_t lastFingerprint_ = 0;
    std::deque<Packet> schedule_;
    std::vector<Packet> presentFollowing_;
    std::chrono::steady_clock::time_point nextRefresh_{};
    std::chrono::steady_clock::time_point nextEmission_{};
    std::chrono::steady_clock::time_point nextPresentFollowing_{};
};
