#pragma once

#include <cstdint>
#include <cstring>

#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define DVBSTREAMER5_VERTICAL_AVX2_RUNTIME 1
#endif

namespace dvbstreamer5::media::codec::detail {

inline void blendVerticalRowScalar(const std::uint16_t* top,
                                   const std::uint16_t* bottom,
                                   std::uint8_t* output,
                                   int width, unsigned fy) {
    const unsigned ify = 256U - fy;
    for (int x = 0; x < width; ++x) {
        const unsigned i = static_cast<unsigned>(x);
        output[x] = static_cast<std::uint8_t>(
            (static_cast<unsigned>(top[i]) * ify +
             static_cast<unsigned>(bottom[i]) * fy + 32768U) >> 16U);
    }
}

#if defined(DVBSTREAMER5_VERTICAL_AVX2_RUNTIME)
__attribute__((target("avx2")))
inline void blendVerticalRowAvx2(const std::uint16_t* top,
                                 const std::uint16_t* bottom,
                                 std::uint8_t* output,
                                 int width, unsigned fy) {
    const __m256i y0 = _mm256_set1_epi32(static_cast<int>(256U - fy));
    const __m256i y1 = _mm256_set1_epi32(static_cast<int>(fy));
    const __m256i round = _mm256_set1_epi32(32768);
    const __m256i zero = _mm256_setzero_si256();
    int x = 0;
    for (; x + 8 <= width; x += 8) {
        const __m256i t = _mm256_cvtepu16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(top + x)));
        const __m256i b = _mm256_cvtepu16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(bottom + x)));
        const __m256i sum = _mm256_add_epi32(
            _mm256_add_epi32(
                _mm256_mullo_epi32(t, y0), _mm256_mullo_epi32(b, y1)),
            round);
        const __m256i p32 = _mm256_srli_epi32(sum, 16);
        const __m256i p16 = _mm256_packus_epi32(p32, zero);
        const __m128i lo = _mm256_castsi256_si128(p16);
        const __m128i hi = _mm256_extracti128_si256(p16, 1);
        const __m128i together = _mm_unpacklo_epi64(lo, hi);
        const __m128i bytes = _mm_packus_epi16(together, _mm_setzero_si128());
        const std::uint64_t packed =
            static_cast<std::uint64_t>(_mm_cvtsi128_si64(bytes));
        std::memcpy(output + x, &packed, sizeof(packed));
    }
    if (x < width) {
        blendVerticalRowScalar(top + x, bottom + x, output + x,
                               width - x, fy);
    }
}
#endif

inline void blendVerticalRow(const std::uint16_t* top,
                             const std::uint16_t* bottom,
                             std::uint8_t* output,
                             int width, unsigned fy) {
#if defined(DVBSTREAMER5_VERTICAL_AVX2_RUNTIME)
    static const bool avx2Available = __builtin_cpu_supports("avx2");
    if (avx2Available) {
        blendVerticalRowAvx2(top, bottom, output, width, fy);
        return;
    }
#endif
    blendVerticalRowScalar(top, bottom, output, width, fy);
}

} // namespace dvbstreamer5::media::codec::detail
