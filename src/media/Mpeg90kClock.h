#pragma once

#include <cstdint>

namespace dvbstreamer5::media::mpegts {

// PTS/DTS use a 33-bit clock which wraps approximately every 26.5 hours.
// Every comparison must be local (less than half that span). Unsigned
// arithmetic on raw timestamps breaks audio/video ordering at wraparound.
struct Clock90k {
    static constexpr std::uint64_t kModulus = 1ULL << 33U;
    static constexpr std::uint64_t kMask = kModulus - 1ULL;
    static constexpr std::uint64_t kHalf = kModulus >> 1U;

    static constexpr std::uint64_t masked(std::uint64_t value) noexcept {
        return value & kMask;
    }

    static constexpr std::int64_t delta(std::uint64_t a,
                                         std::uint64_t b) noexcept {
        const auto forward = (a - b) & kMask;
        return forward < kHalf
            ? static_cast<std::int64_t>(forward)
            : static_cast<std::int64_t>(forward) -
                  static_cast<std::int64_t>(kModulus);
    }

    static constexpr bool before(std::uint64_t a,
                                 std::uint64_t b) noexcept {
        return delta(a, b) < 0;
    }

    static constexpr bool aheadBy(std::uint64_t a,
                                  std::uint64_t b,
                                  std::uint64_t threshold90k) noexcept {
        return delta(a, b) > static_cast<std::int64_t>(threshold90k);
    }

    // Extend a raw 33-bit clock to the epoch of the preceding packet.
    // A small backward PTS jitter is not a valid forward frame.
    static constexpr std::uint64_t advanceMonotonic(
        std::uint64_t current,
        std::uint64_t previous,
        std::uint64_t fallbackDuration) noexcept {
        if (previous == 0) return masked(current);
        const std::int64_t step = delta(current, previous);
        return step > 0
            ? previous + static_cast<std::uint64_t>(step)
            : previous + fallbackDuration;
    }
};

} // namespace dvbstreamer5::media::mpegts
