#pragma once

#include "media/TransportStream.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace dvbstreamer5::media::hls {

struct NativeHlsSegmenterConfig {
    std::filesystem::path directory;
    double targetDurationSeconds = 2.0;
    std::size_t liveWindowSegments = 6;
    bool archiveEnabled = false;
    std::uint32_t archiveHours = 24;
};

class NativeHlsSegmenter {
public:
    NativeHlsSegmenter() = default;
    ~NativeHlsSegmenter();

    NativeHlsSegmenter(const NativeHlsSegmenter&) = delete;
    NativeHlsSegmenter& operator=(const NativeHlsSegmenter&) = delete;

    bool start(const NativeHlsSegmenterConfig& config, std::string& error);
    bool push(const std::uint8_t* data, std::size_t size);
    void stop() noexcept;

    bool isRunning() const noexcept;
    std::string lastError() const;
    std::uint64_t segmentCount() const noexcept;

private:
    struct SegmentInfo {
        std::uint64_t sequence = 0;
        std::string fileName;
        double duration = 0.0;
        std::chrono::system_clock::time_point wallTime;
    };

    bool appendPacket(const mpegts::Packet& packet);
    bool rotate(double durationSeconds);
    bool openSegment();
    bool writePlaylist(bool endList = false);
    void prune();
    void fail(const std::string& error);
    static double pcrDeltaSeconds(std::uint64_t first, std::uint64_t last);

    mutable std::mutex mutex_;
    NativeHlsSegmenterConfig config_;
    mpegts::PacketFramer framer_;
    std::ofstream segment_;
    std::filesystem::path segmentPath_;
    std::deque<SegmentInfo> liveSegments_;
    std::uint64_t nextSequence_ = 0;
    std::uint64_t completedSegments_ = 0;
    bool running_ = false;
    bool segmentHasPackets_ = false;
    bool haveFirstPcr_ = false;
    std::uint64_t firstPcr_ = 0;
    std::uint64_t lastPcr_ = 0;
    std::string lastError_;
    std::chrono::system_clock::time_point programTime_{};
};

} // namespace dvbstreamer5::media::hls
