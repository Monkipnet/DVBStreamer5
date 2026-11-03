#include "media/NativeNvencQualityPolicy.h"

#include <cassert>
#include <iostream>

using dvbstreamer5::media::codec::selectNvencQualityChoice;

int main() {
    // A missing or disabled opt-in must preserve V329's exact RC policy.
    const auto legacy = selectNvencQualityChoice("fast", 1024, 576, 25.0, nullptr);
    assert(!legacy.enabled && legacy.preset == "fast" && legacy.vbvFrames == 2);
    const auto off = selectNvencQualityChoice("fast", 1024, 576, 25.0, "0");
    assert(!off.enabled && off.preset == "fast" && off.vbvFrames == 2);
    const auto invalid = selectNvencQualityChoice("fast", 1024, 576, 25.0, "yes");
    assert(!invalid.enabled && invalid.preset == "fast" && invalid.vbvFrames == 2);

    // Target SD 3.5 Mb/s and HD 6.5 Mb/s retain configured video bitrate;
    // this policy modifies only preset floor and VBV duration.
    const auto sd = selectNvencQualityChoice("fast", 1024, 576, 25.0, "1");
    assert(sd.enabled && sd.preset == "quality" && sd.vbvFrames == 13);
    const auto sdBalanced = selectNvencQualityChoice("balanced", 1280, 720, 25.0, "1");
    assert(sdBalanced.preset == "quality");
    const auto hd = selectNvencQualityChoice("fast", 1920, 1080, 25.0, "1");
    assert(hd.enabled && hd.preset == "balanced" && hd.vbvFrames == 13);
    const auto hdQuality = selectNvencQualityChoice("quality", 1920, 1080, 25.0, "1");
    assert(hdQuality.preset == "quality");
    const auto fiftyFps = selectNvencQualityChoice("balanced", 1920, 1080, 50.0, "1");
    assert(fiftyFps.preset == "balanced" && fiftyFps.vbvFrames == 25);
    const auto invalidFps = selectNvencQualityChoice("fastest", 1024, 576, 0.0, "1");
    assert(invalidFps.enabled && invalidFps.vbvFrames == 13);

    std::cout << "NVENC opt-in policy smoke PASS (no NVIDIA GPU required)\\n";
    return 0;
}
