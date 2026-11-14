#pragma once

// V518: automatic WISI-compatible transport/encoder policy for transcoded
// UDP CBR/VBR. Copy and non-UDP outputs are unaffected. No GPU SDK calls.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace dvbstreamer5::media::codec {

struct WisiChameleonProfilePolicy {
    // Same 500-ms decoder PCR preroll as NativeMpegTsMux; 400 ms is an
    // experimental encoder rate-control window, NOT a maximum access unit.
    static constexpr std::uint32_t kCpbWindowMs = 400U;
    static constexpr std::uint32_t kReferencePcrIntervalMs = 20U;

    static bool encodedVideo(bool transcoded,
                             const std::string& videoCodec) noexcept {
        return transcoded && (videoCodec == "h264" ||
               videoCodec == "hevc" || videoCodec == "h265");
    }

    static bool cbrEligible(bool transcoded, bool cbr, bool udpCbrOutput,
                            const std::string& videoCodec) noexcept {
        return encodedVideo(transcoded, videoCodec) && cbr && udpCbrOutput;
    }

    static bool vbrEligible(bool transcoded, bool udpVbrOutput,
                            const std::string& videoCodec) noexcept {
        return encodedVideo(transcoded, videoCodec) && udpVbrOutput;
    }

    // A transcoded VBR program remains genuinely variable-bit-rate. A
    // 2x video+audio/PSI allowance guards physical UDP bursts without
    // forcing NULL stuffing or changing its PCR, PES or PTS/DTS.
    // The output worker must bypass this cap to protect timestamp
    // delivery if it would accumulate more than 80ms extra delay.
    static std::uint64_t vbrPeakBitsPerSecond(
        std::uint64_t video, std::uint64_t audio) noexcept {
        constexpr std::uint64_t kMax = 100000000ULL;
        if (video > kMax || audio > kMax) return 0ULL;
        const auto total = video + audio + 500000ULL;
        return (std::clamp<std::uint64_t>)(total * 2ULL,
                                            4000000ULL, kMax);
    }

    // Preserve the existing transcoder's audio+PSI/TS reserve: at least
    // 500 kbps, or audio bitrate + 5% of the physical transport.
    static std::uint64_t reserveBitsPerSecond(
        std::uint64_t target, std::uint64_t audio) noexcept {
        const std::uint64_t percentage = target / 20ULL;
        if (audio > UINT64_MAX - percentage) return UINT64_MAX;
        return (std::max<std::uint64_t>)(500000ULL, audio + percentage);
    }

    static bool hasTransportHeadroom(
        std::uint64_t target, std::uint64_t video,
        std::uint64_t audio) noexcept {
        if (target < 1000000ULL || video == 0) return false;
        const auto reserve = reserveBitsPerSecond(target, audio);
        return reserve < target && video <= target - reserve;
    }

    // oneVPL BufferSizeInKB denotes kB (1000 bytes). Keep the codec's
    // 16-bit limits; unlike the old 2000-ms setting, V516 opts in to 400 ms.
    static std::uint16_t qsvBufferKB(
        std::uint64_t bitrate, std::uint32_t windowMs = kCpbWindowMs) noexcept {
        if (!windowMs || !bitrate) return 1U;
        // 1 kB = 8,000 bits; milliseconds add another factor of 1,000.
        // Early saturation prevents unsigned overflow on corrupt configs.
        constexpr std::uint64_t kBitsPerKBMillisecond = 8000000ULL;
        constexpr std::uint64_t kMaxKB = 65535ULL;
        if (bitrate > kMaxKB * kBitsPerKBMillisecond / windowMs)
            return static_cast<std::uint16_t>(kMaxKB);
        const auto bitsMilliseconds = bitrate * windowMs;
        const auto kilobytes =
            (bitsMilliseconds + kBitsPerKBMillisecond - 1ULL) /
            kBitsPerKBMillisecond;
        return static_cast<std::uint16_t>(
            (std::clamp<std::uint64_t>)(kilobytes, 1ULL, kMaxKB));
    }

    // NVENC already has a very small 2-frame default VBV. Never enlarge it
    // simply because WISI mode is enabled. Cap opt-in quality-mode VBV to
    // 400 ms so other A/B presets cannot silently increase output bursts.
    static std::uint32_t nvencVbvFrames(
        std::uint32_t currentFrames, double fps) noexcept {
        const double checkedFps = std::isfinite(fps) && fps >= 1.0 &&
            fps <= 120.0 ? fps : 25.0;
        const auto cap = static_cast<std::uint32_t>(
            (std::max)(1.0, std::ceil(checkedFps * kCpbWindowMs / 1000.0)));
        return (std::max)(1U, (std::min)(currentFrames, cap));
    }
};

} // namespace dvbstreamer5::media::codec
