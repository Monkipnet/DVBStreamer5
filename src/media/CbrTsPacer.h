#pragma once

#include "media/TransportStream.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace dvbstreamer5::media::mpegts {

// WISI Chameleon MPEG-TS over UDP layout used by DVBStreamer5:
// seven 188-byte TS packets per UDP datagram = 1316 bytes.
inline constexpr std::size_t kPacketsPerCbrDatagram = 7;
using CbrDatagram = std::array<Packet, kPacketsPerCbrDatagram>;

// WISI-oriented transport remultiplexer.
//
// The physical UDP clock is intentionally owned by WisiCbrOutputWorker. This
// class owns only MPEG-TS shaping: source NULL removal, useful/NULL slot
// selection and PCR handling. Keeping those responsibilities separate prevents
// input delivery jitter and queue locks from moving the UDP transmission clock.
class CbrTsPacer {
public:
    explicit CbrTsPacer(std::uint64_t targetBitrate);

    bool canEnqueue(const Packet& packet) const noexcept;
    bool enqueue(const Packet& packet);
    bool enqueueAt(
        const Packet& packet,
        std::chrono::steady_clock::time_point arrivalTime);

    // Build one complete 7x188 datagram. No sleeping and no deadline decisions
    // are made here; the dedicated WISI sender owns the physical clock.
    bool emitDatagram(
        std::chrono::steady_clock::time_point now,
        CbrDatagram& datagram);

    // Compatibility path used by non-UDP-CBR observed transports. Native WISI
    // UDP-CBR does not use this clock in V172.
    bool nextDatagram(
        std::chrono::steady_clock::time_point now,
        CbrDatagram& datagram);
    void reanchorDeadline(std::chrono::steady_clock::time_point now) noexcept;

    bool started() const noexcept;
    bool timingLocked() const noexcept;
    std::chrono::steady_clock::time_point nextDeadline() const noexcept;
    std::uint64_t targetBitrate() const noexcept;
    std::uint64_t sourcePayloadBitrate() const noexcept;
    std::uint64_t usefulPaceBitrate() const noexcept;
    std::size_t queuedPackets() const noexcept;
    std::size_t queuedBytes() const noexcept;
    std::uint64_t bufferedMediaMilliseconds() const noexcept;
    std::uint64_t sourceOverTargetWindows() const noexcept;
    std::uint64_t pcrRewriteCount() const noexcept;
    std::uint64_t pcrPidCount() const noexcept;
    std::size_t lastDatagramUsefulPackets() const noexcept;
    std::uint64_t outputSlots() const noexcept;
    bool sourcePllMode() const noexcept { return sourcePllMode_; }

    // V176 keeps the per-stream controls backward-compatible with the existing
    // StreamManager wiring. The Web UI stores target bitrate in whole kbps and
    // uses the otherwise-unused sub-kbps remainder as a private carrier.
    //
    //   +777       = legacy V175 source-PCR switch, default 3000-ms reservoir
    //   +700..+729 = fixed-PCR mode, 500..15000-ms reservoir in 500-ms steps
    //   +800..+829 = source-PCR mode, 500..15000-ms reservoir in 500-ms steps
    //
    // The pacer removes every recognised carrier before transport timing, so
    // physical CBR remains the exact whole-kbps value shown in the UI.
    static constexpr std::uint64_t kSourcePllBitrateMarker = 777ULL;
    static constexpr std::uint64_t kLegacyBufferMarkerBase = 700ULL;
    static constexpr std::uint64_t kSourcePllBufferMarkerBase = 800ULL;
    static constexpr std::uint64_t kBufferMarkerCount = 30ULL;
    static constexpr std::uint64_t kStartupBufferStepMs = 500ULL;
    static constexpr std::uint64_t kMinimumStartupBufferMs = 500ULL;
    static constexpr std::uint64_t kMaximumStartupBufferMs = 15000ULL;
    static constexpr std::uint64_t kDefaultStartupBufferMs = 3000ULL;

    static constexpr bool startupBufferEncodedTargetBitrate(
        std::uint64_t value) noexcept {
        if (value < 100000ULL + kLegacyBufferMarkerBase) return false;
        const std::uint64_t remainder = value % 1000ULL;
        const bool legacy = remainder >= kLegacyBufferMarkerBase &&
            remainder < kLegacyBufferMarkerBase + kBufferMarkerCount;
        const bool sourcePcr = remainder >= kSourcePllBufferMarkerBase &&
            remainder < kSourcePllBufferMarkerBase + kBufferMarkerCount;
        return legacy || sourcePcr;
    }

    static constexpr bool sourcePllEncodedTargetBitrate(
        std::uint64_t value) noexcept {
        if (value < 100000ULL + kLegacyBufferMarkerBase) return false;
        const std::uint64_t remainder = value % 1000ULL;
        return remainder == kSourcePllBitrateMarker ||
            (remainder >= kSourcePllBufferMarkerBase &&
             remainder < kSourcePllBufferMarkerBase + kBufferMarkerCount);
    }

    static constexpr std::uint64_t decodeTargetBitrate(
        std::uint64_t value) noexcept {
        if (value >= 100000ULL + kSourcePllBitrateMarker &&
            (value % 1000ULL) == kSourcePllBitrateMarker) {
            return value - kSourcePllBitrateMarker;
        }
        return startupBufferEncodedTargetBitrate(value)
            ? value - (value % 1000ULL)
            : value;
    }

