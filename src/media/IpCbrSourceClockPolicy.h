#pragma once

#include <string>

namespace dvbstreamer5::media::network {

// V10.8.333: deliberately per-stream A/B for passthrough MPEG-TS carried
// over HTTP(S) or SRT and sent as UDP-CBR. Do not silently change the
// working PCR policy for DVB, transcode, HLS, or unrelated streams.
inline bool enableIpCbrSourceClockAb(
    const char* selectedStreamId, const std::string& streamId,
    const std::string& lowerInputUri, bool transcodeEnabled,
    bool cbrEnabled, bool hasUdpCbrOutput, bool wholeKbpsTarget) noexcept {
    if (!selectedStreamId || !*selectedStreamId ||
        streamId != selectedStreamId || transcodeEnabled ||
        !cbrEnabled || !hasUdpCbrOutput || !wholeKbpsTarget) return false;
    return lowerInputUri.rfind("http://",0)==0 ||
           lowerInputUri.rfind("https://",0)==0 ||
           lowerInputUri.rfind("srt://",0)==0;
}

} // namespace dvbstreamer5::media::network
