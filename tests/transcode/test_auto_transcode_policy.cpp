#include "AutoTranscodePolicy.h"

#include <cassert>
#include <cmath>
#include <iostream>

using dvbstreamer5::app::automaticTranscodeSettings;

int main() {
    const auto hd25 = automaticTranscodeSettings(25.0, 6000000, "1920x1080", "nvenc");
    assert(hd25.gopFrames == 50);
    assert(hd25.preset == "balanced");
    assert(hd25.profile == "auto" && hd25.level == "auto");
    assert(hd25.bFrames == 0 && hd25.refFrames == 0 && !hd25.openGop);

    const auto lowBitrate = automaticTranscodeSettings(25.0, 2500000, "1920x1080", "qsv");
    assert(lowBitrate.preset == "quality");
    assert(lowBitrate.aqStrength == 10);
    assert(lowBitrate.profile == "auto");

    const auto hd50 = automaticTranscodeSettings(50.0, 6000000, "1920x1080", "vaapi");
    assert(hd50.gopFrames == 100);
    assert(hd50.preset == "fast");
    assert(hd50.profile == "auto");

    const auto uhdCpu = automaticTranscodeSettings(60.0, 12000000, "3840x2160", "cpu");
    assert(uhdCpu.preset == "fastest" && uhdCpu.gopFrames == 120);
    const auto uhdIntel = automaticTranscodeSettings(60.0, 12000000, "3840x2160", "qsv");
    assert(uhdIntel.preset == "fast" && uhdIntel.profile == "auto");

    const auto fractionalFps = automaticTranscodeSettings(29.97, 1500000, "720x576_16_9");
    assert(fractionalFps.gopFrames == 60);
    const auto invalidFps = automaticTranscodeSettings(NAN, 2500000, "1280x720");
    assert(invalidFps.gopFrames == 50);

    std::cout << "V10.8.240 CPU / NVENC / Intel auto-transcode policy PASS\n";
    return 0;
}
