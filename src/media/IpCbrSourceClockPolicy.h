#pragma once

#include <string>
#include <cstdint>
#include "media/CbrTsPacer.h"

namespace dvbstreamer5::media::network {

// V334: selected per-stream GUI checkbox replaces V333 service-environment A/B.
// An inactive or unrelated stream always retains its previous PCR policy.
inline bool enableIpCbrSourceClock(
    bool enabled, const std::string& lowerInputUri,
    bool transcodeEnabled, bool cbrEnabled,
    bool hasUdpCbrOutput, bool wholeKbpsTarget,
    bool testPattern = false, bool hlsInput = false) noexcept {
    if (!enabled || transcodeEnabled || !cbrEnabled || !hasUdpCbrOutput ||
        !wholeKbpsTarget || testPattern || hlsInput) return false;
    return lowerInputUri.rfind("http://",0)==0 ||
           lowerInputUri.rfind("https://",0)==0 ||
           lowerInputUri.rfind("srt://",0)==0;
}

// Keep the pre-existing CBR startup-buffer preference when switching the
// selected stream to the source-PCR clock domain. Encoded bitrate markers
// never affect the actual whole-kbps transmission rate.
inline constexpr std::uint64_t sourcePcrCbrTargetKeepingStartupBuffer(
    std::uint64_t configuredBitrate) noexcept {
    using mpegts::CbrTsPacer;
    const auto actual = CbrTsPacer::decodeTargetBitrate(configuredBitrate);
    const auto bufferMs = CbrTsPacer::startupBufferMilliseconds(configuredBitrate);
    if (bufferMs == 0) return actual + CbrTsPacer::kSourcePllNoStartupBufferMarker;
    if (bufferMs == CbrTsPacer::kDefaultStartupBufferMs)
        return actual + CbrTsPacer::kSourcePllBitrateMarker;
    // The UI already restricts the startup buffer to 500-ms steps.
    const auto steps = bufferMs / CbrTsPacer::kStartupBufferStepMs;
    return actual + CbrTsPacer::kSourcePllBufferMarkerBase + steps - 1ULL;
}

static_assert(sourcePcrCbrTargetKeepingStartupBuffer(4000000ULL) == 4000777ULL);
static_assert(sourcePcrCbrTargetKeepingStartupBuffer(4000699ULL) == 4000799ULL);
static_assert(sourcePcrCbrTargetKeepingStartupBuffer(4000703ULL) == 4000803ULL);

} // namespace dvbstreamer5::media::network
