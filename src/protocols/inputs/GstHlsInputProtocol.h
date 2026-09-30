#pragma once
#include "ConfigManager.h"
#include <string>
namespace dvbstreamer5::protocols::inputs {
bool isHlsInput(const StreamConfig& cfg);
std::string hlsInputUri(const StreamConfig& cfg);
}
