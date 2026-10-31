#pragma once
// Software-decoded I420 frames use the native CPU scaler by default.
// VAAPI VPP can be explicitly enabled for A/B testing; dedicated Intel/NVIDIA
// zero-copy pipelines remain hardware accelerated and are unaffected.
#include <cstdlib>

namespace dvbstreamer5::media::codec {
inline bool forceCpuVideoScaling() noexcept {
    const char* value = std::getenv("DVBSTREAMER5_FORCE_CPU_SCALE");
    return value && value[0] == '1' && value[1] == '\0';
}
inline bool enableVaapiSoftwareScaling() noexcept {
    if (forceCpuVideoScaling()) return false;
    const char* value = std::getenv("DVBSTREAMER5_ENABLE_VAAPI_SOFTWARE_SCALE");
    return value && value[0] == '1' && value[1] == '\0';
}
} // namespace dvbstreamer5::media::codec
