#pragma once

#include <string>

namespace dvbstreamer5::stream_protocols::inputs {
bool isFileInput(const std::string& input, const std::string& mode, bool testPattern);
}
