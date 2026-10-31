// Build: g++ -std=c++17 -Isrc tests/test_ffmpeg_pal_pts_clock.cpp -o /tmp/test_ffmpeg_pal_pts_clock
#include "media/FfmpegPalPtsClock.h"
#include <cassert>
#include <cstdint>
#include <iostream>

using dvbstreamer5::media::codec::FfmpegPalPtsClock;

int main() {
    constexpr std::uint64_t mask = FfmpegPalPtsClock::kMask;
    constexpr std::uint64_t step = FfmpegPalPtsClock::kFrameTicks;
    constexpr std::uint64_t base = 8355694736ULL;

    FfmpegPalPtsClock pal;
    std::uint64_t last = 0;
    bool everLocked = false;
    for (std::uint64_t i = 0; i < 750; ++i) {
        // A 25-fps broadcaster with periodic reordered/misassigned field
        // PTS. Total average cadence is 25 fps; the generic gate previously
        // dropped one in five frames after the bad timestamp.
        const auto jitter = (i % 5 == 1) ? -1800LL : 0LL;
        const auto pts = static_cast<std::uint64_t>(
            static_cast<std::int64_t>((base + i * step) & mask) + jitter) & mask;
        auto r = pal.apply(pts);
        if (r.corrected) {
            everLocked = true;
            assert(r.pts90k == ((base + i * step) & mask));
            if (last != 0)
                assert(((r.pts90k - last) & mask) == step);
            last = r.pts90k;
        }
    }
    assert(everLocked);

    // True 30 fps must NOT be transformed to 25 fps: the regular output
    // FPS gate still needs to handle this case.
    FfmpegPalPtsClock ntsc;
    for (unsigned i = 0; i < 180; ++i) {
        auto r = ntsc.apply(base + i * 3000ULL);
        assert(!r.corrected);
    }

    // 33-bit PTS wrap must not look like a stream discontinuity.
    FfmpegPalPtsClock wrapped;
    bool wrapLocked = false;
    for (std::uint64_t i = 0; i < 120; ++i) {
        auto r = wrapped.apply((mask - 36000ULL + i * step) & mask);
        if (r.corrected) wrapLocked = true;
        assert(!r.discontinuity);
    }
    assert(wrapLocked);

    // A real clock jump reanchors rather than dragging a stale offset.
    auto jump = pal.apply(base + 750 * step + 180000ULL);
    assert(jump.discontinuity);
    assert(!jump.corrected);
    auto next = pal.apply(base + 750 * step + 180000ULL + step);
    assert(!next.discontinuity);

    std::cout << "PASS FFmpeg PAL interlaced PTS cadence and discontinuity\n";
    return 0;
}
