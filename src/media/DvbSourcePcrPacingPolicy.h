#pragma once

#include "media/CbrTsPacer.h"

namespace dvbstreamer5::media::network {

inline bool enableDvbSourcePcrPacketPacing(
    bool enabled, bool dvbInput, bool transcode, bool cbr,
    bool udpCbrOutput, bool testPattern, std::uint64_t encodedTarget) noexcept {
    using mpegts::CbrTsPacer;
    return enabled && dvbInput && !transcode && cbr && udpCbrOutput &&
        !testPattern && CbrTsPacer::sourcePllEncodedTargetBitrate(encodedTarget) &&
        CbrTsPacer::startupBufferMilliseconds(encodedTarget) != 0;
}

} // namespace dvbstreamer5::media::network
