#pragma once
// Opt-in A/B decoder selection, scoped to an exact stream ID.
// Missing/empty or mismatched ID always keeps the existing Ittiam path.
#include <string>

namespace dvbstreamer5::media::codec {
inline bool forceOpenH264ForStream(const char* selectedId,
                                  const std::string& streamId) noexcept {
    return selectedId && *selectedId && !streamId.empty() &&
           streamId == selectedId;
}
} // namespace dvbstreamer5::media::codec
