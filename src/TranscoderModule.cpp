#include "TranscoderModule.h"
#include "media/NativeCodecRuntime.h"

#include <algorithm>
#include <cctype>
#include <regex>

TranscoderCapabilities TranscoderModule::inspectCapabilities() {
    TranscoderCapabilities result;
    const auto native = dvbstreamer5::media::codec::inspectRuntimeCapabilities();
    result.x264Available = native.h264Encoder;
    result.x265Available = native.hevcEncoder;
    result.nvencAvailable = native.nvencH264Encoder;
    result.nvencHevcAvailable = native.nvencHevcEncoder;
    result.intelAvailable = native.qsvH264Encoder;
    result.intelHevcAvailable = native.qsvHevcEncoder;
    result.intelEncoder = native.intelHardwareLibrary;
    result.intelHevcEncoder = native.intelHardwareLibrary;
    result.videoEncoder = native.h264Encoder ? "OpenH264 CPU" : std::string{};
    result.hevcVideoEncoder = native.hevcEncoder ? "Kvazaar CPU" : std::string{};
    if (native.nvencH264Encoder) result.videoEncoder += result.videoEncoder.empty() ? "NVENC" : " + NVENC";
    if (native.qsvH264Encoder) result.videoEncoder += result.videoEncoder.empty() ? "QSV/VAAPI" : " + QSV/VAAPI";
    if (native.nvencHevcEncoder) result.hevcVideoEncoder += result.hevcVideoEncoder.empty() ? "NVENC" : " + NVENC";
    if (native.qsvHevcEncoder) result.hevcVideoEncoder += result.hevcVideoEncoder.empty() ? "QSV/VAAPI" : " + QSV/VAAPI";
    result.aacEncoder = native.aacEncoder ? "native AAC encoder" : std::string{};
    result.audioEncoder = result.aacEncoder;
    result.mp2EncoderAvailable = true;
    result.deinterlaceAvailable = true;

    if (!native.h264Decoder) result.missingElements.push_back("h264-decoder");
    if (!native.h264Encoder) result.missingElements.push_back("h264-encoder");
    if (!native.hevcDecoder) result.missingElements.push_back("hevc-decoder");
    if (!native.hevcEncoder) result.missingElements.push_back("hevc-encoder");
    if (!native.aacDecoder) result.missingElements.push_back("aac-decoder");
    if (!native.aacEncoder) result.missingElements.push_back("aac-encoder");
    if (!native.mpegAudioDecoder) result.missingElements.push_back("mpeg-audio-decoder");

    // Copy-mode and MP2 output remain usable even when not every optional codec
    // backend is present. Full native transcode capability means H.264/HEVC
    // decode+encode plus AAC decode+encode are all available.
    result.available = native.h264Decoder && native.h264Encoder &&
                       native.hevcDecoder && native.hevcEncoder &&
                       native.aacDecoder && native.aacEncoder;
    result.message = result.available
        ? "native video/audio transcoder available; direct CPU/NVENC/QSV-VAAPI backends; no GStreamer/FFmpeg/libav media framework"
        : "native transcoder core active; one or more codec backends are unavailable";
    return result;
}

bool TranscoderModule::resolutionSize(const std::string& resolution, int& width, int& height) {
    // Accept the UI's optional anamorphic/aspect suffix (for example
    // 720x576_16_9) while keeping coded dimensions independent from DAR.
    static const std::regex pattern(
        R"(^\s*([0-9]{2,5})x([0-9]{2,5})(?:_([0-9]{1,3})_([0-9]{1,3}))?\s*$)",
        std::regex::icase);
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
