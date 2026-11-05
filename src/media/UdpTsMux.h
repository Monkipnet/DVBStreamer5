#pragma once

#include "media/CbrTsPacer.h" // existing on-disk bitrate/buffer encoding
#include "media/SourcePcrPacketSchedule.h"

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace dvbstreamer5::media::network {

// Pure TS packetization/shaping. No sockets, sleeps, callbacks or averaged
// payload-rate controller. One instance feeds every destination of a mode.
class UdpTsMux {
public:
    using Packet = mpegts::Packet;
    struct Datagram {
        std::array<std::uint8_t, 1316> bytes{};
        std::size_t size = 0;
        std::size_t useful = 0;
    };
    static constexpr std::size_t kMaxBytes = 8U * 1024U * 1024U;
    enum class PushError { None, InvalidPacket, QueueFull };
    explicit UdpTsMux(bool cbr, std::uint64_t encodedBitrate);
    bool push(const std::vector<Packet>& packets, std::uint64_t arrivalNs);
    bool emit(std::uint64_t nowNs, Datagram& datagram, bool finish = false);
    std::size_t queuedPackets() const noexcept { return queued_; }
    bool started() const noexcept { return started_; }
    std::uint64_t bitrate() const noexcept { return bitrate_; }
    std::uint64_t startupNs() const noexcept { return startupNs_; }
    bool sourcePcr() const noexcept { return sourcePcr_; }
    PushError lastPushError() const noexcept { return pushError_; }

private:
    struct Block { std::vector<Packet> packets; std::size_t offset; std::uint64_t arrival; };
    struct PcrAnchor { std::uint64_t ticks, slot; };
    static bool pcr(const Packet&, std::uint64_t&, bool&);
    static unsigned pid(const Packet& p) { return ((p[1]&31U)<<8U)|p[2]; }
    void restamp(Packet&);
    bool due();
    const bool cbr_, sourcePcr_;
    const std::uint64_t bitrate_, startupNs_;
    bool started_ = false;
    std::uint64_t firstArrival_ = 0, slots_ = 0, accepted_ = 0, emitted_ = 0;
    unsigned nullCc_ = 0, referencePid_ = 8192;
    std::size_t queued_ = 0;
    std::deque<Block> blocks_;
    mpegts::SourcePcrPacketSchedule schedule_;
    std::unordered_map<unsigned,PcrAnchor> pcrAnchors_;
    PushError pushError_ = PushError::None;
};

} // namespace dvbstreamer5::media::network
