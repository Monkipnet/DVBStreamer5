#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

// V10.8.240: Shared automatic live-encoder tuning for CPU, NVIDIA NVENC,
// Intel QSV/oneVPL and VAAPI. Never infer a manual H.264/HEVC profile/level:
// the selected encoder chooses what its hardware and output raster support.
namespace dvbstreamer5::app {

struct AutoTranscodeSettings {
    std::string preset = "balanced";
    std::string profile = "auto";
    std::string level = "auto";
    std::uint32_t gopFrames = 50;
    std::uint32_t bFrames = 0;
    std::uint32_t refFrames = 0;
    bool openGop = false;
    std::uint32_t aqStrength = 8;
};

inline AutoTranscodeSettings automaticTranscodeSettings(
    double requestedFps,
    std::uint64_t videoBitrate,
    std::uint32_t width,
    std::uint32_t height,
    const std::string& encoder = "auto") {
    AutoTranscodeSettings result;
    const double fps = std::isfinite(requestedFps) && requestedFps >= 1.0 &&
                       requestedFps <= 60.0 ? requestedFps : 25.0;
    result.gopFrames = static_cast<std::uint32_t>(
        std::clamp(std::lround(fps * 2.0), 1L, 1000L));

    // Bits per coded pixel per output frame measures the available quality
    // budget without making a preset depend on hardcoded resolution classes.
    const double pixels = static_cast<double>(std::max(1u, width)) *
                          static_cast<double>(std::max(1u, height));
    const double bpp = static_cast<double>(videoBitrate) / (pixels * fps);
    const bool highThroughput = fps >= 48.0 && pixels >= 1920.0 * 1080.0;
    if (encoder == "cpu" && pixels > 1920.0 * 1080.0 && fps >= 48.0) {
        result.preset = "fastest";
    } else if (highThroughput) {
        // Leave headroom for real-time 50/59.94/60 fps encoding on Intel/NVIDIA.
        result.preset = "fast";
    } else if (bpp < 0.065) {
        // Small bitrate budget: spend more encoder effort per output bit.
        result.preset = "quality";
    } else if (bpp < 0.13) {
        result.preset = "balanced";
    } else {
        result.preset = "fast";
    }
    result.aqStrength = bpp < 0.065 ? 10U : (bpp < 0.13 ? 8U : 6U);
    return result;
}

inline AutoTranscodeSettings automaticTranscodeSettings(
    double fps,
    std::uint64_t videoBitrate,
    const std::string& resolution,
    const std::string& encoder = "auto") {
    unsigned width = 1920;
    unsigned height = 1080;
    // Accept e.g. "720x576_16_9" and preserve the coded dimensions.
    unsigned parsedWidth = 0;
    unsigned parsedHeight = 0;
    if (std::sscanf(resolution.c_str(), "%ux%u", &parsedWidth, &parsedHeight) == 2 &&
        parsedWidth > 0 && parsedHeight > 0) {
        width = parsedWidth;
        height = parsedHeight;
    }
    return automaticTranscodeSettings(fps, videoBitrate, width, height, encoder);
}

} // namespace dvbstreamer5::app
