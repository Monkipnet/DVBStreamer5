#pragma once

#include <string>

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

} // namespace dvbstreamer5::media::network
