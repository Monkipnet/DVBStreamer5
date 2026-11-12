#include "media/NativeI420Nv12.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

int main() {
    using dvbstreamer5::media::codec::detail::copyI420ToNv12;
    std::mt19937 random(491);
    for (int w : {2, 4, 6, 16, 30, 32, 34, 70, 1920, 2560}) {
        for (int h : {2, 4, 8, 18}) {
            for (int padding : {0, 1, 16, 63}) {
                const int yPitch = w + padding;
                const int uvPitch = w + padding + 3;
                std::vector<std::uint8_t> src(w * h * 3 / 2);
                for (auto& pixel : src)
                    pixel = static_cast<std::uint8_t>(random() & 255U);
                const std::uint8_t* y = src.data();
                const std::uint8_t* u = y + w * h;
                const std::uint8_t* v = u + w * h / 4;
                std::vector<std::uint8_t> outY(yPitch * h + 32, 0xa5);
                std::vector<std::uint8_t> outUV(uvPitch * h / 2 + 32, 0xa5);
                copyI420ToNv12(y, u, v, w, h, outY.data(), yPitch,
                               outUV.data(), uvPitch);
                for (int row = 0; row < h; ++row) {
                    for (int x = 0; x < w; ++x)
                        assert(outY[row * yPitch + x] == y[row * w + x]);
                    for (int x = w; x < yPitch; ++x)
                        assert(outY[row * yPitch + x] == 0xa5);
                }
                for (int row = 0; row < h / 2; ++row) {
                    for (int x = 0; x < w / 2; ++x) {
                        assert(outUV[row * uvPitch + 2 * x] == u[row * (w / 2) + x]);
                        assert(outUV[row * uvPitch + 2 * x + 1] == v[row * (w / 2) + x]);
                    }
                    for (int x = w; x < uvPitch; ++x)
                        assert(outUV[row * uvPitch + x] == 0xa5);
                }
                for (std::size_t x = static_cast<std::size_t>(yPitch) * h;
                     x < outY.size(); ++x)
                    assert(outY[x] == 0xa5);
            }
        }
    }
    std::cout << "PASS: SIMD I420->NV12 matches scalar, padding/tails\n";
}
