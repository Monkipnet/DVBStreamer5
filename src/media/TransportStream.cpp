#include "media/TransportStream.h"

#include <algorithm>
#include <cstring>

namespace dvbstreamer5::media::mpegts {

bool inspectPacket(const std::uint8_t* data, std::size_t size, PacketInfo& info) noexcept {
    if (!data || size < kPacketSize || data[0] != kSyncByte) {
        return false;
    }

    PacketInfo parsed;
    parsed.transportError = (data[1] & 0x80U) != 0;
    parsed.payloadUnitStart = (data[1] & 0x40U) != 0;
    parsed.pid = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[1] & 0x1fU) << 8) | data[2]);
    parsed.scrambled = (data[3] & 0xc0U) != 0;
    parsed.adaptationFieldControl = static_cast<std::uint8_t>((data[3] >> 4) & 0x03U);
    parsed.continuityCounter = static_cast<std::uint8_t>(data[3] & 0x0fU);
    if (parsed.adaptationFieldControl == 0) {
        return false;
    }

    parsed.hasAdaptationField = (parsed.adaptationFieldControl & 0x02U) != 0;
    parsed.hasPayload = (parsed.adaptationFieldControl & 0x01U) != 0;
    parsed.payloadOffset = 4;
    if (parsed.hasAdaptationField) {
        const std::size_t adaptationLength = data[4];
        parsed.payloadOffset = 5 + adaptationLength;
        if (parsed.payloadOffset > kPacketSize ||
            (!parsed.hasPayload && parsed.payloadOffset != kPacketSize) ||
            (parsed.hasPayload && parsed.payloadOffset == kPacketSize)) {
            return false;
        }

        if (adaptationLength > 0) {
            const std::uint8_t flags = data[5];
            parsed.discontinuity = (flags & 0x80U) != 0;
            parsed.randomAccess = (flags & 0x40U) != 0;
            parsed.hasPcr = (flags & 0x10U) != 0;
            if (parsed.hasPcr) {
                if (adaptationLength < 7) {
                    return false;
                }
                parsed.pcrBase90k =
                    (static_cast<std::uint64_t>(data[6]) << 25) |
                    (static_cast<std::uint64_t>(data[7]) << 17) |
                    (static_cast<std::uint64_t>(data[8]) << 9) |
                    (static_cast<std::uint64_t>(data[9]) << 1) |
                    (static_cast<std::uint64_t>(data[10]) >> 7);
            }
        }
    }

    info = parsed;
    return true;
}

bool rewritePid(std::uint8_t* data, std::size_t size, std::uint16_t pid) noexcept {
    PacketInfo info;
    if (pid > kNullPid || !inspectPacket(data, size, info)) {
        return false;
    }
    data[1] = static_cast<std::uint8_t>((data[1] & 0xe0U) | ((pid >> 8) & 0x1fU));
    data[2] = static_cast<std::uint8_t>(pid & 0xffU);
    return true;
}

