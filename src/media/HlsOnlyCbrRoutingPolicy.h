#pragma once

#include <cstddef>

namespace dvbstreamer5::media::hls {

// V341: CBR is a network transport pacing requirement, not an HLS media
// segmentation requirement. An HLS-only transcoded tile can use its encoded
// MPEG-TS directly without a redundant bounded CBR observer queue.
// Mixed HTTP/SRT/UDP output and non-transcoded tiles are deliberately excluded.
inline constexpr bool bypassNetworkCbrForTranscodedHlsOnly(
    bool transcodeEnabled, bool hasHlsOutput, bool hasHttpOutput,
    std::size_t nativeOutputCount, std::size_t srtOutputCount,
    std::size_t rtspOutputCount, std::size_t rtmpOutputCount) noexcept {
    return transcodeEnabled && hasHlsOutput && !hasHttpOutput &&
        nativeOutputCount == 0 && srtOutputCount == 0 &&
        rtspOutputCount == 0 && rtmpOutputCount == 0;
}

} // namespace dvbstreamer5::media::hls
