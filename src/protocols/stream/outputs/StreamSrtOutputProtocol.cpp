#include "protocols/stream/outputs/StreamSrtOutputProtocol.h"

namespace dvbstreamer5::stream_protocols::outputs {
bool isSrtOutput(const std::string& type) {
    return type == "srt";
}
}
