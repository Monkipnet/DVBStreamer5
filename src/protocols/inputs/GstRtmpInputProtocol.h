#pragma once
#include "ConfigManager.h"
#include <string>
namespace dvbstreamer5::protocols::inputs {
bool isRtmpInput(const StreamConfig& cfg);
std::string rtmpInputUri(const StreamConfig& cfg);
}
