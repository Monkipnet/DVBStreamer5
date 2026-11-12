#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace dvbstreamer5::media::codec::detail {

// One-pass, byte-exact planar I420 -> NV12 upload. The hardware Y and UV
// pitches can differ (vaDeriveImage can return independent strides).
// Padding is not touched; only visible pixels are copied.
inline void copyI420ToNv12(const std::uint8_t* srcY,
                           const std::uint8_t* srcU,
                           const std::uint8_t* srcV,
                           int width, int height,
                           std::uint8_t* dstY, int dstYPitch,
                           std::uint8_t* dstUV, int dstUVPitch) noexcept {
    if (!srcY || !srcU || !srcV || !dstY || !dstUV ||
        width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        dstYPitch < width || dstUVPitch < width)
        return;

    for (int row = 0; row < height; ++row)
        std::memcpy(dstY + static_cast<std::size_t>(row) * dstYPitch,
                    srcY + static_cast<std::size_t>(row) * width,
                    static_cast<std::size_t>(width));

    const int cw = width / 2;
    for (int row = 0; row < height / 2; ++row) {
        const auto* u = srcU + static_cast<std::size_t>(row) * cw;
        const auto* v = srcV + static_cast<std::size_t>(row) * cw;
        auto* dst = dstUV + static_cast<std::size_t>(row) * dstUVPitch;
        int x = 0;
#if defined(__SSE2__)
        // SSE2 is baseline for x86_64. Unaligned loads/stores also handle
        // arbitrary mapped-surface pitches and source pointer alignment.
        for (; x + 16 <= cw; x += 16) {
            const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(u + x));
            const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(v + x));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 2 * x),
                             _mm_unpacklo_epi8(a, b));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 2 * x + 16),
                             _mm_unpackhi_epi8(a, b));
        }
#endif
        for (; x < cw; ++x) {
            dst[2 * x] = u[x];
            dst[2 * x + 1] = v[x];
        }
    }
}

} // namespace dvbstreamer5::media::codec::detail
