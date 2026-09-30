#include "TranscoderModule.h"

#include <algorithm>
#include <cctype>
#include <regex>

TranscoderCapabilities TranscoderModule::inspectCapabilities() {
    TranscoderCapabilities result;
    result.available = false;
    result.mp2EncoderAvailable = true;
    result.missingElements = {
        "native-video-decoder",
        "native-video-scaler",
        "native-video-encoder",
        "native-audio-decoder",
        "native-aac-encoder"
    };
    result.message =
        "The legacy external media framework has been removed completely. Native MPEG-TS/UDP/RTP/DVB/HTTP transport is active; "
        "video/audio transcoding remains disabled until native codecs are integrated.";
    return result;
}

bool TranscoderModule::resolutionSize(const std::string& resolution, int& width, int& height) {
    static const std::regex pattern(R"(^\s*([0-9]{2,5})x([0-9]{2,5})\s*$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(resolution, match, pattern)) return false;
    try {
        width = std::stoi(match[1].str());
        height = std::stoi(match[2].str());
    } catch (...) {
        return false;
    }
    return width > 0 && height > 0;
}

uint64_t TranscoderModule::recommendedVideoBitrate(const std::string& resolution) {
    int width = 0, height = 0;
    if (!resolutionSize(resolution, width, height)) return 6000000;
    const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixels <= 640ull * 360ull) return 1000000;
    if (pixels <= 854ull * 480ull) return 1800000;
    if (pixels <= 1280ull * 720ull) return 3500000;
    if (pixels <= 1920ull * 1080ull) return 6000000;
    return 12000000;
}
