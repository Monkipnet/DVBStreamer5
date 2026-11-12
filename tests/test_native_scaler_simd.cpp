// V10.8.486: hardware-free, exact-pixel regression for optimized I420 scaler.
#include "media/NativeScalerHorizontal.h"
#include "media/NativeScalerVertical.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

int main() {
    using namespace dvbstreamer5::media::codec::detail;
    std::mt19937 rng(785421);

    // Important broadcast conversion: 1080p -> 1440p, plus PAL upscale,
    // downscaling, tiny frames and non-vector tail boundaries.
    constexpr std::array<std::array<int, 2>, 12> geometries{{
        {{1920, 2560}}, {{1080, 1440}}, {{960, 1920}},
        {{720, 1280}}, {{720, 576}}, {{1920, 1280}},
        {{32, 128}}, {{31, 30}}, {{1, 100}},
        {{12, 16}}, {{1920, 1920}}, {{720, 1920}}
    }};

    for (const auto& geometry : geometries) {
        const int sourceWidth = geometry[0];
        const int width = geometry[1];
        std::vector<int> x0(width), x1(width);
        std::vector<std::uint16_t> wx(width);
        for (int x = 0; x < width; ++x) {
            const std::uint64_t fp = width > 1
                ? static_cast<std::uint64_t>(x) *
                    static_cast<std::uint64_t>(sourceWidth - 1) *
                    256ULL / static_cast<std::uint64_t>(width - 1)
                : 0ULL;
            x0[x] = static_cast<int>(fp >> 8U);
            x1[x] = std::min(x0[x] + 1, sourceWidth - 1);
            wx[x] = static_cast<std::uint16_t>(fp & 0xffU);
        }
        std::vector<HorizontalShuffleBlock> blocks;
        for (int x = 0; x + 16 <= width; x += 16)
            blocks.push_back(prepareHorizontalShuffleBlock(
                x0.data(), x1.data(), wx.data(), sourceWidth, x));
        std::vector<std::uint8_t> source(sourceWidth);
        std::vector<std::uint16_t> expected(width), accelerated(width);
        for (int trial = 0; trial < 5; ++trial) {
            for (auto& sample : source)
                sample = static_cast<std::uint8_t>(rng() & 255U);
            blendHorizontalRowScalar(
                source.data(), x0.data(), x1.data(),
                wx.data(), expected.data(), width);
            blendHorizontalRow(
                source.data(), x0.data(), x1.data(),
                wx.data(), blocks, accelerated.data(), width);
            if (expected != accelerated) {
                std::cerr << "FAILED horizontal " << sourceWidth
                          << " -> " << width << '\n';
                return 1;
            }
        }
    }

    const int widths[] = {1, 2, 3, 4, 7, 8, 9, 15, 16, 17,
                          127, 128, 129, 640, 1920, 2560};
    for (int width : widths) {
        std::vector<std::uint16_t> top(width), bottom(width);
        std::vector<std::uint8_t> expected(width), accelerated(width);
        for (int trial = 0; trial < 5; ++trial) {
            for (int x = 0; x < width; ++x) {
                // Horizontal bilinear intermediate is always <= 255*256.
                top[x] = static_cast<std::uint16_t>(rng() % 65281U);
                bottom[x] = static_cast<std::uint16_t>(rng() % 65281U);
            }
            for (unsigned fy = 0; fy <= 255U; ++fy) {
                blendVerticalRowScalar(
                    top.data(), bottom.data(),
                    expected.data(), width, fy);
                blendVerticalRow(
                    top.data(), bottom.data(),
                    accelerated.data(), width, fy);
                if (expected != accelerated) {
                    std::cerr << "FAILED vertical width=" << width
                              << " fy=" << fy << '\n';
                    return 1;
                }
            }
        }
    }
    std::cout << "PASS: scalar and CPU-SIMD I420 scaler produce exactly"
                 " the same pixels for source/output and edge cases\n";
    return 0;
}
