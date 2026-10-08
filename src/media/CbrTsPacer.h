#pragma once

#include "media/TransportStream.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace dvbstreamer5::media::mpegts {

// WISI Chameleon exposes TS packets/datagram separately from the 188-byte
// MPEG-TS packet size. Seven 188-byte TS packets (1316-byte UDP payload) is
// the interoperable MPEG-over-UDP default; receivers may configure lower.
inline constexpr std::size_t kPacketsPerCbrDatagram = 7;
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
    void observePayloadPacket(std::chrono::steady_clock::time_point now) noexcept;
    void finishPayloadRateWindow(std::chrono::steady_clock::time_point now) noexcept;
    void applyTargetBitrate(std::uint64_t targetBitrate,
                            std::uint64_t measuredPayloadBitrate,
                            const char* direction) noexcept;
    static bool isNullPacket(const Packet& packet) noexcept;
    static std::uint64_t roundUp100K(std::uint64_t bitrate) noexcept;

    static constexpr std::size_t kMaximumQueuedBytes = 2 * 1024 * 1024;
    static constexpr std::uint64_t kMinimumBitrate = 512000;
    static constexpr std::uint64_t kMaximumBitrate = 200000000;

    std::uint64_t configuredTargetBitrate_;
    std::uint64_t targetBitrate_;
    std::deque<Packet> queuedPackets_;
    std::chrono::steady_clock::time_point nextDeadline_ {};
    std::uint64_t pacingRemainder_ = 0;
    std::uint8_t nullContinuityCounter_ = 0;
    bool started_ = false;

    std::chrono::steady_clock::time_point payloadRateWindowStart_ {};
    std::chrono::steady_clock::time_point lastAutoTune_ {};
    std::uint64_t payloadPacketsInWindow_ = 0;
    std::array<std::uint64_t, 3> recentPayloadRates_ {};
    std::size_t recentPayloadRateIndex_ = 0;
    std::size_t recentPayloadRateCount_ = 0;
    bool payloadRateWindowStarted_ = false;
    bool autoTuneApplied_ = false;
};

} // namespace dvbstreamer5::media::mpegts
