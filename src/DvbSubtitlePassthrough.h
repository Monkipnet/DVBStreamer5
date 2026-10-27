#pragma once

#include "media/TransportStream.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Optional DVB bitmap subtitles passthrough for native video/audio transcoding.
//
// Observe the post-remap/post-CA input, preserve original subtitle PES packets
// and the PMT subtitling_descriptor (0x59), and splice them into the native
// transcoder's MPEG-TS with the corresponding PMT entry.
//
// All methods run on NativeUdpRelay's single worker thread. No tuner changes,
// subtitle rendering, media decoding or additional worker threads are needed.
class DvbSubtitlePassthrough {
public:
    using Packet = dvbstreamer5::media::mpegts::Packet;
    explicit DvbSubtitlePassthrough(std::string language = "auto");
    void observe(const std::uint8_t* data, std::size_t size);
    void inject(std::vector<Packet>& output);
private:
    struct Track {
        std::uint16_t pid = 0;
        std::vector<std::uint8_t> descriptors;
    };
    struct PsiState {
        std::vector<std::uint8_t> bytes;
        std::size_t expected = 0;
        std::uint8_t lastContinuity = 0;
        bool continuityValid = false;
    };
    void readPat(const Packet& packet);
    void readPmt(const Packet& packet);
    void acceptPmtSection(const std::vector<std::uint8_t>& section);
    void rewriteOutputPmt(Packet& packet);
    static std::uint32_t crc32(const std::uint8_t* bytes, std::size_t size);
    static std::size_t payloadOffset(const Packet& packet);
    static std::uint16_t packetPid(const Packet& packet);
    std::uint16_t sourcePmtPid_ = 0x1fff;
    PsiState sourcePmt_;
    std::vector<Track> sourceTracks_;
    std::array<bool, 8192> selectedPids_{};
    std::array<std::uint16_t, 8192> outputPids_{};
    std::array<std::uint8_t, 8192> continuity_{};
    std::array<bool, 8192> awaitStart_{};
    std::deque<Packet> queued_;
    bool warnedPmtCapacity_ = false;
    bool sourcePmtParsed_ = false;
    std::string language_;
};
