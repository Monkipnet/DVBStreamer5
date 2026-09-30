#pragma once

#include "ConfigManager.h"

#include <string>
#include <vector>

namespace dvbstreamer5::protocols {

std::string inputUriForGstreamer(const StreamConfig& cfg);
void appendDecodeInput(std::vector<std::string>& args, const StreamConfig& cfg);
std::vector<std::string> requiredInputElements();

} // namespace dvbstreamer5::protocols
