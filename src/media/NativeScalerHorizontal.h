#pragma once

#include <array>
#include <cstdint>
#include <vector>

#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define DVBSTREAMER5_HORIZONTAL_SSSE3_RUNTIME 1
#endif

namespace dvbstreamer5::media::codec::detail {

// Precomputed for a block of 16 destination pixels. Pixel coordinates,
// weights and rounding are the same as the original scalar scaler.
struct HorizontalShuffleBlock {
    int sourceBase = 0;
    bool valid = false;
    alignas(16) std::array<std::uint8_t,16> leftIndex{};
    alignas(16) std::array<std::uint8_t,16> rightIndex{};
    alignas(16) std::array<std::uint16_t,16> leftWeight{};
    alignas(16) std::array<std::uint16_t,16> rightWeight{};
};

inline HorizontalShuffleBlock prepareHorizontalShuffleBlock(
    const int* x0, const int* x1, const std::uint16_t* wx,
    int inputWidth, int outputStart) {
    HorizontalShuffleBlock block;
    const int base = x0[outputStart];
    // Never read beyond a source-row boundary, including its final block.
    if (base < 0 || base + 16 > inputWidth) return block;
    block.sourceBase = base;
    for (int i = 0; i < 16; ++i) {
        const int left = x0[outputStart+i] - base;
        const int right = x1[outputStart+i] - base;
        // Downscaling / wide source spans keep the safe scalar path.
        if (left < 0 || left >= 16 || right < 0 || right >= 16)
            return block;
        block.leftIndex[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(left);
        block.rightIndex[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(right);
        block.leftWeight[static_cast<std::size_t>(i)] =
            static_cast<std::uint16_t>(256U-wx[outputStart+i]);
        block.rightWeight[static_cast<std::size_t>(i)] =
            wx[outputStart+i];
    }
    block.valid = true;
    return block;
}

inline void blendHorizontalRowScalar(const std::uint8_t* inputRow,
                                    const int* x0, const int* x1,
                                    const std::uint16_t* wx,
                                    std::uint16_t* output, int width) {
    for (int x = 0; x < width; ++x) {
        const unsigned fx = wx[x];
        output[x] = static_cast<std::uint16_t>(
            static_cast<unsigned>(inputRow[x0[x]]) * (256U-fx) +
            static_cast<unsigned>(inputRow[x1[x]]) * fx);
    }
}

#if defined(DVBSTREAMER5_HORIZONTAL_SSSE3_RUNTIME)
__attribute__((target("ssse3")))
inline void blendHorizontalShuffleBlockSsse3(
    const std::uint8_t* inputRow, const HorizontalShuffleBlock& block,
    std::uint16_t* output) {
    const __m128i source = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(inputRow + block.sourceBase));
    const __m128i left = _mm_shuffle_epi8(source, _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(block.leftIndex.data())));
    const __m128i right = _mm_shuffle_epi8(source, _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(block.rightIndex.data())));
    const __m128i zero = _mm_setzero_si128();
    const __m128i lw0 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(block.leftWeight.data()));
    const __m128i lw1 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(block.leftWeight.data()+8));
    const __m128i rw0 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(block.rightWeight.data()));
    const __m128i rw1 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(block.rightWeight.data()+8));
    const __m128i sum0 = _mm_add_epi16(
        _mm_mullo_epi16(_mm_unpacklo_epi8(left, zero), lw0),
        _mm_mullo_epi16(_mm_unpacklo_epi8(right, zero), rw0));
    const __m128i sum1 = _mm_add_epi16(
        _mm_mullo_epi16(_mm_unpackhi_epi8(left, zero), lw1),
        _mm_mullo_epi16(_mm_unpackhi_epi8(right, zero), rw1));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output), sum0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output+8), sum1);
}
#endif

inline bool horizontalSimdAvailable() {
#if defined(DVBSTREAMER5_HORIZONTAL_SSSE3_RUNTIME)
    static const bool hasSsse3 = __builtin_cpu_supports("ssse3");
    return hasSsse3;
#else
    return false;
#endif
}

inline void blendHorizontalRow(
    const std::uint8_t* inputRow, const int* x0, const int* x1,
    const std::uint16_t* wx,
    const std::vector<HorizontalShuffleBlock>& blocks,
    std::uint16_t* output, int width) {
    if (!horizontalSimdAvailable() || blocks.empty()) {
        blendHorizontalRowScalar(inputRow, x0, x1, wx, output, width);
        return;
    }
    int x = 0;
    for (const auto& block : blocks) {
        if (block.valid) {
#if defined(DVBSTREAMER5_HORIZONTAL_SSSE3_RUNTIME)
            blendHorizontalShuffleBlockSsse3(
                inputRow, block, output+x);
#endif
        } else {
            blendHorizontalRowScalar(
                inputRow, x0+x, x1+x, wx+x, output+x, 16);
        }
        x += 16;
    }
    if (x < width)
        blendHorizontalRowScalar(
            inputRow, x0+x, x1+x, wx+x, output+x, width-x);
}

} // namespace dvbstreamer5::media::codec::detail
