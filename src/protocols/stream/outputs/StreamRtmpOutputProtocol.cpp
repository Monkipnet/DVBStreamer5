#include "protocols/stream/outputs/StreamRtmpOutputProtocol.h"

namespace dvbstreamer5::stream_protocols::outputs {
bool isRtmpOutput(const std::string& type) {
    return type == "rtmp" || type == "youtube";
}
}
