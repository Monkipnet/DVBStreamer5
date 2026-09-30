#pragma once

#include "media/NativeMpegTsMux.h"
#include "media/NativeTsDemux.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <array>

namespace dvbstreamer5::media::cmaf {

struct TrackInfo {
    std::uint32_t id = 0;
    mpegts::ElementaryKind kind = mpegts::ElementaryKind::Video;
    mpegts::ElementaryCodec codec = mpegts::ElementaryCodec::Unknown;
    std::uint32_t timescale = 90000;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 2;
    std::uint32_t sampleRate = 48000;
    std::uint8_t nalLengthSize = 4;
    std::vector<std::uint8_t> codecConfig;
    std::vector<std::uint8_t> parameterSets;
};

class Fmp4ToMpegTs {
public:
    using DataCallback = std::function<bool(const std::uint8_t*, std::size_t)>;
    bool initialize(const std::vector<std::uint8_t>& initSegment, DataCallback output, std::string& error);
    bool pushFragment(const std::vector<std::uint8_t>& fragment, std::string& error);
    const std::vector<TrackInfo>& tracks() const noexcept { return tracks_; }
private:
    std::vector<TrackInfo> tracks_;
    DataCallback output_;
    mpegts::NativeMpegTsMux mux_;
};

struct SegmenterConfig {
    std::filesystem::path directory;
    double targetDurationSeconds = 2.0;
    std::size_t liveWindowSegments = 6;
    bool archiveEnabled = false;
    std::uint32_t archiveHours = 24;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::string encryption = "none"; // none | sample-aes (CMAF cbcs)
    std::string keyUri = "key.bin";
    std::array<std::uint8_t,16> key{};
    bool hasKey = false;
};

bool decryptSampleAesFragment(std::vector<std::uint8_t>& fragment,
                              const std::array<std::uint8_t,16>& key,
                              std::string& error);

class NativeCmafSegmenter {
public:
    NativeCmafSegmenter();
    ~NativeCmafSegmenter();
    bool start(const SegmenterConfig& config, std::string& error);
    bool push(const std::uint8_t* data, std::size_t size);
    void stop() noexcept;
    bool isRunning() const noexcept;
    std::string lastError() const;
    std::uint64_t segmentCount() const noexcept;
private:
    struct Sample {
        mpegts::DemuxStreamInfo stream;
        std::vector<std::uint8_t> data;
        std::uint64_t pts90k=0,dts90k=0;
        bool key=false;
    };
    struct SegmentInfo { std::uint64_t sequence=0;std::string fileName;double duration=0;std::chrono::system_clock::time_point wallTime; };
    void onSample(mpegts::DemuxSample&& sample);
    bool maybeInitialize(std::string& error);
    bool rotate(std::string& error, bool force=false);
    bool writeInit(std::string& error);
    bool writePlaylist(bool endList, std::string& error);
    void prune();
    void fail(const std::string& error);

    mutable std::mutex mutex_;
    SegmenterConfig config_;
    mpegts::NativeTsDemux demux_;
    std::vector<TrackInfo> tracks_;
    std::vector<Sample> pending_;
    std::deque<SegmentInfo> live_;
    std::uint64_t sequence_=0;
    std::uint64_t completed_=0;
    std::uint64_t segmentStartPts_=0;
    bool haveSegmentStart_=false;
    bool initialized_=false;
    bool running_=false;
    std::string lastError_;
    std::chrono::system_clock::time_point programTime_{};
};

} // namespace dvbstreamer5::media::cmaf
