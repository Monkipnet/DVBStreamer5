// Build: g++ -std=c++17 -Isrc tests/test_hls_segment_settings.cpp -o /tmp/test_hls_segment_settings
#include "media/HlsSegmentSettings.h"
#include <cassert>
#include <iostream>

using dvbstreamer5::media::hls::resolveSegmentSettings;

int main() {
    const auto tsAuto = resolveSegmentSettings(0, 0, false);
    const auto cmafAuto = resolveSegmentSettings(0, 0, true);
    assert(tsAuto.durationSeconds == 4.0 && tsAuto.count == 8);
    assert(cmafAuto.durationSeconds == 2.0 && cmafAuto.count == 6);
    assert(!tsAuto.customDuration && !tsAuto.customCount);

    const auto explicitTs = resolveSegmentSettings(2, 4, false);
    const auto explicitCmaf = resolveSegmentSettings(2, 4, true);
    assert(explicitTs.durationSeconds == 2.0 && explicitTs.count == 4);
    assert(explicitCmaf.durationSeconds == 2.0 && explicitCmaf.count == 4);
    assert(explicitTs.customDuration && explicitTs.customCount);

    const auto durationOnly = resolveSegmentSettings(3, 0, false);
    assert(durationOnly.durationSeconds == 3.0 && durationOnly.count == 8);
    assert(durationOnly.customDuration && !durationOnly.customCount);

    const auto countOnly = resolveSegmentSettings(0, 12, true);
    assert(countOnly.durationSeconds == 2.0 && countOnly.count == 12);
    assert(!countOnly.customDuration && countOnly.customCount);

    const auto bounded = resolveSegmentSettings(100, 100, false);
    assert(bounded.durationSeconds == 10.0 && bounded.count == 30);
    assert(resolveSegmentSettings(1, 1, false).count == 3);

    std::cout << "HLS segment settings tests passed\n";
}
