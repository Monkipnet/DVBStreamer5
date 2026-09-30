#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TranscoderCapabilities {
    bool available = false;
    std::string videoEncoder;
    bool x264Available = false;
    bool nvencAvailable = false;
    bool intelAvailable = false;
    std::string intelEncoder;
    std::string hevcVideoEncoder;
    bool x265Available = false;
    bool nvencHevcAvailable = false;
    bool intelHevcAvailable = false;
    std::string intelHevcEncoder;
    std::string audioEncoder;
    std::string aacEncoder;
    std::string mp3Encoder;
    bool mp2EncoderAvailable = true;
    bool deinterlaceAvailable = false;
    std::vector<std::string> missingElements;
    std::string message;
};

class TranscoderModule {
public:
    static TranscoderCapabilities inspectCapabilities();
    static bool resolutionSize(const std::string& resolution, int& width, int& height);
    static uint64_t recommendedVideoBitrate(const std::string& resolution);
};
