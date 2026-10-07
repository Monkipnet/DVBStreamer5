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
    'inline constexpr const char* kProgramVersion = "10.8.123";',
    'inline constexpr const char* kProgramVersion = "10.8.124";',
)

replace_once(
    "src/media/TransportStream.cpp",
    '''            Packet packet {};\n            std::memcpy(packet.data(), data + direct, kPacketSize);\n            packets.push_back(packet);\n            direct += kPacketSize;\n''',
    '''            // V10.8.124: construct the destination packet directly in the\n            // vector. The previous temporary Packet + push_back(packet) copied\n            // every 188-byte TS packet twice on this hot aligned-input path.\n            packets.emplace_back();\n            std::memcpy(packets.back().data(), data + direct, kPacketSize);\n            direct += kPacketSize;\n''',
)

replace_once(
    "src/media/TransportStream.cpp",
    '''        Packet packet {};\n        std::memcpy(packet.data(), pending_.data() + candidate, kPacketSize);\n        packets.push_back(packet);\n        consumed = candidate + kPacketSize;\n''',
    '''        // Same single-copy rule for the byte-resync fallback path.\n        packets.emplace_back();\n        std::memcpy(\n            packets.back().data(), pending_.data() + candidate, kPacketSize);\n        consumed = candidate + kPacketSize;\n''',
)

print("V10.8.124 PacketFramer single-copy optimization applied")
