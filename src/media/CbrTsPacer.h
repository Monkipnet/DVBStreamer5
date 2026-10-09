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

enum class CbrPacingProfile {
    Standard,
    // V10.8.158: port of TVStreammerSAT5 StableUdpOutput's continuous
    // SRT/HTTP CBR strategy: five-second cold reservoir, useful-packet token
    // pacing, slow reservoir correction, NULL stuffing and a 20 ms PCR clock.
    TvStreammerSat5Network
};

class CbrTsPacer {
public:
    explicit CbrTsPacer(
        std::uint64_t targetBitrate,
        CbrPacingProfile profile = CbrPacingProfile::Standard);

    bool enqueue(const Packet& packet);
    bool nextDatagram(
        std::chrono::steady_clock::time_point now,
        CbrDatagram& datagram);

    bool started() const noexcept;
    std::chrono::steady_clock::time_point nextDeadline() const noexcept;
    std::uint64_t targetBitrate() const noexcept;
    std::size_t queuedPackets() const noexcept;
    bool canEnqueue(const Packet& packet) const noexcept;
    // Dedicated SAT5 sender threads poll this during the five-second cold
    // reservoir so startup is not dependent on another producer enqueue.
    void pollStart(std::chrono::steady_clock::time_point now) noexcept;
    CbrPacingProfile profile() const noexcept { return profile_; }

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

    bool tvStreammerSat5Profile() const noexcept;
    std::size_t maximumQueuedBytes() const noexcept;
    void observeTvStreammerSat5Arrival(
        const Packet& packet,
        std::chrono::steady_clock::time_point now) noexcept;
    void maybeStartTvStreammerSat5(
        std::chrono::steady_clock::time_point now) noexcept;
    void updateTvStreammerSat5Controller(
        std::chrono::steady_clock::time_point now) noexcept;
    void fillTvStreammerSat5Datagram(
        std::chrono::steady_clock::time_point datagramTime,
        CbrDatagram& datagram) noexcept;
    void processTvStreammerSat5RealPacket(
        Packet& packet,
        std::chrono::steady_clock::time_point slotTime) noexcept;
    void makePeriodicPcrPacket(
        Packet& packet,
        std::chrono::steady_clock::time_point slotTime) noexcept;
    std::uint64_t pcrTicksAt(
        std::chrono::steady_clock::time_point slotTime) const noexcept;
    static void writePcr(Packet& packet, std::uint64_t pcrTicks) noexcept;

    static constexpr std::size_t kMaximumQueuedBytes = 2 * 1024 * 1024;
    static constexpr std::size_t kTvStreammerSat5MaximumQueuedBytes = 32 * 1024 * 1024;
    static constexpr std::uint64_t kMinimumBitrate = 512000;
    static constexpr std::uint64_t kMaximumBitrate = 200000000;

    CbrPacingProfile profile_ = CbrPacingProfile::Standard;
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

    // TVStreammerSAT5 network-profile state. Incoming real packets remain in a
    // jitter reservoir; the token controller spaces them over the configured
    // transport slots instead of draining burst arrivals back-to-back.
    std::chrono::steady_clock::time_point tvSatFirstPacketTime_ {};
    std::chrono::steady_clock::time_point tvSatArrivalWindowStart_ {};
    std::chrono::steady_clock::time_point tvSatLastControllerUpdate_ {};
    std::uint64_t tvSatArrivalPacketsInWindow_ = 0;
    std::uint64_t tvSatEstimatedPayloadBitrate_ = 0;
    std::uint64_t tvSatRealPaceBitrate_ = 0;
    std::uint64_t tvSatRealTokenAccumulator_ = 0;
    std::size_t tvSatStartupPcrSamples_ = 0;
    bool tvSatArrivalWindowStarted_ = false;
    bool tvSatStartLogged_ = false;

    bool tvSatPcrInitialized_ = false;
    std::uint16_t tvSatPcrPid_ = kNullPid;
    std::uint64_t tvSatPcrOriginTicks_ = 0;
    std::chrono::steady_clock::time_point tvSatPcrOriginTime_ {};
    std::chrono::steady_clock::time_point tvSatNextPeriodicPcrTime_ {};
    std::uint8_t tvSatPcrPidContinuityCounter_ = 0;
    bool tvSatPcrPidContinuityValid_ = false;
};

} // namespace dvbstreamer5::media::mpegts
