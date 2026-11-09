#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace dvbstreamer5::media::network {

// A published mask is immutable. The service remapper updates its private PAT/
// CAT/PMT state in its subscriber thread, and the tuner reader only sees a
// complete snapshot. This avoids a cross-thread remapper read/data race.
struct SharedDvbPidAdmission {
    using Mask = std::array<std::uint8_t, 8192>;
    std::shared_ptr<const Mask> publishedMask;

    void publish(std::shared_ptr<const Mask> next) noexcept {
        std::atomic_store_explicit(
            &publishedMask, std::move(next), std::memory_order_release);
    }
    std::shared_ptr<const Mask> snapshot() const noexcept {
        return std::atomic_load_explicit(
            &publishedMask, std::memory_order_acquire);
    }
};

// Physical DVB demux delivers full MPTS. Only queue the PIDs needed by each
// selected service, instead of storing 8 MiB of unrelated channels per tile.
// The original chunk is retained until a service mask has been published.
// PID 0x12 must always reach the EPG collector before its own CA/remap hook.
// Unexpected packet alignment falls back to the original chunk (never drop).
inline std::shared_ptr<const std::vector<std::uint8_t>>
filterSharedDvbPidChunk(
    const std::shared_ptr<const std::vector<std::uint8_t>>& chunk,
    const std::shared_ptr<SharedDvbPidAdmission>& admission) {
    if (!chunk || !admission) return chunk;
    const auto mask = admission->snapshot();
    constexpr std::size_t packetSize = 188;
    if (!mask || chunk->size() % packetSize != 0) return chunk;

    std::vector<std::uint8_t> selected;
    selected.reserve((std::min)(chunk->size(), std::size_t{16384}));
    for (std::size_t offset = 0; offset < chunk->size(); offset += packetSize) {
        const std::uint8_t* packet = chunk->data() + offset;
        if (packet[0] != 0x47) return chunk;
        const std::uint16_t pid = static_cast<std::uint16_t>(
            ((packet[1] & 0x1fU) << 8U) | packet[2]);
        if (pid == 0x0012U || (*mask)[pid]) {
            selected.insert(selected.end(), packet, packet + packetSize);
        }
    }
    if (selected.size() == chunk->size()) return chunk;
    if (selected.empty()) return {};
    return std::make_shared<const std::vector<std::uint8_t>>(std::move(selected));
}

} // namespace dvbstreamer5::media::network
