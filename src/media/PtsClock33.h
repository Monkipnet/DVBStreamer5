#pragma once

#include <cstdint>

namespace dvbstreamer5::media::mpegts {

// MPEG-TS PTS/DTS is a 33-bit 90-kHz clock. An elementary decoder is free to
// produce an unwrapped 64-bit count while the audio encoder emits the wrapped
// 33-bit value. All cross-stream ordering/lead comparisons must use modular
// time, otherwise an A/V mux deadlocks once every 2^33 ticks (~26.5 hours).
inline constexpr std::uint64_t kPts33Modulus = 1ULL << 33U;
inline constexpr std::uint64_t kPts33Mask = kPts33Modulus - 1ULL;
inline constexpr std::uint64_t kPts33Half = kPts33Modulus >> 1U;

inline constexpr std::int64_t ptsDelta90k(std::uint64_t lhs,
                                            std::uint64_t rhs) noexcept {
    const std::uint64_t delta = (lhs - rhs) & kPts33Mask;
    return delta < kPts33Half
        ? static_cast<std::int64_t>(delta)
        : static_cast<std::int64_t>(delta) -
              static_cast<std::int64_t>(kPts33Modulus);
}

// True if lhs is later than rhs by more than limit 90-kHz ticks.
// Assumes A/V timestamps are within half of the MPEG clock cycle of each
// other (normal live-stream operation).
inline constexpr bool ptsAheadOf90k(std::uint64_t lhs,
                                     std::uint64_t rhs,
                                     std::uint64_t limit) noexcept {
    return ptsDelta90k(lhs, rhs) > static_cast<std::int64_t>(limit);
}

inline constexpr bool ptsBefore90k(std::uint64_t lhs,
                                    std::uint64_t rhs) noexcept {
    return ptsDelta90k(lhs, rhs) < 0;
}

} // namespace dvbstreamer5::media::mpegts
