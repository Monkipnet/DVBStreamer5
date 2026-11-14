#pragma once

// V515: physical output-clock PCR for an explicitly selected test CBR tile.
// PCR-only packets have no payload, therefore the PID continuity counter must
// be the CC of the most recently emitted *payload* packet on that PID.

#include "media/TransportStream.h"

#include <cstdint>
#include <string>

namespace dvbstreamer5::media::mpegts {

struct CbrOutputPcrSmoothingPolicy {
    static constexpr std::uint64_t kPcrHz = 27000000ULL;
    static constexpr std::uint64_t kPcrWrap = (1ULL << 33U) * 300ULL;
    static constexpr std::uint64_t kPacketBits = 188ULL * 8ULL;
    static constexpr std::uint64_t kIntervalMs = 20ULL;

    static bool selected(const std::string& streamId, const char* selector) noexcept {
        return selector && *selector && !streamId.empty() && streamId == selector;
    }

    static bool due(std::uint64_t slot, std::uint64_t previousSlot,
                    std::uint64_t bitrate) noexcept {
        if (bitrate == 0 || slot <= previousSlot) return false;
        return static_cast<unsigned __int128>(slot - previousSlot) *
                   kPacketBits * 1000ULL >=
               static_cast<unsigned __int128>(bitrate) * kIntervalMs;
    }

    static std::uint64_t outputPcr(std::uint64_t anchorPcr,
                                   std::uint64_t slotsSinceAnchor,
                                   std::uint64_t bitrate) noexcept {
        if (!bitrate) return anchorPcr % kPcrWrap;
        const auto elapsed = static_cast<std::uint64_t>(
            static_cast<unsigned __int128>(slotsSinceAnchor) *
            kPacketBits * kPcrHz / bitrate);
        return (anchorPcr + elapsed) % kPcrWrap;
    }

    static void makeClockOnly(Packet& packet, std::uint16_t pid,
                              std::uint8_t lastPayloadCc,
                              std::uint64_t pcrTicks) noexcept {
        packet.fill(0xff);
        packet[0] = 0x47;
        packet[1] = static_cast<std::uint8_t>((pid >> 8U) & 0x1fU);
        packet[2] = static_cast<std::uint8_t>(pid);
        packet[3] = static_cast<std::uint8_t>(0x20U | (lastPayloadCc & 0x0fU));
        packet[4] = 183; // adaptation-only, no payload, no CC increment
        packet[5] = 0x10; // PCR_flag, no discontinuity or random-access flag

        pcrTicks %= kPcrWrap;
        const auto base = pcrTicks / 300ULL;
        const auto ext = pcrTicks % 300ULL;
        packet[6] = static_cast<std::uint8_t>(base >> 25U);
        packet[7] = static_cast<std::uint8_t>(base >> 17U);
        packet[8] = static_cast<std::uint8_t>(base >> 9U);
        packet[9] = static_cast<std::uint8_t>(base >> 1U);
        packet[10] = static_cast<std::uint8_t>(
            ((base & 1ULL) << 7U) | 0x7eU | ((ext >> 8U) & 1ULL));
        packet[11] = static_cast<std::uint8_t>(ext);
    }
};

} // namespace dvbstreamer5::media::mpegts
