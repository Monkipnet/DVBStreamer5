#pragma once
#include "ConfigManager.h"
#include <string>
namespace dvbstreamer5::protocols::inputs {
bool isSrtInput(const StreamConfig& cfg);
std::string srtInputUri(const StreamConfig& cfg);
}
