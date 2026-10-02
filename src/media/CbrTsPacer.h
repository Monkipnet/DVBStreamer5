#pragma once

#include "media/TransportStream.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace dvbstreamer5::media::mpegts {

// WISI Chameleon compatibility: send one 188-byte MPEG-TS packet per
// UDP datagram. RTP keeps its own packetizer and is unaffected.
inline constexpr std::size_t kPacketsPerCbrDatagram = 1;
using CbrDatagram = std::array<Packet, kPacketsPerCbrDatagram>;

class CbrTsPacer {
public:
    explicit CbrTsPacer(std::uint64_t targetBitrate);

    bool enqueue(const Packet& packet);
    bool nextDatagram(
        std::chrono::steady_clock::time_point now,
        CbrDatagram& datagram);

    bool started() const noexcept;
    std::chrono::steady_clock::time_point nextDeadline() const noexcept;
    std::uint64_t targetBitrate() const noexcept;
    std::size_t queuedPackets() const noexcept;

private:
    void advanceDeadline() noexcept;
    void makeNullPacket(Packet& packet) noexcept;

    static constexpr std::size_t kMaximumQueuedBytes = 2 * 1024 * 1024;
    static constexpr std::uint64_t kMinimumBitrate = 512000;
    static constexpr std::uint64_t kMaximumBitrate = 200000000;

    std::uint64_t targetBitrate_;
    std::deque<Packet> queuedPackets_;
    std::chrono::steady_clock::time_point nextDeadline_ {};
    std::uint64_t pacingRemainder_ = 0;
    std::uint8_t nullContinuityCounter_ = 0;
    bool started_ = false;
};

} // namespace dvbstreamer5::media::mpegts
