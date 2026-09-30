#pragma once

#include <string>

namespace dvbstreamer5::stream_protocols::outputs {
bool isRtmpOutput(const std::string& type);
}