    static constexpr std::uint64_t startupBufferMilliseconds(
        std::uint64_t value) noexcept {
        if (!startupBufferEncodedTargetBitrate(value)) {
            return kDefaultStartupBufferMs;
        }
        const std::uint64_t remainder = value % 1000ULL;
        const std::uint64_t base = remainder >= kSourcePllBufferMarkerBase
            ? kSourcePllBufferMarkerBase
            : kLegacyBufferMarkerBase;
        return (remainder - base + 1ULL) * kStartupBufferStepMs;
    }

    // Kept for the existing diagnostics interface.
    std::size_t readySegments() const noexcept;
    std::uint64_t insufficientTargetSegments() const noexcept;

    static bool sourcePacketIsNull(const Packet& packet) noexcept;

    static constexpr std::size_t maximumQueuedBytes() noexcept {
        return kMaximumQueuedBytes;
    }

private:
    struct OutputPcrState {
        bool anchored = false;
        std::uint64_t sourceAnchorTicks = 0;
        std::uint64_t outputAnchorSlot = 0;

        // Historical V174 source-PLL state retained in the structure for binary
        // simplicity. V175+ source-PCR mode preserves provider PCR instead of
        // applying the failed output-slot phase chase.
        std::uint64_t previousSourceTicks = 0;
        std::uint64_t sourceElapsedTicks = 0;
        std::int64_t filteredPhaseTicks = 0;
        std::uint64_t previousOutputDeltaTicks = 0;
    };

    void advanceDeadline() noexcept;
    void makeNullPacket(Packet& packet) noexcept;
    void observeArrival(std::chrono::steady_clock::time_point now) noexcept;
    void observeSourcePcr(const Packet& packet) noexcept;
    void updateUsefulPace(std::chrono::steady_clock::time_point now) noexcept;
    bool restampPcr(Packet& packet, std::uint64_t outputSlot) noexcept;

    static bool isNullPacket(const Packet& packet) noexcept;
    static std::uint16_t packetPid(const Packet& packet) noexcept;
    static bool readPcr(
        const Packet& packet,
        std::uint64_t& pcrTicks,
        bool& discontinuity) noexcept;
    static bool writePcr(Packet& packet, std::uint64_t pcrTicks) noexcept;

    static constexpr std::size_t kMaximumQueuedBytes = 8 * 1024 * 1024;
    static constexpr std::uint64_t kMinimumBitrate = 100000;
    static constexpr std::uint64_t kMaximumBitrate = 200000000;
    static constexpr std::size_t kPidCount = 8192;

    const bool sourcePllMode_;
    std::uint64_t targetBitrate_;
    std::deque<Packet> queuedPackets_;

    // Legacy/observer pacing clock. WisiCbrOutputWorker has a completely
    // separate physical clock in V172.
    std::chrono::steady_clock::time_point nextDeadline_ {};
    std::uint64_t pacingRemainder_ = 0;

    std::uint64_t usefulToken_ = 0;
    std::uint8_t nullContinuityCounter_ = 0;
    bool started_ = false;
    std::size_t lastDatagramUsefulPackets_ = 0;

    // Source media-rate measurement. A single stable reference PCR PID is used
    // only to estimate useful payload rate. PCR restamping itself is performed
    // independently for every PCR-bearing PID in the transport.
    std::uint64_t totalUsefulPackets_ = 0;
    bool haveReferencePcrPid_ = false;
    std::uint16_t referencePcrPid_ = 0;
    bool havePreviousSourcePcr_ = false;
    std::uint64_t previousSourcePcrTicks_ = 0;
    std::uint64_t previousSourcePcrPacketCount_ = 0;
    std::uint64_t mediaWindowTicks_ = 0;
    std::uint64_t mediaWindowPackets_ = 0;
    bool pcrTimingLocked_ = false;
    std::uint64_t sourcePayloadBitrate_ = 0;
    std::uint64_t sourceOverTargetWindows_ = 0;

    // Wall-clock fallback for malformed/no-PCR streams.
    bool arrivalWindowStarted_ = false;
    std::chrono::steady_clock::time_point arrivalWindowStart_ {};
    std::uint64_t arrivalWindowPackets_ = 0;
    std::uint64_t arrivalPayloadBitrate_ = 0;

    // Useful-packet reservoir controller. It only decides real-vs-NULL slots;
    // the configured physical transport bitrate is never modified.
    std::uint64_t usefulPaceBitrate_ = 0;
    std::chrono::steady_clock::time_point lastControllerUpdate_ {};

    // Output PCR clocks. Every PCR PID has its own anchor so SPTS and MPTS are
    // both supported. Legacy mode restamps PCR to output slot phase; Source-PCR
    // mode preserves provider PCR and keeps media timestamps in one clock domain.
    std::uint64_t outputSlotCounter_ = 0;
    std::array<OutputPcrState, kPidCount> outputPcrStates_ {};
    std::uint64_t pcrRewriteCount_ = 0;
    std::uint64_t pcrPidCount_ = 0;
};

} // namespace dvbstreamer5::media::mpegts
