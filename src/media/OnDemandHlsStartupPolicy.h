#pragma once

#include <chrono>

namespace dvbstreamer5::media::ondemand {

// V340: a cold transcoder can take longer than the regular 30-second idle
// timeout to publish its first decoder-safe HLS segment and have it fetched
// by a client. Scope the extended grace only to the *first* HLS media fetch
// after an explicitly requested OnDemand startup; never extend it forever.
inline constexpr auto kHlsColdStartGrace = std::chrono::seconds(120);

// V529: An HLS player consumes live media through short-lived segment GETs,
// not a persistent HTTP socket. Once a real segment was served, allow a bounded
// playback/buffer gap before releasing the OnDemand pipeline. A manifest-only
// probe must NOT extend the lifetime. This is independent of cold start.
inline constexpr auto kHlsRecentMediaGrace = std::chrono::seconds(75);

inline bool keepHlsOnDemandFromRecentMedia(
    std::chrono::steady_clock::time_point lastMediaResponse,
    std::chrono::steady_clock::time_point now) noexcept {
    return lastMediaResponse != std::chrono::steady_clock::time_point{} &&
        now >= lastMediaResponse &&
        now - lastMediaResponse < kHlsRecentMediaGrace;
}

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
