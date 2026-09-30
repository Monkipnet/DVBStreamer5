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
            break;
        }

        if (candidate + kPacketSize > pending_.size()) {
            consumed = pending_.size() > kPacketSize - 1
                ? pending_.size() - (kPacketSize - 1)
                : 0;
            break;
        }

        Packet packet {};
        std::memcpy(packet.data(), pending_.data() + candidate, kPacketSize);
        packets.push_back(packet);
        consumed = candidate + kPacketSize;
    }

    if (consumed > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
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
