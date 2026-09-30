#pragma once

#include "media/NativeCodecRuntime.h"

#include <string>

namespace dvbstreamer5::media::codec {

bool scaleI420(const RawVideoFrame& input, int width, int height,
               RawVideoFrame& output, std::string& error);
void deinterlaceBlendI420(RawVideoFrame& frame);
bool resamplePcm16(const PcmAudioFrame& input, int sampleRate, int channels,
                   PcmAudioFrame& output, std::string& error);

} // namespace dvbstreamer5::media::codec
