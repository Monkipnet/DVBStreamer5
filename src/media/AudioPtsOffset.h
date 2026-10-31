#pragma once

#include <algorithm>
#include <cstdint>

namespace dvbstreamer5::media::codec {

// Per-tile A/V lip-sync adjustment. Positive milliseconds delay audio;
// negative milliseconds advance audio. The MPEG-TS PTS/DTS domain is 33-bit
// modulo 2^33 at 90 kHz. Never change payload or resampling rate.
inline std::uint64_t shiftAudioPts90k(std::uint64_t pts90k,
                                     int audioOffsetMs) noexcept {
    constexpr std::uint64_t mask = (1ULL << 33U) - 1ULL;
    constexpr std::int64_t modulus = 1LL << 33U;
    const auto clamped = std::clamp(audioOffsetMs, -500, 500);
    const auto shifted =
        static_cast<std::int64_t>(pts90k & mask) +
        static_cast<std::int64_t>(clamped) * 90LL;
    return static_cast<std::uint64_t>(
        (shifted + modulus) % modulus);
}

} // namespace dvbstreamer5::media::codec
