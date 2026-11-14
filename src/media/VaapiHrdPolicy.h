#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dvbstreamer5::media::codec {

// V514: opt-in only for a single explicitly selected test tile.
// Never change the VAAPI rate control of unrelated running channels.
inline bool vaapiStrictHrdForStream(
    const std::string& streamId, const char* selectedStreamId) noexcept {
    return selectedStreamId && *selectedStreamId &&
        !streamId.empty() && streamId == selectedStreamId;
}

// A 400-ms encoder CPB is smaller than the native TS mux's 500-ms
// decode preroll. This is a *driver rate-control request*, not a hard
// per-frame size guarantee; peak-frame telemetry must confirm efficacy.
inline std::uint32_t vaapiStrictHrdBufferBits(std::uint32_t bitrate) noexcept {
    const std::uint64_t bits = static_cast<std::uint64_t>(bitrate) * 400ULL / 1000ULL;
    return static_cast<std::uint32_t>(
        (std::min)(bits, static_cast<std::uint64_t>(0xffffffffULL)));
}

inline bool vaapiHrdFrameExceedsBuffer(std::size_t frameBytes,
                                      std::uint32_t bufferBits) noexcept {
    return static_cast<std::uint64_t>(frameBytes) * 8ULL > bufferBits;
}

} // namespace dvbstreamer5::media::codec
