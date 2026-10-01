#pragma once

#include "media/NativeCodecRuntime.h"

#include <memory>
#include <string>

namespace dvbstreamer5::media::codec {

struct NativeHardwareCapabilities {
    bool vaapiRuntime = false;
    bool qsvAvailable = false;
    bool qsvH264 = false;
    bool qsvHevc = false;
    bool nvencAvailable = false;
    bool nvencH264 = false;
    bool nvencHevc = false;
    std::string intelBackend;
    std::string nvencBackend;
};

NativeHardwareCapabilities inspectNativeHardwareCapabilities();
std::unique_ptr<VideoEncoder> createNativeHardwareVideoEncoder(
    mpegts::ElementaryCodec codec,
    const std::string& backend,
    std::string& error);

} // namespace dvbstreamer5::media::codec
