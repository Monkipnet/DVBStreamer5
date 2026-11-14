#pragma once
// V518: VBR peak spacing is a best-effort guard, not CBR replacement.
// Do not rewrite media PCR/PTS/DTS or allow a cap to add >80 ms latency.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dvbstreamer5::media::network {
struct VbrPeakRatePolicy {
    static constexpr std::chrono::milliseconds kMaximumDebt{80};

    static std::int64_t spacingNs(std::size_t previousBytes,
                                  std::uint64_t peakBps) noexcept {
        if (!previousBytes || peakBps == 0) return 0;
        constexpr std::uint64_t kBitsToNs = 8000000000ULL;
        if (previousBytes > std::numeric_limits<std::uint64_t>::max() / kBitsToNs)
            return std::numeric_limits<std::int64_t>::max();
        const auto numerator = static_cast<std::uint64_t>(previousBytes) * kBitsToNs;
        const auto interval = numerator / peakBps +
            static_cast<std::uint64_t>(numerator % peakBps != 0);
        return interval <= static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
            ? static_cast<std::int64_t>(interval)
            : std::numeric_limits<std::int64_t>::max();
    }

    template<typename Clock>
    static bool canDelay(std::chrono::time_point<Clock> sourceDue,
                         std::chrono::time_point<Clock> cappedDue) noexcept {
        return cappedDue > sourceDue &&
               cappedDue - sourceDue <= kMaximumDebt;
    }
};
} // namespace dvbstreamer5::media::network
