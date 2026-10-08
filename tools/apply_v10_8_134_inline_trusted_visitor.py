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
    'inline constexpr const char* kProgramVersion = "10.8.133";',
    'inline constexpr const char* kProgramVersion = "10.8.134";',
)

replace_once(
    "src/media/TransportStream.h",
    '''    using TrustedPacketVisitor = bool (*)(void*, const std::uint8_t*);\n    bool visitTrustedAligned(\n        const std::uint8_t* data, std::size_t size,\n        void* context, TrustedPacketVisitor visitor);\n    void reset() noexcept;\n''',
    '''    using TrustedPacketVisitor = bool (*)(void*, const std::uint8_t*);\n    bool visitTrustedAligned(\n        const std::uint8_t* data, std::size_t size,\n        void* context, TrustedPacketVisitor visitor);\n\n    // V10.8.134: shared-DVB executes this visitor for every packet of the\n    // full multiplex. Keeping the common aligned loop in the caller's TU lets\n    // the compiler inline the selected-service visitor instead of paying an\n    // indirect function-pointer call for every 188-byte packet. The fallback\n    // still uses push(), preserving the existing byte-resync/full-validation\n    // behavior for partial, misaligned, or corrupt input.\n    template <typename Visitor>\n    bool visitTrustedAlignedInline(\n        const std::uint8_t* data, std::size_t size, Visitor&& visitor) {\n        if (!data || size == 0) return true;\n\n        auto visitFramed = [&](const std::uint8_t* bytes, std::size_t bytesSize) {\n            std::vector<Packet> fallbackPackets;\n            push(bytes, bytesSize, fallbackPackets);\n            for (const auto& packet : fallbackPackets) {\n                if (!visitor(packet.data())) return false;\n            }\n            return true;\n        };\n\n        if (!pending_.empty()) {\n            return visitFramed(data, size);\n        }\n\n        std::size_t direct = 0;\n        while (size - direct >= kPacketSize) {\n            if (data[direct] != kSyncByte) break;\n            const std::size_t remaining = size - direct;\n            if (remaining >= 2 * kPacketSize &&\n                data[direct + kPacketSize] != kSyncByte) {\n                break;\n            }\n            if (remaining >= 3 * kPacketSize &&\n                data[direct + 2 * kPacketSize] != kSyncByte) {\n                break;\n            }\n\n            if (!visitor(data + direct)) return false;\n            direct += kPacketSize;\n        }\n\n        if (direct == size) return true;\n        if (direct > 0) {\n            data += direct;\n            size -= direct;\n        }\n        return visitFramed(data, size);\n    }\n\n    void reset() noexcept;\n''',
)

replace_once(
    "src/StreamManager.cpp",
    '''        if (!framer_.visitTrustedAligned(\n                data, size, &visitContext, &SharedDvbServicePrefilter::visitPacket)) {\n            return false;\n        }\n''',
    '''        if (!framer_.visitTrustedAlignedInline(\n                data, size, [&](const std::uint8_t* bytes) {\n                    return visitPacket(&visitContext, bytes);\n                })) {\n            return false;\n        }\n''',
)
