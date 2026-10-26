#pragma once

#include "media/NativeCodecRuntime.h"

#include <memory>
#include <string>

namespace dvbstreamer5::media::codec {

std::unique_ptr<AudioDecoder> createNativeAc3Decoder(std::string& error);

} // namespace dvbstreamer5::media::codec
