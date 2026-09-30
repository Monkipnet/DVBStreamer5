#pragma once

#include "protocols/GstOutputProtocols.h"

namespace dvbstreamer5::protocols::outputs {
bool appendFifoSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec);
}
