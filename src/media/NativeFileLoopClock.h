#pragma once

#include "media/TransportStream.h"

#include <array>
#include <cstddef>
#include <cstdint>

// V342: keep a repeatedly played replacement TS on one monotonic media
// timeline. PCR and PES PTS/DTS get exactly the same 33-bit offset; all
// per-PID continuity counters remain continuous across file rewinds.
// Only used for backup_file_loop, not for live inputs.
namespace dvbstreamer5::media::mpegts {
class NativeFileLoopClock {
public:
    static constexpr std::uint64_t kMask = (1ULL << 33U) - 1ULL;

    void nextLoop() noexcept {
        if (haveFirstPcr_ && haveLastPcr_) {
            // Include a small positive gap for the next random-access frame.
            const std::uint64_t span = (lastRawPcr_ - firstRawPcr_) & kMask;
            offset_ = (offset_ + span + 3600ULL) & kMask;
        } else {
            // Missing PCR cannot provide an exact loop period. The source
            // remains usable for PSI/audio-only TS; preserve bounded offset.
            offset_ = (offset_ + 90000ULL) & kMask;
        }
        haveFirstPcr_ = false;
        haveLastPcr_ = false;
        ++loops_;
    }

    void apply(Packet& packet) noexcept {
        PacketInfo info;
        if (!inspectPacket(packet.data(), packet.size(), info)) return;
        const auto pid = info.pid;
        if (pid != kNullPid) {
            auto& cc = continuity_[pid];
            if (seen_[pid]) {
                packet[3] = static_cast<std::uint8_t>((packet[3] & 0xf0U) | cc);
            }
            seen_[pid] = true;
            cc = static_cast<std::uint8_t>(
                (packet[3] & 0x0fU) + (info.hasPayload ? 1U : 0U));
            cc &= 0x0fU;
        }
        if (info.hasPcr) {
            const std::uint64_t raw = info.pcrBase90k;
            if (!haveFirstPcr_) { firstRawPcr_ = raw; haveFirstPcr_ = true; }
            lastRawPcr_ = raw;
            haveLastPcr_ = true;
            const auto out = (raw + offset_) & kMask;
            packet[6] = static_cast<std::uint8_t>(out >> 25);
            packet[7] = static_cast<std::uint8_t>(out >> 17);
            packet[8] = static_cast<std::uint8_t>(out >> 9);
            packet[9] = static_cast<std::uint8_t>(out >> 1);
            packet[10] = static_cast<std::uint8_t>(
                ((out & 1ULL) << 7U) | (packet[10] & 0x7fU));
        }
        if (!info.payloadUnitStart || !info.hasPayload ||
            info.payloadOffset + 14U > kPacketSize) return;
        auto* pes = packet.data() + info.payloadOffset;
        if (pes[0] || pes[1] || pes[2] != 1U) return;
        const unsigned stampFlags = (pes[7] >> 6U) & 3U;
        if ((stampFlags != 2U && stampFlags != 3U) || pes[8] < 5U ||
            info.payloadOffset + 14U > kPacketSize) return;
        adjustStamp(pes + 9);
        if (stampFlags == 3U && pes[8] >= 10U &&
            info.payloadOffset + 19U <= kPacketSize) {
            adjustStamp(pes + 14);
        }
    }

    std::uint64_t loops() const noexcept { return loops_; }

private:
    void adjustStamp(std::uint8_t* p) const noexcept {
        const auto raw =
            (static_cast<std::uint64_t>((p[0] >> 1U) & 0x07U) << 30U) |
            (static_cast<std::uint64_t>(p[1]) << 22U) |
            (static_cast<std::uint64_t>(p[2] >> 1U) << 15U) |
            (static_cast<std::uint64_t>(p[3]) << 7U) |
            static_cast<std::uint64_t>(p[4] >> 1U);
        const auto value = (raw + offset_) & kMask;
        p[0] = static_cast<std::uint8_t>((p[0] & 0xf1U) |
            (((value >> 30U) & 7ULL) << 1U));
        p[1] = static_cast<std::uint8_t>(value >> 22U);
        p[2] = static_cast<std::uint8_t>((((value >> 15U) & 0x7fULL) << 1U) | 1U);
        p[3] = static_cast<std::uint8_t>(value >> 7U);
        p[4] = static_cast<std::uint8_t>(((value & 0x7fULL) << 1U) | 1U);
    }
    std::array<std::uint8_t,8192> continuity_{};
    std::array<bool,8192> seen_{};
    std::uint64_t offset_ = 0;
    std::uint64_t firstRawPcr_ = 0;
    std::uint64_t lastRawPcr_ = 0;
    std::uint64_t loops_ = 0;
    bool haveFirstPcr_ = false;
    bool haveLastPcr_ = false;
};
} // namespace dvbstreamer5::media::mpegts
