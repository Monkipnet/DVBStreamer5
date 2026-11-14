#pragma once

// V516: per-stream WISI Chameleon encoder/UDP-CBR compatibility profile.
// Configuration is off by default. This policy contains no GPU SDK calls,
// packet mutations, dynamic allocation, or environment dependence.

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

    static bool eligible(bool requested, bool transcoded, bool cbr,
                         bool udpCbrOutput, const std::string& videoCodec) noexcept {
        return requested && transcoded && cbr && udpCbrOutput &&
               videoCodec != "copy" &&
               (videoCodec == "h264" || videoCodec == "hevc" ||
                videoCodec == "h265");
    }

    // Preserve the existing transcoder's audio+PSI/TS reserve: at least
    // 500 kbps, or audio bitrate + 5% of the physical transport.
    static std::uint64_t reserveBitsPerSecond(
        std::uint64_t target, std::uint64_t audio) noexcept {
        const std::uint64_t percentage = target / 20ULL;
        if (audio > UINT64_MAX - percentage) return UINT64_MAX;
        return (std::max)(500000ULL, audio + percentage);
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
        const std::uint64_t bytes = bitrate / 8ULL;
        const std::uint64_t sizeKB =
            (bytes * windowMs / 1000ULL + 999ULL) / 1000ULL;
        return static_cast<std::uint16_t>(
            (std::clamp)(sizeKB, 1ULL, 65535ULL));
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
