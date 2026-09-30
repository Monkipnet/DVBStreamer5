#include "protocols/stream/inputs/StreamUdpInputProtocol.h"

namespace dvbstreamer5::stream_protocols::inputs {
bool isUdpInput(const std::string& input, const std::string& mode, bool testPattern) {
    return input.rfind("udp://", 0) == 0;
}
}
