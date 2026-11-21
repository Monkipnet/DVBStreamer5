#pragma once

#include <cstdint>

namespace dvbstreamer5::media::mpegts {

// H.264 nal_ref_idc is 0 for a non-reference field and 1..3 for a
// reference field. Mixed complementary fields are valid to observe
// but are not handled by every software decoder. Diagnostic only:
// do not split, drop, or rewrite source fields.
inline constexpr bool h264MixedReferenceFields(std::uint8_t first,
                                                std::uint8_t second) noexcept {
    return (first == 0U) != (second == 0U);
}

} // namespace dvbstreamer5::media::mpegts
