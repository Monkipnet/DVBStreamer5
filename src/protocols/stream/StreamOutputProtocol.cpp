#include "protocols/stream/StreamOutputProtocol.h"

#include "utils.h"
#include "protocols/stream/outputs/StreamHlsOutputProtocol.h"
#include "protocols/stream/outputs/StreamHttpOutputProtocol.h"
#include "protocols/stream/outputs/StreamRtmpOutputProtocol.h"
#include "protocols/stream/outputs/StreamRtpOutputProtocol.h"
#include "protocols/stream/outputs/StreamRtspOutputProtocol.h"
#include "protocols/stream/outputs/StreamSrtOutputProtocol.h"
#include "protocols/stream/outputs/StreamUdpOutputProtocol.h"

namespace dvbstreamer5::stream_protocols {

OutputProtocolKind outputKind(const StreamConfig& cfg) {
    std::string type = toLower(cfg.outputType);
    if (type == "udp_vbr" || type == "udpvbr") type = "udp-vbr";
    if (type == "udp_cbr" || type == "udpcbr") type = "udp-cbr";
    if (outputs::isUdpOutput(type)) {
        if (type == "udp") return cfg.cbr ? OutputProtocolKind::UdpCbr : OutputProtocolKind::UdpVbr;
        if (type == "udp-cbr") return OutputProtocolKind::UdpCbr;
        return OutputProtocolKind::UdpVbr;
    }
    if (outputs::isRtpOutput(type)) return OutputProtocolKind::Rtp;
    if (outputs::isHttpOutput(type)) return OutputProtocolKind::Http;
    if (outputs::isHlsOutput(type)) return OutputProtocolKind::Hls;
    if (outputs::isSrtOutput(type)) return OutputProtocolKind::Srt;
    if (outputs::isRtspOutput(type)) return OutputProtocolKind::Rtsp;
    if (type == "youtube") return OutputProtocolKind::Youtube;
    if (outputs::isRtmpOutput(type)) return OutputProtocolKind::Rtmp;
    return OutputProtocolKind::Unknown;
}

std::string outputKindName(OutputProtocolKind kind) {
    switch (kind) {
        case OutputProtocolKind::Udp: return "udp";
        case OutputProtocolKind::UdpCbr: return "udp-cbr";
        case OutputProtocolKind::UdpVbr: return "udp-vbr";
        case OutputProtocolKind::Rtp: return "rtp";
        case OutputProtocolKind::Http: return "http";
        case OutputProtocolKind::Hls: return "hls";
        case OutputProtocolKind::Srt: return "srt";
        case OutputProtocolKind::Rtsp: return "rtsp";
        case OutputProtocolKind::Rtmp: return "rtmp";
        case OutputProtocolKind::Youtube: return "youtube";
        default: return "unknown";
    }
}

std::vector<const char*> requiredElementsForOutput(OutputProtocolKind) {
    return {};
}

bool isUdpLikeOutput(OutputProtocolKind kind) {
    return kind == OutputProtocolKind::Udp || kind == OutputProtocolKind::UdpCbr ||
           kind == OutputProtocolKind::UdpVbr || kind == OutputProtocolKind::Rtp;
}

bool isTsOutput(OutputProtocolKind kind) {
    return kind == OutputProtocolKind::Udp || kind == OutputProtocolKind::UdpCbr ||
           kind == OutputProtocolKind::UdpVbr || kind == OutputProtocolKind::Rtp ||
           kind == OutputProtocolKind::Http || kind == OutputProtocolKind::Hls ||
           kind == OutputProtocolKind::Srt;
}

bool isFlvOutput(OutputProtocolKind kind) {
    return kind == OutputProtocolKind::Rtmp || kind == OutputProtocolKind::Youtube;
}

} // namespace dvbstreamer5::stream_protocols
