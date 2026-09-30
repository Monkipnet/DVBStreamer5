#pragma once

#include "media/NativeMpegTsMux.h"
#include "media/TransportStream.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dvbstreamer5::media::mpegts {

struct DemuxStreamInfo {
    std::uint16_t pid = 0;
    ElementaryKind kind = ElementaryKind::Video;
    ElementaryCodec codec = ElementaryCodec::Unknown;
    std::uint8_t streamType = 0;
};

struct DemuxSample {
    DemuxStreamInfo stream;
    std::vector<std::uint8_t> data;
    std::uint64_t pts90k = 0;
    std::uint64_t dts90k = 0;
    bool hasPts = false;
    bool hasDts = false;
    bool randomAccess = false;
};

class NativeTsDemux {
public:
    using SampleCallback = std::function<void(DemuxSample&&)>;
    using ProgramCallback = std::function<void(const std::vector<DemuxStreamInfo>&)>;

    NativeTsDemux() = default;

    void setSampleCallback(SampleCallback callback);
    void setProgramCallback(ProgramCallback callback);
    void reset();
    bool push(const std::uint8_t* data, std::size_t size, std::string& error);
    void flush();

    std::vector<DemuxStreamInfo> streams() const;
    std::uint16_t programNumber() const noexcept { return programNumber_; }
    std::uint16_t pmtPid() const noexcept { return pmtPid_; }

private:
    struct SectionAssembler {
        std::vector<std::uint8_t> bytes;
        std::size_t expected = 0;
    };
    struct PesAssembler {
        DemuxStreamInfo stream;
        std::vector<std::uint8_t> bytes;
        bool randomAccessHint = false;
    };

    void consumePacket(const Packet& packet, std::string& error);
    void consumePsi(std::uint16_t pid, const PacketInfo& info, const std::uint8_t* payload, std::size_t size);
    void consumeSection(std::uint16_t pid, const std::vector<std::uint8_t>& section);
    void parsePat(const std::vector<std::uint8_t>& section);
    void parsePmt(const std::vector<std::uint8_t>& section);
    void consumePes(const PacketInfo& info, const std::uint8_t* payload, std::size_t size);
    void flushPes(std::uint16_t pid);
    void notifyProgram();

    static ElementaryCodec codecFromStreamType(std::uint8_t streamType,
                                                const std::uint8_t* descriptors,
                                                std::size_t descriptorSize);
    static ElementaryKind kindFromCodec(ElementaryCodec codec);
    static bool parsePes(const PesAssembler& pes, DemuxSample& sample);
    static bool containsRandomAccessNal(ElementaryCodec codec, const std::uint8_t* data, std::size_t size);
    static std::uint64_t readPts(const std::uint8_t* data) noexcept;

    mutable std::mutex mutex_;
    PacketFramer framer_;
    std::map<std::uint16_t, SectionAssembler> psi_;
    std::map<std::uint16_t, DemuxStreamInfo> streamsByPid_;
    std::map<std::uint16_t, PesAssembler> pes_;
    SampleCallback sampleCallback_;
    ProgramCallback programCallback_;
    std::uint16_t programNumber_ = 0;
    std::uint16_t pmtPid_ = 0xffff;
};

} // namespace dvbstreamer5::media::mpegts
