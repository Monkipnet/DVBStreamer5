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
    'inline constexpr const char* kProgramVersion = "10.8.119";',
    'inline constexpr const char* kProgramVersion = "10.8.120";',
)

replace_once(
    "src/media/MpegTsRemapper.h",
    '''    bool initialize(const RemapConfig& config, std::string& error);\n    bool process(const Packet& input, std::vector<Packet>& output, std::string& error);\n''',
    '''    bool initialize(const RemapConfig& config, std::string& error);\n    bool process(const Packet& input, std::vector<Packet>& output, std::string& error);\n    void tick(std::vector<Packet>& output);\n    bool wantsInputPid(std::uint16_t pid) const noexcept;\n''',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '''bool Remapper::isAllowed(std::uint16_t pid) const noexcept {\n    return pid == kNullPid || (pid < allowedPids_.size() && allowedPids_[pid]);\n}\n''',
    '''void Remapper::tick(std::vector<Packet>& output) {\n    emitPeriodicPsi(output);\n}\n\nbool Remapper::wantsInputPid(std::uint16_t pid) const noexcept {\n    if (!initialized_) return true;\n\n    // PAT and CAT are always needed to discover the selected PMT and CA/EMM PIDs.\n    if (pid == 0x0000 || pid == 0x0001) return true;\n\n    // Once PAT has selected the service, its PMT must keep flowing so dynamic\n    // PMT/PID changes are observed without restarting the channel.\n    if (pmtPid_ != kNullPid && pid == pmtPid_) return true;\n\n    // Before PMT is complete, the remaining multiplex payload cannot contribute\n    // to the selected service and Remapper::process() would discard it anyway.\n    if (!remapReady_) return false;\n\n    // SDT is regenerated for the selected service and may change at runtime.\n    if (pid == 0x0011) return true;\n\n    // Keep every already-learned selected-service PID, CA/EMM PID and NULL PID.\n    // Input video/audio are checked explicitly because output PID remapping can\n    // remove their original PID bits from allowedPids_.\n    return isAllowed(pid) || pid == inputVideoPid_ || pid == inputAudioPid_;\n}\n\nbool Remapper::isAllowed(std::uint16_t pid) const noexcept {\n    return pid == kNullPid || (pid < allowedPids_.size() && allowedPids_[pid]);\n}\n''',
)

replace_once(
    "src/StreamManager.cpp",
    '''        std::string remapError;\n        for (const auto& packet : inputPackets_) {\n            if (dropSourceNullPackets_ &&\n                packet[0] == dvbstreamer5::media::mpegts::kSyncByte) {\n                const std::uint16_t pid = static_cast<std::uint16_t>(\n                    (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8) |\n                    packet[2]);\n                if (pid == dvbstreamer5::media::mpegts::kNullPid) continue;\n            }\n\n            // Remapper appends to the supplied packet vector. Accumulate the\n            // whole selected-service block directly instead of creating a\n            // temporary vector for each input packet and copying every 188-byte\n            // packet again into a byte vector.\n            if (!remapper_.process(packet, filteredPackets_, remapError)) {\n''',
    '''        std::string remapError;\n        for (const auto& packet : inputPackets_) {\n            if (packet[0] == dvbstreamer5::media::mpegts::kSyncByte) {\n                const std::uint16_t pid = static_cast<std::uint16_t>(\n                    (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8) |\n                    packet[2]);\n                if (dropSourceNullPackets_ &&\n                    pid == dvbstreamer5::media::mpegts::kNullPid) {\n                    continue;\n                }\n\n                // V10.8.120: once PAT/PMT state is known, reject PIDs belonging\n                // to other services before inspectPacket(), PSI assembly and the\n                // per-packet steady-clock check inside Remapper::process().\n                if (!remapper_.wantsInputPid(pid)) continue;\n            }\n\n            // Remapper appends to the supplied packet vector. Accumulate the\n            // whole selected-service block directly instead of creating a\n            // temporary vector for each input packet and copying every 188-byte\n            // packet again into a byte vector.\n            if (!remapper_.process(packet, filteredPackets_, remapError)) {\n''',
)

replace_once(
    "src/StreamManager.cpp",
    '''        if (filteredPackets_.empty()) return true;\n        static_assert(\n''',
    '''        // Preserve the V10.8.114 PAT/PMT/SDT cadence even when an input\n        // block contained only foreign-service PIDs and therefore skipped every\n        // Remapper::process() call above.\n        remapper_.tick(filteredPackets_);\n\n        if (filteredPackets_.empty()) return true;\n        static_assert(\n''',
)

print("V10.8.120 shared DVB early PID gate applied")
