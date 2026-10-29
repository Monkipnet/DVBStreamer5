#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dvbstreamer5::media::hls {

struct SegmentSettings {
    double durationSeconds = 4.0;
    std::size_t count = 8;
    bool customDuration = false;
    bool customCount = false;
};

// 0 = Auto and deliberately preserves existing V10.8.217 per-container
// effective defaults. Each field can independently override its own default.
inline SegmentSettings resolveSegmentSettings(
    std::uint32_t duration, std::uint32_t count, bool cmaf) {
    SegmentSettings settings;
    settings.customDuration = duration != 0U;
    settings.customCount = count != 0U;
    settings.durationSeconds = settings.customDuration
        ? static_cast<double>(std::clamp(duration, 1U, 10U))
        : (cmaf ? 2.0 : 4.0);
    settings.count = settings.customCount
        ? static_cast<std::size_t>(std::clamp(count, 3U, 30U))
        : (cmaf ? 6U : 8U);
    return settings;
}

} // namespace dvbstreamer5::media::hls
