#include "protocols/stream/inputs/StreamHlsInputProtocol.h"

namespace dvbstreamer5::stream_protocols::inputs {
bool isHlsInput(const std::string& input, const std::string& mode, bool testPattern) {
    return input.rfind("hls://", 0) == 0 || mode == "hls" || input.find(".m3u8") != std::string::npos;
}
}
