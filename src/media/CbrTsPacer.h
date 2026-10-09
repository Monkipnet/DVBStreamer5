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

// Fixed transport clock for WISI Chameleon.
//
// Source NULL packets are removed. Useful packets are grouped into complete
// source-PCR intervals. Once the closing PCR is known, the complete interval is
// distributed over the exact number of target-CBR TS slots represented by the
// source PCR delta. The PCR packet itself is forced into the first slot of each
// interval, so consecutive source PCRs stay on exact CBR interval boundaries.
// PTS/DTS/PCR bytes in source packets remain byte-for-byte unchanged.
class CbrTsPacer {
public:
    explicit CbrTsPacer(std::uint64_t targetBitrate);

    bool canEnqueue(const Packet& packet) const noexcept;
    bool enqueue(const Packet& packet);
    bool nextDatagram(
        std::chrono::steady_clock::time_point now,
        CbrDatagram& datagram);

    bool started() const noexcept;
    bool timingLocked() const noexcept;
    std::chrono::steady_clock::time_point nextDeadline() const noexcept;
    std::uint64_t targetBitrate() const noexcept;
    std::uint64_t sourcePayloadBitrate() const noexcept;
    std::size_t queuedPackets() const noexcept;
    std::size_t queuedBytes() const noexcept;
    std::size_t readySegments() const noexcept;
    std::uint64_t insufficientTargetSegments() const noexcept;
    static constexpr std::size_t maximumQueuedBytes() noexcept {
        return kMaximumQueuedBytes;
    }

private:
    struct QueuedPacket {
        Packet packet {};
        std::uint64_t sequence = 0;
    };

    struct ReadySegment {
        std::uint64_t firstSequence = 0;
        std::uint64_t lastSequence = 0;
        std::uint64_t realPackets = 0;
        std::uint64_t slots = 0;
        bool startsWithPcr = false;
    };

    void advanceDeadline() noexcept;
    void makeNullPacket(Packet& packet) noexcept;
    void observePcr(const Packet& packet, std::uint64_t sequence) noexcept;
    std::uint64_t countQueuedPackets(
        std::uint64_t firstSequence,
        std::uint64_t lastSequence) const noexcept;
    void queuePrefixSegment(
        std::uint64_t firstSequence,
        std::uint64_t lastSequence) noexcept;
    void queuePcrSegment(
        std::uint64_t firstSequence,
        std::uint64_t lastSequence,
        std::uint64_t deltaTicks) noexcept;
    void queueFallbackSegment(
        std::uint64_t firstSequence,
        std::uint64_t lastSequence) noexcept;
    bool activateNextSegment() noexcept;
    bool emitRealFromActiveSegment(Packet& packet) noexcept;
    static bool isNullPacket(const Packet& packet) noexcept;
    static bool readPcr(
        const Packet& packet,
        std::uint64_t& pcrTicks,
        bool& discontinuity) noexcept;

    static constexpr std::size_t kMaximumQueuedBytes = 8 * 1024 * 1024;
    static constexpr std::uint64_t kMinimumBitrate = 100000;
    static constexpr std::uint64_t kMaximumBitrate = 200000000;

    std::uint64_t targetBitrate_;
    std::deque<QueuedPacket> queuedPackets_;
    std::deque<ReadySegment> readySegments_;
    std::chrono::steady_clock::time_point nextDeadline_ {};
    std::uint64_t pacingRemainder_ = 0;
    std::uint8_t nullContinuityCounter_ = 0;
    bool started_ = false;

    std::uint64_t nextSequence_ = 1;
    bool havePcrPid_ = false;
    std::uint16_t pcrPid_ = 0;
    bool havePreviousPcr_ = false;
    bool pcrTimingLocked_ = false;
    std::uint64_t previousPcrTicks_ = 0;
    std::uint64_t previousPcrSequence_ = 0;
    std::uint64_t sourcePayloadBitrate_ = 0;
    std::uint64_t insufficientTargetSegments_ = 0;

    bool activeSegment_ = false;
    bool activeForceFirstReal_ = false;
    std::uint64_t activeFirstSequence_ = 0;
    std::uint64_t activeLastSequence_ = 0;
    std::uint64_t activeRealTotal_ = 0;
    std::uint64_t activeRealRemaining_ = 0;
    std::uint64_t activeSlotsTotal_ = 0;
    std::uint64_t activeSlotsRemaining_ = 0;
    std::uint64_t activeSpreadRealTotal_ = 0;
    std::uint64_t activeSpreadSlotsTotal_ = 0;
    std::uint64_t activeToken_ = 0;
};

} // namespace dvbstreamer5::media::mpegts
