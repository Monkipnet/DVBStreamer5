#pragma once

#include <string>

namespace dvbstreamer5::stream_protocols::outputs {
bool isRtpOutput(const std::string& type);
}
