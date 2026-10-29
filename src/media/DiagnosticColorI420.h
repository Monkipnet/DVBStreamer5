#pragma once
// Diagnostic color quadrants after decoder, before resize/encode. Unlike
// neutral gray, this distinguishes U/V plane mapping and chroma corruption.
#include "media/NativeCodecRuntime.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dvbstreamer5::media::codec {

inline bool fillDiagnosticColorI420(RawVideoFrame& frame, std::uint64_t frameNumber) {
    const int w = frame.width;
    const int h = frame.height;
    if (w < 4 || h < 4 || (w & 1) || (h & 1)) return false;
    const auto ySize = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (frame.i420.size() < ySize + ySize / 2) return false;
    auto* y = frame.i420.data();
    auto* u = y + ySize;
    auto* v = u + ySize / 4;
    struct Yuv { std::uint8_t y, u, v; };
    // 4 broad blocks, not stripes: gray, warm, blue, green.
    constexpr Yuv colors[4] = {
        {145,128,128}, {140,96,195},
        {145,192,80}, {136,77,81}
    };
    const int splitX = ((w / 2) & ~1);
    const int splitY = ((h / 2) & ~1);
    for (int row = 0; row < h; row += 2) {
        const int bottom = row >= splitY ? 2 : 0;
        for (int col = 0; col < w; col += 2) {
            const Yuv c = colors[bottom + (col >= splitX ? 1 : 0)];
            const std::size_t cidx = static_cast<std::size_t>(row / 2) * (w / 2) + col / 2;
            u[cidx] = c.u; v[cidx] = c.v;
            for (int dy=0;dy<2;++dy) {
                const std::size_t offset = static_cast<std::size_t>(row + dy) * w + col;
                y[offset] = c.y;
                y[offset + 1] = c.y;
            }
        }
    }
    // Bright moving marker in the gray quadrant shows that frames are live.
    const int size = std::max(2, std::min({splitX / 8, splitY / 8, 32}));
    const int travel = std::max(1, splitX - size);
    const int left = static_cast<int>((frameNumber * 5ULL) % static_cast<std::uint64_t>(travel));
    const int top = std::max(0, splitY / 4);
    for(int row=top;row<std::min(splitY, top+size);++row)
        std::fill(y+static_cast<std::size_t>(row)*w+left,
                  y+static_cast<std::size_t>(row)*w+std::min(splitX,left+size),
                  static_cast<std::uint8_t>(235));
    return true;
}

} // namespace dvbstreamer5::media::codec
