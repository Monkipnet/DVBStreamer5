#pragma once

#include "media/NativeCodecRuntime.h"
#include "media/NativeMpegTsMux.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace dvbstreamer5::media::testpattern {

struct NativeTestPatternConfig {
    int width = 1280;
    int height = 720;
    double fps = 25.0;
    std::uint64_t videoBitrate = 2500000;
    std::uint64_t audioBitrate = 128000;
    std::uint64_t muxBitrate = 0;
    std::uint16_t serviceId = 1;
    std::uint16_t videoPid = 0x0100;
    std::uint16_t audioPid = 0x0101;
    std::string serviceName = "DVBStreamer5 Test";
    std::string serviceProvider = "DVBStreamer5";
};

class NativeTestPatternSource {
public:
    using Sink = std::function<bool(const std::uint8_t*, std::size_t)>;

    bool run(const NativeTestPatternConfig& config,
             const Sink& sink,
             std::atomic<bool>& stop,
             std::string& error);

private:
    static void fillBars(codec::RawVideoFrame& frame, std::uint64_t frameIndex);
};

} // namespace dvbstreamer5::media::testpattern
