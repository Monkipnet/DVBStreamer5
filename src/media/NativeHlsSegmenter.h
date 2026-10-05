#pragma once

#include "media/TransportStream.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace dvbstreamer5::media::hls {

struct NativeHlsSegmenterConfig {
    std::filesystem::path directory;
    double targetDurationSeconds = 2.0;
    std::size_t liveWindowSegments = 6;
    bool archiveEnabled = false;
    std::uint32_t archiveHours = 24;
    // For transcoded ABR renditions every published segment must begin at a
    // random-access point so a player can switch bitrate without freezing.
    bool independentSegments = false;
    std::string encryption = "none"; // none | aes-128 | sample-aes
    std::string keyUri = "key.bin";
    std::array<std::uint8_t, 16> key{};
    bool hasKey = false;
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
    void observePsi(const mpegts::Packet& packet, const mpegts::PacketInfo& info);
    bool writePsiPrefix();
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
    // Keep one previous live window on disk after it leaves the playlist.
    // HLS clients can legally request segments from a slightly stale playlist;
    // deleting an evicted segment immediately turns that race into a 404/stall.
    std::deque<SegmentInfo> retiredSegments_;
    // MPEG-TS HLS clients may open every segment with a fresh demuxer.
    // Keep the latest complete PAT/PMT repetition and prepend it to each
    // subsequent segment so audio/video PIDs are known before media bytes.
    std::vector<mpegts::Packet> patCollecting_;
    std::vector<mpegts::Packet> patPrefix_;
    std::vector<mpegts::Packet> pmtCollecting_;
    std::vector<mpegts::Packet> pmtPrefix_;
    std::uint16_t pmtPid_ = mpegts::kNullPid;
    // V10.8.85: remember PMT stream_type per elementary PID. The clean-start
    // gate must apply MPEG-2 sequence-header rules only to MPEG-1/2 video, not
    // to AVC/HEVC PES that use the same 0xE0..0xEF PES stream_id range.
    std::array<std::uint8_t, 8192> elementaryStreamType_{};
    std::uint64_t nextSequence_ = 0;
    std::uint64_t completedSegments_ = 0;
    bool running_ = false;
    bool segmentHasPackets_ = false;
    bool waitingForIndependentStart_ = false;
    // V10.8.79: passthrough/live MPEG-TS must not publish a first segment that
    // starts in the middle of video/audio PES. Wait for complete PSI plus a
    // PCR-bearing PES boundary, then admit each PID only from its first PUSI.
    bool waitingForCleanStart_ = false;
    std::array<bool, 8192> firstSegmentPidStarted_{};
    // V10.8.94: once the target duration is reached, hold the beginning of a
    // passthrough AVC/HEVC PES until its first VCL NAL is known. Broadcast
    // encoders frequently place the IDR/IRAP NAL in the second or later TS
    // packet and omit random_access_indicator, so a one-packet probe cannot
    // choose a decoder-safe segment boundary.
    bool h26xCutArmed_ = false;
    bool h26xBoundaryPending_ = false;
    std::uint16_t h26xBoundaryPid_ = mpegts::kNullPid;
    std::uint8_t h26xBoundaryStreamType_ = 0;
    double h26xBoundaryDuration_ = 0.0;
    std::uint64_t h26xBoundaryPcr_ = 0;
    std::vector<mpegts::Packet> h26xBoundaryPackets_;
    std::vector<std::uint8_t> h26xBoundaryElementary_;
    // V10.8.95: cache only Annex-B decoder configuration NAL units from the
    // source (SPS/PPS for AVC, VPS/SPS/PPS for HEVC). Some broadcast encoders
    // do not repeat these before later IDR/IRAP frames, so fresh HLS segments
    // otherwise expose a video PID but no width/height to a new decoder.
    std::uint16_t h26xConfigPid_ = mpegts::kNullPid;
    std::uint8_t h26xConfigStreamType_ = 0;
    std::vector<std::uint8_t> h26xParameterSets_;
    // V10.8.96: collect one complete source video PES at a time before caching
    // decoder configuration. Merely seeing a PPS NAL header in the first TS
    // packet is insufficient because the PPS body may continue in later packets.
    bool h26xConfigProbeActive_ = false;
    std::uint16_t h26xConfigProbePid_ = mpegts::kNullPid;
    std::uint8_t h26xConfigProbeStreamType_ = 0;
    std::vector<std::uint8_t> h26xConfigProbeElementary_;
    bool haveFirstPcr_ = false;
    std::uint64_t firstPcr_ = 0;
    std::uint64_t lastPcr_ = 0;
    std::string lastError_;
    std::chrono::system_clock::time_point programTime_{};
};

} // namespace dvbstreamer5::media::hls
