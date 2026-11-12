#include "media/NativeVideoProcessing.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

int main() {
    using namespace dvbstreamer5::media::codec;
    RawVideoFrame source;
    source.width = 6;
    source.height = 4;
    source.pts90k = 90000;
    source.dts90k = 86400;
    source.hasPts = source.hasDts = true;
    source.keyFrame = true;
    source.i420.resize(6U * 4U * 3U / 2U);
    for (std::size_t i = 0; i < source.i420.size(); ++i)
        source.i420[i] = static_cast<std::uint8_t>((i * 53U) & 255U);

    RawVideoFrame scaled;
    std::string error;
    assert(scaleI420(source, 32, 24, scaled, error));
    const auto capacity = scaled.i420.capacity();
    const auto* pixels = scaled.i420.data();
    assert(capacity >= 32U * 24U * 3U / 2U);
    scaled.interlaced = true;
    scaled.topFieldFirst = false;

    for (auto& pixel : source.i420) pixel ^= 0x55;
    source.pts90k = 93600;
    source.dts90k = 90000;
    source.keyFrame = false;

    assert(scaleI420(source, 32, 24, scaled, error));
    assert(scaled.i420.data() == pixels);
    assert(scaled.i420.capacity() == capacity);
    assert(!scaled.interlaced && scaled.topFieldFirst);
    assert(scaled.width == 32 && scaled.height == 24);
    assert(scaled.pts90k == source.pts90k &&
           scaled.dts90k == source.dts90k && scaled.hasPts &&
           scaled.hasDts && !scaled.keyFrame);

    RawVideoFrame fresh;
    assert(scaleI420(source, 32, 24, fresh, error));
    assert(scaled.i420 == fresh.i420);

    std::cout << "PASS: I420 scaler reuses allocation with byte-exact pixels\n";
}
