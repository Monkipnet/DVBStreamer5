#pragma once

#include <chrono>

namespace dvbstreamer5::media::ondemand {

// V340: a cold transcoder can take longer than the regular 30-second idle
// timeout to publish its first decoder-safe HLS segment and have it fetched
// by a client. Scope the extended grace only to the *first* HLS media fetch
// after an explicitly requested OnDemand startup; never extend it forever.
inline constexpr auto kHlsColdStartGrace = std::chrono::seconds(120);

inline bool keepTranscodedHlsOnDemandStarting(
    std::chrono::steady_clock::time_point firstHlsRequest,
    std::chrono::steady_clock::time_point now,
    bool firstMediaServed) noexcept {
    if (firstMediaServed || firstHlsRequest ==
        std::chrono::steady_clock::time_point{}) return false;
    return now >= firstHlsRequest &&
        now - firstHlsRequest < kHlsColdStartGrace;
}

} // namespace dvbstreamer5::media::ondemand
