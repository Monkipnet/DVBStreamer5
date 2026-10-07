from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}\n--- OLD ---\n{old}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.121";',
    'inline constexpr const char* kProgramVersion = "10.8.122";',
)

old = r'''void PacketFramer::push(
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

        Packet packet {};
        std::memcpy(packet.data(), pending_.data() + candidate, kPacketSize);
        packets.push_back(packet);
        consumed = candidate + kPacketSize;
    }

    if (consumed > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
}
'''

new = r'''void PacketFramer::push(
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

            Packet packet {};
            std::memcpy(packet.data(), data + direct, kPacketSize);
            packets.push_back(packet);
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

        Packet packet {};
        std::memcpy(packet.data(), pending_.data() + candidate, kPacketSize);
        packets.push_back(packet);
        consumed = candidate + kPacketSize;
    }

    if (consumed > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
}
'''

replace_once("src/media/TransportStream.cpp", old, new)
print("V10.8.122 PacketFramer aligned fast path applied")
