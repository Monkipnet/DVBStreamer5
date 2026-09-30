#include "protocols/stream/outputs/StreamRtpOutputProtocol.h"

namespace dvbstreamer5::stream_protocols::outputs {
bool isRtpOutput(const std::string& type) {
    return type == "rtp";
}
}
