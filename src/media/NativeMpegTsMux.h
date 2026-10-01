#pragma once

#include "media/TransportStream.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvbstreamer5::media::mpegts {

enum class ElementaryCodec {
    Unknown,
    H264,
    H265,
    Mpeg2Video,
    AacAdts,
    AacLatm,
    MpegAudio,
    Ac3,
    Eac3
};

enum class ElementaryKind {
    Video,
    Audio
};

struct NativeMuxConfig {
    std::uint16_t serviceId = 1;
    std::uint16_t transportStreamId = 1;
    std::uint16_t originalNetworkId = 1;
    std::uint16_t pmtPid = 0x1000;
    std::uint16_t videoPid = 0x0100;
    std::uint16_t audioPid = 0x0101;
    std::uint64_t targetBitrate = 0;
    std::string serviceName = "DVBStreamer5";
    std::string serviceProvider = "DVBStreamer5";
};

struct ElementarySample {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint64_t pts90k = 0;
    std::uint64_t dts90k = 0;
    std::uint64_t duration90k = 0;
    bool hasPts = false;
    bool hasDts = false;
    bool randomAccess = false;
};

class NativeMpegTsMux {
public:
    bool initialize(const NativeMuxConfig& config, std::string& error);
    bool setCodec(ElementaryKind kind, ElementaryCodec codec, std::string& error);
    bool write(
        ElementaryKind kind,
        const ElementarySample& sample,
        std::vector<Packet>& output,
        std::string& error);
    void reset() noexcept;

    const NativeMuxConfig& config() const noexcept { return config_; }
    ElementaryCodec videoCodec() const noexcept { return videoCodec_; }
    ElementaryCodec audioCodec() const noexcept { return audioCodec_; }

private:
    static bool validElementaryPid(std::uint16_t pid) noexcept;
    static std::uint8_t streamType(ElementaryCodec codec) noexcept;
    static std::uint8_t pesStreamId(ElementaryKind kind) noexcept;
    static void appendCrc(std::vector<std::uint8_t>& section);
    static void appendPts(std::vector<std::uint8_t>& data, std::uint8_t prefix, std::uint64_t value90k);

    Packet makeSectionPacket(std::uint16_t pid, std::uint8_t& continuity,
                             const std::vector<std::uint8_t>& section);
    Packet makeNullPacket();
    std::vector<std::uint8_t> makePatSection() const;
    std::vector<std::uint8_t> makePmtSection() const;
    std::vector<std::uint8_t> makeSdtSection() const;
    void emitPsi(std::uint64_t clock90k, bool force, std::vector<Packet>& output);
    void padToClock(std::uint64_t clock90k, std::vector<Packet>& output);
    void packetizePes(
        ElementaryKind kind,
        std::uint16_t pid,
        const ElementarySample& sample,
        std::vector<Packet>& output);
    std::uint64_t sampleClock(ElementaryKind kind, const ElementarySample& sample);
    void markCodecChange() noexcept;

    NativeMuxConfig config_;
    ElementaryCodec videoCodec_ = ElementaryCodec::Unknown;
    ElementaryCodec audioCodec_ = ElementaryCodec::Unknown;
    std::array<std::uint8_t, 8192> continuity_ {};
    std::uint8_t patContinuity_ = 0;
    std::uint8_t pmtContinuity_ = 0;
    std::uint8_t sdtContinuity_ = 0;
    std::uint8_t nullContinuity_ = 0;
    std::uint8_t pmtVersion_ = 0;
    std::uint64_t lastPsi90k_ = 0;
    std::uint64_t lastSdt90k_ = 0;
    std::uint64_t lastVideoPts90k_ = 0;
    std::uint64_t lastAudioPts90k_ = 0;
    std::uint64_t clockBase90k_ = 0;
    std::uint64_t emittedPackets_ = 0;
    bool initialized_ = false;
    bool psiEmitted_ = false;
    bool clockStarted_ = false;
};

} // namespace dvbstreamer5::media::mpegts
