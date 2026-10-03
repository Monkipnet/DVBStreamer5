#pragma once

#include "media/NativeCodecRuntime.h"

#include <memory>
#include <string>

namespace dvbstreamer5::media::codec {

// Full AVC/H.264 software decoder fallback based on Ittiam libavc.
// Unlike OpenH264, libavc supports broadcast interlaced/field-coded AVC.
std::unique_ptr<VideoDecoder> createIttiamAvcDecoder(std::string& error);

} // namespace dvbstreamer5::media::codec
