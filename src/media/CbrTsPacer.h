#pragma once

#include "media/TransportStream.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace dvbstreamer5::media::mpegts {

// WISI Chameleon interoperable MPEG-over-UDP layout: 7 * 188 = 1316 bytes.
inline constexpr std::size_t kPacketsPerCbrDatagram = 7;
using CbrDatagram = std::array<Packet, kPacketsPerCbrDatagram>;

// Fixed-rate WISI transport shaper. It deliberately does not estimate or tune
// the requested transport bitrate. Source NULL packets are discarded and the
// shaper regenerates exactly the NULL budget required by the configured CBR.
// PCR/PTS/DTS bytes in useful source packets are preserved byte-for-byte.
class CbrTsPacer {
public:
    explicit CbrTsPacer(std::uint64_t targetBitrate);

    bool canEnqueue(const Packet& packet) const noexcept;
    bool enqueue(const Packet& packet);
    bool nextDatagram(
        std::chrono::steady_clock::time_point now,
        CbrDatagram& datagram);

    bool started() const noexcept;
    std::chrono::steady_clock::time_point nextDeadline() const noexcept;
    std::uint64_t targetBitrate() const noexcept;
    std::size_t queuedPackets() const noexcept;
    std::size_t queuedBytes() const noexcept;
    static constexpr std::size_t maximumQueuedBytes() noexcept {
        return kMaximumQueuedBytes;
    }

private:
    void advanceDeadline() noexcept;
    void makeNullPacket(Packet& packet) noexcept;
    static bool isNullPacket(const Packet& packet) noexcept;

    // Large enough for ordinary HTTP/SRT/HLS delivery bursts without turning
    // the shaper into a multi-second SAT5-style playout reservoir.
    static constexpr std::size_t kMaximumQueuedBytes = 8 * 1024 * 1024;
    static constexpr std::uint64_t kMinimumBitrate = 100000;
    static constexpr std::uint64_t kMaximumBitrate = 200000000;

    std::uint64_t targetBitrate_;
    std::deque<Packet> queuedPackets_;
    std::chrono::steady_clock::time_point nextDeadline_ {};
    std::uint64_t pacingRemainder_ = 0;
    std::uint8_t nullContinuityCounter_ = 0;
    bool started_ = false;
};

} // namespace dvbstreamer5::media::mpegts
