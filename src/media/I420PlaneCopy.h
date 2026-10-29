#pragma once
// Copy planar 8-bit I420 from decoder-owned, independently pitched Y/U/V planes
// into the tightly packed frame format used by software NVENC, QSV and VAAPI.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace dvbstreamer5::media::codec {

inline bool copyDecoderI420(const std::uint8_t* y, int yStride,
                           const std::uint8_t* u, int uStride,
                           const std::uint8_t* v, int vStride,
                           int width, int height,
                           std::vector<std::uint8_t>& output) {
    if (!y || !u || !v || width <= 0 || height <= 0 ||
        (width & 1) || (height & 1) ||
        yStride < width || uStride < width/2 || vStride < width/2)
        return false;
    const std::size_t ySize = static_cast<std::size_t>(width) * height;
    const std::size_t chromaSize = ySize / 4;
    output.resize(ySize + chromaSize * 2);
    auto* dstY = output.data();
    auto* dstU = dstY + ySize;
    auto* dstV = dstU + chromaSize;
    for (int row = 0; row < height; ++row)
        std::memcpy(dstY + static_cast<std::size_t>(row) * width,
                    y + static_cast<std::size_t>(row) * yStride,
                    static_cast<std::size_t>(width));
    for (int row = 0; row < height/2; ++row) {
        std::memcpy(dstU + static_cast<std::size_t>(row) * (width/2),
                    u + static_cast<std::size_t>(row) * uStride,
                    static_cast<std::size_t>(width/2));
        std::memcpy(dstV + static_cast<std::size_t>(row) * (width/2),
                    v + static_cast<std::size_t>(row) * vStride,
                    static_cast<std::size_t>(width/2));
    }
    return true;
}

// OpenH264's SSysMEMBuffer exposes exactly TWO pitches: Y and a shared
// chroma pitch for both U and V. The array-reference type prevents accidental
// use of a nonexistent third stride. Other decoders may use 3 independent
// pitches and should continue to call copyDecoderI420 directly.
inline bool copyOpenH264I420(const std::uint8_t* y,
                            const std::uint8_t* u,
                            const std::uint8_t* v,
                            const int (&strides)[2],
                            int width, int height,
                            std::vector<std::uint8_t>& output) {
    return copyDecoderI420(y, strides[0],
                           u, strides[1],
                           v, strides[1],
                           width, height, output);
}

} // namespace dvbstreamer5::media::codec
