#include "protocols/stream/inputs/StreamSrtInputProtocol.h"

namespace dvbstreamer5::stream_protocols::inputs {
bool isSrtInput(const std::string& input, const std::string& mode, bool testPattern) {
    return input.rfind("srt://", 0) == 0;
}
}
