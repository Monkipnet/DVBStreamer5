#pragma once
#include "media/AvcDecoderDiagnostic.h"
#include <string>

namespace dvbstreamer5::media::codec {

// Explicit tile setting takes precedence; legacy diagnostic env only applies
// when the saved setting is Auto. The rest of the application is unchanged.
inline std::string chooseAvcDecoder(const std::string& requested,
                                    const std::string& streamId,
                                    const char* diagnosticStream) {
    if (requested == "openh264" || requested == "ittiam" || requested == "ffmpeg" || requested == "intel_hw" || requested == "nvidia_hw") return requested;
    return forceOpenH264ForStream(diagnosticStream, streamId)
        ? "openh264" : "auto";
}

} // namespace dvbstreamer5::media::codec
