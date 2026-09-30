#pragma once
#include "ConfigManager.h"
#include <string>
namespace dvbstreamer5::protocols::inputs {
bool isHttpInput(const StreamConfig& cfg);
std::string httpInputUri(const StreamConfig& cfg);
}
