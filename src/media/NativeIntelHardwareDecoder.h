#pragma once

#include "media/NativeCodecRuntime.h"

#include <memory>
#include <string>

namespace dvbstreamer5::media::codec {

// Standalone Intel oneVPL hardware decoder. Unlike IntelZeroCopyTranscoder this
// returns ordinary RawVideoFrame/I420 frames, so the output encoder remains an
// independent per-tile choice (CPU/QSV/VAAPI/NVENC).
std::unique_ptr<VideoDecoder> createIntelHardwareVideoDecoder(
    mpegts::ElementaryCodec codec,
    std::string& error);

bool intelHardwareDecoderRuntimeAvailable() noexcept;

} // namespace dvbstreamer5::media::codec
