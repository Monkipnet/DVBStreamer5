#include "protocols/stream/outputs/StreamRtspOutputProtocol.h"

namespace dvbstreamer5::stream_protocols::outputs {
bool isRtspOutput(const std::string& type) {
    return type == "rtsp";
}
}
