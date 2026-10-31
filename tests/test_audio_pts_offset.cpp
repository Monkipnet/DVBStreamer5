// Build: g++ -std=c++17 -Isrc tests/test_audio_pts_offset.cpp -o /tmp/test_audio_pts_offset
#include "media/AudioPtsOffset.h"
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using dvbstreamer5::media::codec::shiftAudioPts90k;
    constexpr std::uint64_t modulus = 1ULL << 33U;
    constexpr std::uint64_t mask = modulus - 1ULL;

    assert(shiftAudioPts90k(90000ULL, 0) == 90000ULL);
    assert(shiftAudioPts90k(90000ULL, 100) == 99000ULL);
    assert(shiftAudioPts90k(90000ULL, -100) == 81000ULL);
    assert(shiftAudioPts90k(90000ULL, 500) == 135000ULL);
    assert(shiftAudioPts90k(90000ULL, -500) == 45000ULL);
    assert(shiftAudioPts90k(90000ULL, 9999) == 135000ULL);
    assert(shiftAudioPts90k(90000ULL, -9999) == 45000ULL);
    assert(shiftAudioPts90k(mask - 4499ULL, 100) ==
           ((mask - 4499ULL + 9000ULL) & mask));
    assert(shiftAudioPts90k(3000ULL, -100) ==
           ((3000ULL + modulus - 9000ULL) & mask));

    std::cout << "PASS per-stream audio PTS adjustment\n";
}
