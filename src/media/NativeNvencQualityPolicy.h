#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

// V10.8.330: conservative, opt-in NVIDIA encoder A/B tuning.
// This header deliberately has no NVIDIA SDK or GPU dependency; the same
// choice is shared by CPU-fed NVENC and NVDEC->CUDA->NVENC.
// No mode is enabled unless the operator explicitly sets the environment
// variable on a test service before startup.
namespace dvbstreamer5::media::codec {

struct NvencQualityChoice {
    bool enabled = false;
    std::string preset;
    std::uint32_t vbvFrames = 2;
};

inline NvencQualityChoice selectNvencQualityChoice(
    const std::string& requestedPreset,
    int width, int height, double requestedFps,
    const char* qualityEnvironment) {
    NvencQualityChoice choice;
    choice.preset = requestedPreset;
    choice.enabled = qualityEnvironment &&
                     std::strcmp(qualityEnvironment, "1") == 0;
    if (!choice.enabled) return choice;

    // A half-second virtual buffer allows more bits to be allocated across
    // difficult scene transitions than the old two-frame (80 ms @25fps)
    // buffer, without immediately increasing the transport's target CBR.
    // Keep zero-reorder, no lookahead/B frames or extra frame buffers:
    // the live zero-copy path currently requires synchronous NVENC output.
    const double fps = std::isfinite(requestedFps) &&
        requestedFps >= 1.0 && requestedFps <= 60.0 ? requestedFps : 25.0;
    choice.vbvFrames = static_cast<std::uint32_t>(
        std::clamp(std::lround(fps * 0.5), 8L, 30L));

    // The common automatic policy may select "fast" (P2) at 3500 kbps SD.
    // Prioritize compression efficiency when explicitly running this test:
    // SD/720p <=30fps: at least P6; bigger/faster frames: at least P5.
    // Preserve P6/P7 requests and respect the encoder bitrate exactly.
    const bool sd = width > 0 && height > 0 &&
        width <= 1280 && height <= 720 && fps <= 30.0;
    const std::string& p = requestedPreset;
    const bool alreadyHigh = p == "quality" || p == "best" ||
                             p == "p6" || p == "p7";
    const bool alreadyBalanced = p == "balanced" ||
                                 p == "p4" || p == "p5";
    if (sd) {
        if (!alreadyHigh) choice.preset = "quality";
    } else if (!alreadyHigh && !alreadyBalanced) {
        choice.preset = "balanced";
    }
    return choice;
}

} // namespace dvbstreamer5::media::codec