void PacketFramer::push(
    const std::uint8_t* data, std::size_t size, std::vector<Packet>& packets) {
    if (!data || size == 0) {
        return;
    }

    // V10.8.122: most native paths already deliver MPEG-TS on a 188-byte
    // packet grid. Reserve the output once and consume that aligned prefix
    // directly instead of first copying the whole chunk into pending_ and then
    // shifting it back out again. The exact legacy byte-resync path below is
    // retained for partial, corrupt or arbitrarily aligned input.
    const std::size_t estimatedPackets = (pending_.size() + size) / kPacketSize;
    if (estimatedPackets > 0 &&
        estimatedPackets <= packets.max_size() - packets.size()) {
        packets.reserve(packets.size() + estimatedPackets);
    }

    if (pending_.empty()) {
        std::size_t direct = 0;
        while (size - direct >= kPacketSize) {
            if (data[direct] != kSyncByte) {
                break;
            }
            const std::size_t remaining = size - direct;
            if (remaining >= 2 * kPacketSize &&
                data[direct + kPacketSize] != kSyncByte) {
                break;
            }
            if (remaining >= 3 * kPacketSize &&
                data[direct + 2 * kPacketSize] != kSyncByte) {
                break;
            }
            PacketInfo candidateInfo;
            if (!inspectPacket(data + direct, kPacketSize, candidateInfo)) {
                break;
            }

            // V10.8.124: construct the destination packet directly in the
            // vector. The previous temporary Packet + push_back(packet) copied
            // every 188-byte TS packet twice on this hot aligned-input path.
            packets.emplace_back();
            std::memcpy(packets.back().data(), data + direct, kPacketSize);
            direct += kPacketSize;
        }

        if (direct == size) {
            return;
        }
        if (direct > 0) {
            data += direct;
            size -= direct;
        }
    }

    pending_.insert(pending_.end(), data, data + size);

    std::size_t consumed = 0;
    while (pending_.size() - consumed >= kPacketSize) {
        std::size_t candidate = consumed;
        for (; candidate + kPacketSize <= pending_.size(); ++candidate) {
            if (pending_[candidate] != kSyncByte) {
                continue;
            }
            const std::size_t remaining = pending_.size() - candidate;
            if (remaining >= 2 * kPacketSize &&
                pending_[candidate + kPacketSize] != kSyncByte) {
                continue;
            }
            if (remaining >= 3 * kPacketSize &&
                pending_[candidate + 2 * kPacketSize] != kSyncByte) {
                continue;
            }

            // A stray 0x47 byte inside payload can satisfy the sync-spacing
            // heuristic, especially when two HTTP live connections meet at
            // an arbitrary byte boundary after reconnect.  Never emit a
            // candidate unless its MPEG-TS header/adaptation field is itself
            // structurally valid.  Invalid candidates are skipped one byte at
            // a time until a genuine packet boundary is found.
            PacketInfo candidateInfo;
            if (!inspectPacket(
                    pending_.data() + candidate, kPacketSize, candidateInfo)) {
                continue;
            }
            break;
        }

        if (candidate + kPacketSize > pending_.size()) {
            consumed = pending_.size() > kPacketSize - 1
                ? pending_.size() - (kPacketSize - 1)
                : 0;
            break;
        }

        // Same single-copy rule for the byte-resync fallback path.
        packets.emplace_back();
        std::memcpy(
            packets.back().data(), pending_.data() + candidate, kPacketSize);
        consumed = candidate + kPacketSize;
    }

    if (consumed > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
}

void PacketFramer::pushTrustedAligned(
    const std::uint8_t* data, std::size_t size, std::vector<Packet>& packets) {
    if (!data || size == 0) return;

    // V10.8.125: SharedDvbInputPool receives bytes directly from the Linux DVB
    // TS tap. On the common aligned path, checking the 188-byte sync grid is
    // sufficient for framing; the selected-service remapper still performs the
    // full MPEG-TS header/adaptation validation before accepting a packet. This
    // avoids parsing every foreign-service packet in a high-bitrate MPTS.
    if (!pending_.empty()) {
        push(data, size, packets);
        return;
    }

    const std::size_t estimatedPackets = size / kPacketSize;
    if (estimatedPackets > 0 &&
        estimatedPackets <= packets.max_size() - packets.size()) {
        packets.reserve(packets.size() + estimatedPackets);
    }

    std::size_t direct = 0;
    while (size - direct >= kPacketSize) {
        if (data[direct] != kSyncByte) break;
        const std::size_t remaining = size - direct;
        if (remaining >= 2 * kPacketSize &&
            data[direct + kPacketSize] != kSyncByte) {
            break;
        }
        if (remaining >= 3 * kPacketSize &&
            data[direct + 2 * kPacketSize] != kSyncByte) {
            break;
        }

        packets.emplace_back();
        std::memcpy(packets.back().data(), data + direct, kPacketSize);
        direct += kPacketSize;
    }

    if (direct == size) return;
    if (direct > 0) {
        data += direct;
        size -= direct;
    }

    // Partial/misaligned/corrupt data retains the exact legacy resync and full
    // structural validation behavior.
    push(data, size, packets);
}

bool PacketFramer::visitTrustedAligned(
    const std::uint8_t* data, std::size_t size,
    void* context, TrustedPacketVisitor visitor) {
    if (!data || size == 0 || !visitor) return true;

    auto visitFramed = [&](const std::uint8_t* bytes, std::size_t bytesSize) {
        std::vector<Packet> fallbackPackets;
        push(bytes, bytesSize, fallbackPackets);
        for (const auto& packet : fallbackPackets) {
            if (!visitor(context, packet.data())) return false;
        }
        return true;
    };

    if (!pending_.empty()) {
        return visitFramed(data, size);
    }

    std::size_t direct = 0;
    while (size - direct >= kPacketSize) {
        if (data[direct] != kSyncByte) break;
        const std::size_t remaining = size - direct;
        if (remaining >= 2 * kPacketSize &&
            data[direct + kPacketSize] != kSyncByte) {
            break;
        }
        if (remaining >= 3 * kPacketSize &&
            data[direct + 2 * kPacketSize] != kSyncByte) {
            break;
        }

        if (!visitor(context, data + direct)) return false;
        direct += kPacketSize;
    }

    if (direct == size) return true;
    if (direct > 0) {
        data += direct;
        size -= direct;
    }

    return visitFramed(data, size);
}

void PacketFramer::reset() noexcept {
    pending_.clear();
}

ContinuityStatus ContinuityTracker::observe(
    const std::uint8_t* data, std::size_t size) noexcept {
    PacketInfo info;
    if (!inspectPacket(data, size, info)) {
        return ContinuityStatus::InvalidPacket;
    }
    if (info.transportError) {
        return ContinuityStatus::TransportError;
    }
    if (info.pid == kNullPid) {
        return ContinuityStatus::NullPacket;
    }

    State& state = states_[info.pid];
    if (info.discontinuity) {
        state = {info.continuityCounter, true, info.hasPayload};
        return ContinuityStatus::Discontinuity;
    }

    ContinuityStatus status = ContinuityStatus::InOrder;
    if (state.initialized) {
        if (info.hasPayload) {
            const std::uint8_t expected = static_cast<std::uint8_t>((state.counter + 1U) & 0x0fU);
            if (state.previousHadPayload && info.continuityCounter == state.counter) {
                status = ContinuityStatus::Duplicate;
            } else if (info.continuityCounter != expected) {
                status = ContinuityStatus::Gap;
            }
        } else if (info.continuityCounter != state.counter) {
            status = ContinuityStatus::Gap;
        }
    }

    state = {info.continuityCounter, true, info.hasPayload};
    return status;
}

void ContinuityTracker::reset() noexcept {
    states_.fill({});
}

} // namespace dvbstreamer5::media::mpegts
