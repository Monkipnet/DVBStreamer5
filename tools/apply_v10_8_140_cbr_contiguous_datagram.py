from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


def replace_exact_count(path: str, old: str, new: str, expected: int) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"{path}: expected {expected} matches, got {count}")
    p.write_text(text.replace(old, new), encoding="utf-8")


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.139";',
    'inline constexpr const char* kProgramVersion = "10.8.140";',
)

replace_exact_count(
    "src/media/NativeUdpRelay.cpp",
    "dvbstreamer5::media::mpegts::CbrDatagram cbrDatagram {};",
    "dvbstreamer5::media::mpegts::CbrDatagram cbrDatagram;",
    2,
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "    std::array<std::uint8_t,\n"
    "        dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram *\n"
    "            dvbstreamer5::media::mpegts::kPacketSize> bytes {};\n"
    "    for (std::size_t index = 0; index < packets.size(); ++index) {\n"
    "        std::copy(\n"
    "            packets[index].begin(),\n"
    "            packets[index].end(),\n"
    "            bytes.begin() + static_cast<std::ptrdiff_t>(\n"
    "                index * dvbstreamer5::media::mpegts::kPacketSize));\n"
    "    }\n"
    "    if (!output.send(bytes.data(), bytes.size(), error)) return false;\n"
    "    outputBytes.fetch_add(bytes.size(), std::memory_order_relaxed);",
    "    constexpr std::size_t kCbrDatagramBytes =\n"
    "        dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram *\n"
    "        dvbstreamer5::media::mpegts::kPacketSize;\n"
    "    static_assert(\n"
    "        sizeof(dvbstreamer5::media::mpegts::CbrDatagram) == kCbrDatagramBytes,\n"
    "        \"CBR datagram storage must be contiguous\");\n"
    "    // V10.8.140: CbrDatagram is already seven contiguous 188-byte packets.\n"
    "    // Send that storage directly instead of zeroing and copying a second\n"
    "    // 1316-byte scratch array for every paced UDP datagram.\n"
    "    const auto* bytes = reinterpret_cast<const std::uint8_t*>(packets.data());\n"
    "    if (!output.send(bytes, kCbrDatagramBytes, error)) return false;\n"
    "    outputBytes.fetch_add(kCbrDatagramBytes, std::memory_order_relaxed);",
)

replace_once(
    "src/media/NativeUdpRelay.cpp",
    "                    std::array<std::uint8_t,\n"
    "                        dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram *\n"
    "                            dvbstreamer5::media::mpegts::kPacketSize> bytes {};\n"
    "                    for (std::size_t index = 0; index < cbrDatagram.size(); ++index) {\n"
    "                        std::memcpy(\n"
    "                            bytes.data() +\n"
    "                                index * dvbstreamer5::media::mpegts::kPacketSize,\n"
    "                            cbrDatagram[index].data(),\n"
    "                            dvbstreamer5::media::mpegts::kPacketSize);\n"
    "                    }\n"
    "                    config_.observeTransport(bytes.data(), bytes.size());",
    "                    constexpr std::size_t kObservedCbrBytes =\n"
    "                        dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram *\n"
    "                        dvbstreamer5::media::mpegts::kPacketSize;\n"
    "                    static_assert(\n"
    "                        sizeof(dvbstreamer5::media::mpegts::CbrDatagram) ==\n"
    "                            kObservedCbrBytes,\n"
    "                        \"observed CBR datagram storage must be contiguous\");\n"
    "                    // V10.8.140: nextDatagram() fills every one of the seven\n"
    "                    // packets before returning true, so the observer can use\n"
    "                    // CbrDatagram storage directly with no second memset/copy.\n"
    "                    config_.observeTransport(\n"
    "                        reinterpret_cast<const std::uint8_t*>(cbrDatagram.data()),\n"
    "                        kObservedCbrBytes);",
)
