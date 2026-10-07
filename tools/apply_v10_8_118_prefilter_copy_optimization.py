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
    'inline constexpr const char* kProgramVersion = "10.8.117";',
    'inline constexpr const char* kProgramVersion = "10.8.118";',
)

replace_once(
    "src/StreamManager.cpp",
    '''        inputPackets_.reserve(384);
        remappedPackets_.reserve(64);
        outputBytes_.reserve(64 * 1024);
''',
    '''        inputPackets_.reserve(384);
        filteredPackets_.reserve(128);
''',
)

replace_once(
    "src/StreamManager.cpp",
    '''        inputPackets_.clear();
        framer_.push(data, size, inputPackets_);
        outputBytes_.clear();

        for (const auto& packet : inputPackets_) {
            if (dropSourceNullPackets_) {
                dvbstreamer5::media::mpegts::PacketInfo info;
                if (dvbstreamer5::media::mpegts::inspectPacket(
                        packet.data(), packet.size(), info) &&
                    info.pid == dvbstreamer5::media::mpegts::kNullPid) {
                    continue;
                }
            }

            remappedPackets_.clear();
            std::string remapError;
            if (!remapper_.process(packet, remappedPackets_, remapError)) {
                const std::string message = remapError.empty()
                    ? "shared DVB service prefilter failed"
                    : "shared DVB service prefilter failed: " + remapError;
                std::cerr << "SHARED DVB service prefilter failed stream="
                          << streamId_ << " error=" << message << std::endl;
                relay->finishInput(message);
                return false;
            }

            for (const auto& filtered : remappedPackets_) {
                outputBytes_.insert(
                    outputBytes_.end(), filtered.begin(), filtered.end());
            }
        }

        if (outputBytes_.empty()) return true;
        return relay->pushInput(outputBytes_.data(), outputBytes_.size());
''',
    '''        inputPackets_.clear();
        framer_.push(data, size, inputPackets_);
        filteredPackets_.clear();

        // V10.8.118: keep the V10.8.117 filtering semantics but avoid doing a
        // full inspectPacket() before Remapper::process() for every non-null
        // packet. For the NULL fast path the three-byte TS header is enough;
        // malformed packets still reach Remapper and retain its validation.
        std::string remapError;
        for (const auto& packet : inputPackets_) {
            if (dropSourceNullPackets_ &&
                packet[0] == dvbstreamer5::media::mpegts::kSyncByte) {
                const std::uint16_t pid = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8) |
                    packet[2]);
                if (pid == dvbstreamer5::media::mpegts::kNullPid) continue;
            }

            // Remapper appends to the supplied packet vector. Accumulate the
            // whole selected-service block directly instead of creating a
            // temporary vector for each input packet and copying every 188-byte
            // packet again into a byte vector.
            if (!remapper_.process(packet, filteredPackets_, remapError)) {
                const std::string message = remapError.empty()
                    ? "shared DVB service prefilter failed"
                    : "shared DVB service prefilter failed: " + remapError;
                std::cerr << "SHARED DVB service prefilter failed stream="
                          << streamId_ << " error=" << message << std::endl;
                relay->finishInput(message);
                return false;
            }
        }

        if (filteredPackets_.empty()) return true;
        static_assert(
            sizeof(dvbstreamer5::media::mpegts::Packet) ==
                dvbstreamer5::media::mpegts::kPacketSize,
            "MPEG-TS packet storage must be contiguous");
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(
            filteredPackets_.data());
        return relay->pushInput(
            bytes,
            filteredPackets_.size() * dvbstreamer5::media::mpegts::kPacketSize);
''',
)

replace_once(
    "src/StreamManager.cpp",
    '''    std::vector<dvbstreamer5::media::mpegts::Packet> inputPackets_;
    std::vector<dvbstreamer5::media::mpegts::Packet> remappedPackets_;
    std::vector<std::uint8_t> outputBytes_;
''',
    '''    std::vector<dvbstreamer5::media::mpegts::Packet> inputPackets_;
    std::vector<dvbstreamer5::media::mpegts::Packet> filteredPackets_;
''',
)

print("V10.8.118 prefilter copy/parse optimization applied")
