#pragma once

#include "ConfigManager.h"
#include <string>

namespace dvbstreamer5::protocols::inputs {

bool isRtpInput(const StreamConfig& cfg);
std::string rtpInputUri(const StreamConfig& cfg);

} // namespace dvbstreamer5::protocols::inputs
