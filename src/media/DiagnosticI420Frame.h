#pragma once

// Diagnostic-only post-decoder I420 image. Preserve frame geometry and PTS,
// replace only the pixel planes. Neutral U/V makes unintended chroma bands
// visible downstream (scaling, NV12 upload or hardware encoding).
#include "media/NativeCodecRuntime.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dvbstreamer5::media::codec {

inline bool fillDiagnosticNeutralI420(RawVideoFrame& frame,
                                     std::uint64_t frameIndex) {
    const int w = frame.width;
    const int h = frame.height;
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return false;

    const auto ySize = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (frame.i420.size() < ySize + ySize / 2) return false;
    auto* y = frame.i420.data();
    auto* u = y + ySize;
    auto* v = u + ySize / 4;
    std::fill(y, y + ySize, static_cast<std::uint8_t>(144));
    std::fill(u, u + ySize / 4, static_cast<std::uint8_t>(128));
    std::fill(v, v + ySize / 4, static_cast<std::uint8_t>(128));

    // Neutral gray raster grid: NO intentional colored vertical bars.
    // Use 2-pixel even-grid lines to avoid chroma ambiguity in 4:2:0.
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            if ((row % 64) < 2 || (col % 64) < 2)
                y[static_cast<std::size_t>(row) * w + col] = 56;
        }
    }
    // Moving white square proves the encoder receives fresh frames.
    const int square = std::max(2, std::min({w/8, h/8, 48}));
    const int range = std::max(1, w - square);
    const int left = static_cast<int>((frameIndex * 7U) % static_cast<std::uint64_t>(range));
    const int top = std::max(0, h / 2 - square / 2);
    for (int row = top; row < std::min(h, top + square); ++row)
        std::fill(y + static_cast<std::size_t>(row) * w + left,
                  y + static_cast<std::size_t>(row) * w +
                      std::min(w, left + square),
                  static_cast<std::uint8_t>(235));
    return true;
}

} // namespace dvbstreamer5::media::codec
