#pragma once
// V10.8.346: per-SRT-output MPEG-TS clock/CC continuity across source failover.
// Source switch does not close the SRT socket. Rebase both PCR and PES PTS/DTS
// with ONE offset, retaining decoder lead and A/V synchronization.
#include "media/TransportStream.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace dvbstreamer5::media::srt {

class NativeSrtSwitchClock {
public:
    static constexpr std::uint64_t kMask = (1ULL << 33U) - 1U;
    static constexpr std::uint64_t kTransitionGap90k = 9000ULL; // 100 ms

    void sourceSwitched() noexcept {
        pendingAnchor_ = true;
        rewriteContinuity_ = true;
        ++transitions_;
    }
    std::uint64_t transitions() const noexcept { return transitions_; }
    std::uint64_t lastMediaClock90k() const noexcept { return lastOutput90k_; }

    // Caller serializes calls. A 188-byte TS packet is modified in-place.
    void apply(std::uint8_t* data, std::size_t size) noexcept {
        dvbstreamer5::media::mpegts::PacketInfo info;
        if (!dvbstreamer5::media::mpegts::inspectPacket(data, size, info)) return;
        constexpr auto kPacketSize = dvbstreamer5::media::mpegts::kPacketSize;
        if (info.pid != dvbstreamer5::media::mpegts::kNullPid) {
            const std::uint8_t incomingCc = static_cast<std::uint8_t>(data[3] & 15U);
            if (rewriteContinuity_ && seen_[info.pid])
                data[3] = static_cast<std::uint8_t>(
                    (data[3] & 0xf0U) | nextCc_[info.pid]);
            const std::uint8_t outputCc = static_cast<std::uint8_t>(data[3] & 15U);
            nextCc_[info.pid] = static_cast<std::uint8_t>(
                (outputCc + (info.hasPayload ? 1U : 0U)) & 15U);
            seen_[info.pid] = true;
            (void)incomingCc;
        }

        std::uint8_t* pts = nullptr;
        std::uint8_t* dts = nullptr;
        if (info.payloadUnitStart && info.hasPayload &&
            info.payloadOffset + 14U <= kPacketSize) {
            auto* pes = data + info.payloadOffset;
            if (pes[0] == 0U && pes[1] == 0U && pes[2] == 1U) {
                const unsigned flags = (pes[7] >> 6U) & 3U;
                if ((flags == 2U || flags == 3U) && pes[8] >= 5U)
                    pts = pes + 9;
                if (flags == 3U && pes[8] >= 10U &&
                    info.payloadOffset + 19U <= kPacketSize)
                    dts = pes + 14;
            }
        }
        // The first real clock from the new source determines its offset.
        // PSI may precede that clock; its continuity was already normalized.
        if (pendingAnchor_ && (info.hasPcr || pts)) {
            const std::uint64_t firstRaw =
                info.hasPcr ? info.pcrBase90k : readStamp(pts);
            if (haveOutputClock_)
                offset90k_ = (lastOutput90k_ + kTransitionGap90k +
                              (kMask + 1ULL) - firstRaw) & kMask;
            else
                offset90k_ = 0;
            pendingAnchor_ = false;
        }
        if (info.hasPcr) {
            const std::uint64_t out =
                (info.pcrBase90k + offset90k_) & kMask;
            data[6] = static_cast<std::uint8_t>(out >> 25);
            data[7] = static_cast<std::uint8_t>(out >> 17);
            data[8] = static_cast<std::uint8_t>(out >> 9);
            data[9] = static_cast<std::uint8_t>(out >> 1);
            data[10] = static_cast<std::uint8_t>(
                ((out & 1ULL) << 7U) | (data[10] & 0x7fU));
            advanceClock(out);
        }
        if (pts) {
            const std::uint64_t out = (readStamp(pts) + offset90k_) & kMask;
            writeStamp(pts, out);
            advanceClock(out);
        }
        if (dts) {
            const std::uint64_t out = (readStamp(dts) + offset90k_) & kMask;
            writeStamp(dts, out);
        }
    }

private:
    static std::uint64_t readStamp(const std::uint8_t* p) noexcept {
        return (static_cast<std::uint64_t>((p[0] >> 1U) & 7U) << 30U) |
               (static_cast<std::uint64_t>(p[1]) << 22U) |
               (static_cast<std::uint64_t>(p[2] >> 1U) << 15U) |
               (static_cast<std::uint64_t>(p[3]) << 7U) |
               static_cast<std::uint64_t>(p[4] >> 1U);
    }
    static void writeStamp(std::uint8_t* p, std::uint64_t value) noexcept {
        p[0] = static_cast<std::uint8_t>(
            (p[0] & 0xf1U) | (((value >> 30U) & 7ULL) << 1U));
        p[1] = static_cast<std::uint8_t>(value >> 22U);
        p[2] = static_cast<std::uint8_t>(
            (((value >> 15U) & 0x7fULL) << 1U) | 1U);
        p[3] = static_cast<std::uint8_t>(value >> 7U);
        p[4] = static_cast<std::uint8_t>(((value & 0x7fULL) << 1U) | 1U);
    }
    void advanceClock(std::uint64_t out) noexcept {
        if (!haveOutputClock_ || out > lastOutput90k_)
            lastOutput90k_ = out;
        haveOutputClock_ = true;
    }

    std::array<std::uint8_t, 8192> nextCc_{};
    std::array<bool, 8192> seen_{};
    std::uint64_t lastOutput90k_ = 0;
    std::uint64_t offset90k_ = 0;
    std::uint64_t transitions_ = 0;
    bool haveOutputClock_ = false;
    bool pendingAnchor_ = false;
    bool rewriteContinuity_ = false;
};

} // namespace dvbstreamer5::media::srt
