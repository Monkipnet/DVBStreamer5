#pragma once

#include "media/NativeCodecRuntime.h"

#include <memory>
#include <string>

namespace dvbstreamer5::media::codec {

// Explicit software H.264 decoder for PAFF/MBAFF and field-coded broadcast
// sources. Compiled only when libavcodec/libavutil development files exist;
// the existing project-local decoder backends stay fully operational otherwise.
bool ffmpegAvcDecoderAvailable() noexcept;
std::unique_ptr<VideoDecoder> createFfmpegAvcDecoder(std::string& error);

} // namespace dvbstreamer5::media::codec
