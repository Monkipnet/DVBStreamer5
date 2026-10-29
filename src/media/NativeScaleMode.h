#pragma once
// Diagnostic A/B: bypass VAAPI VPP and use the native CPU I420 scaler.
// The default remains unchanged; the override is opt-in per service process.
#include <cstdlib>

namespace dvbstreamer5::media::codec {
inline bool forceCpuVideoScaling() noexcept {
    const char* value = std::getenv("DVBSTREAMER5_FORCE_CPU_SCALE");
    return value && value[0] == '1' && value[1] == '\0';
}
} // namespace dvbstreamer5::media::codec
