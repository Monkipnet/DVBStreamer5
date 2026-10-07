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
    'inline constexpr const char* kProgramVersion = "10.8.124";',
    'inline constexpr const char* kProgramVersion = "10.8.125";',
)

replace_once(
    "src/media/TransportStream.h",
    '''class PacketFramer {\npublic:\n    void push(const std::uint8_t* data, std::size_t size, std::vector<Packet>& packets);\n    void reset() noexcept;\n''',
    '''class PacketFramer {\npublic:\n    void push(const std::uint8_t* data, std::size_t size, std::vector<Packet>& packets);\n    // Fast path for trusted Linux DVB TS-tap input. It verifies the 188-byte\n    // sync grid but intentionally defers full packet-header validation to the\n    // selected-service remapper. Any alignment break falls back to push().\n    void pushTrustedAligned(\n        const std::uint8_t* data, std::size_t size, std::vector<Packet>& packets);\n    void reset() noexcept;\n''',
)

replace_once(
    "src/media/TransportStream.cpp",
    '''void PacketFramer::reset() noexcept {\n    pending_.clear();\n}\n''',
    '''void PacketFramer::pushTrustedAligned(\n    const std::uint8_t* data, std::size_t size, std::vector<Packet>& packets) {\n    if (!data || size == 0) return;\n\n    // V10.8.125: SharedDvbInputPool receives bytes directly from the Linux DVB\n    // TS tap. On the common aligned path, checking the 188-byte sync grid is\n    // sufficient for framing; the selected-service remapper still performs the\n    // full MPEG-TS header/adaptation validation before accepting a packet. This\n    // avoids parsing every foreign-service packet in a high-bitrate MPTS.\n    if (!pending_.empty()) {\n        push(data, size, packets);\n        return;\n    }\n\n    const std::size_t estimatedPackets = size / kPacketSize;\n    if (estimatedPackets > 0 &&\n        estimatedPackets <= packets.max_size() - packets.size()) {\n        packets.reserve(packets.size() + estimatedPackets);\n    }\n\n    std::size_t direct = 0;\n    while (size - direct >= kPacketSize) {\n        if (data[direct] != kSyncByte) break;\n        const std::size_t remaining = size - direct;\n        if (remaining >= 2 * kPacketSize &&\n            data[direct + kPacketSize] != kSyncByte) {\n            break;\n        }\n        if (remaining >= 3 * kPacketSize &&\n            data[direct + 2 * kPacketSize] != kSyncByte) {\n            break;\n        }\n\n        packets.emplace_back();\n        std::memcpy(packets.back().data(), data + direct, kPacketSize);\n        direct += kPacketSize;\n    }\n\n    if (direct == size) return;\n    if (direct > 0) {\n        data += direct;\n        size -= direct;\n    }\n\n    // Partial/misaligned/corrupt data retains the exact legacy resync and full\n    // structural validation behavior.\n    push(data, size, packets);\n}\n\nvoid PacketFramer::reset() noexcept {\n    pending_.clear();\n}\n''',
)

replace_once(
    "src/StreamManager.cpp",
    '''        inputPackets_.clear();\n        framer_.push(data, size, inputPackets_);\n        filteredPackets_.clear();\n''',
    '''        inputPackets_.clear();\n        // V10.8.125: bytes originate from the shared Linux DVB TS tap. Avoid\n        // full inspectPacket() work for every foreign-service MPTS packet; the\n        // selected packets are validated below and again by Remapper::process().\n        framer_.pushTrustedAligned(data, size, inputPackets_);\n        filteredPackets_.clear();\n''',
)

replace_once(
    "src/StreamManager.cpp",
    '''                if (!remapper_.wantsInputPid(pid)) continue;\n            }\n\n            // Remapper appends to the supplied packet vector. Accumulate the\n''',
    '''                if (!remapper_.wantsInputPid(pid)) continue;\n            }\n\n            // The trusted framer intentionally skipped full header validation.\n            // Preserve the old behavior for malformed selected-service packets:\n            // drop them here instead of turning a recoverable damaged TS packet\n            // into a fatal remapper error. Foreign-service packets never pay this\n            // parsing cost because wantsInputPid() rejected them above.\n            dvbstreamer5::media::mpegts::PacketInfo selectedInfo;\n            if (!dvbstreamer5::media::mpegts::inspectPacket(\n                    packet.data(), packet.size(), selectedInfo)) {\n                continue;\n            }\n\n            // Remapper appends to the supplied packet vector. Accumulate the\n''',
)

print("V10.8.125 shared DVB trusted framer fast path applied")
