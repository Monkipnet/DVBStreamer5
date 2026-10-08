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
    'inline constexpr const char* kProgramVersion = "10.8.129";',
    'inline constexpr const char* kProgramVersion = "10.8.130";',
)

old = r'''bool Remapper::processCatSection(
    const std::vector<std::uint8_t>& section,
    std::string& error) {
    if (section.size() < 12 || section[0] != 0x01) return true;

    std::array<bool, 8192> found {};
    if (!addCaPids(section.data() + 8, section.size() - 12, found)) {
        error = "native remap encountered malformed CAT descriptors";
        return false;
    }
    for (std::size_t pid = 0; pid < found.size(); ++pid) {
        if (found[pid]) caPids_[pid] = true;
    }
    if (remapReady_) {
        allowedPids_[0x01] = true;
        for (std::size_t pid = 0; pid < caPids_.size(); ++pid) {
            if (caPids_[pid]) allowedPids_[pid] = true;
        }
    }
    return true;
}
'''

new = r'''bool Remapper::processCatSection(
    const std::vector<std::uint8_t>& section,
    std::string& error) {
    if (section.size() < 12 || section[0] != 0x01) return true;

    // V10.8.130: CAT normally carries only a handful of CA descriptors. The
    // previous path cleared an 8192-entry bitmap and then scanned all 8192 PIDs
    // twice for every repeated CAT section. Collect only the descriptor PIDs,
    // validate the whole section first, then commit them transactionally.
    // A section is capped at 4096 bytes by collectSections(); a CA descriptor
    // needs at least tag+length+4 bytes, so 682 entries safely cover the limit.
    std::array<std::uint16_t, 682> foundPids;
    std::size_t foundCount = 0;
    const std::uint8_t* descriptors = section.data() + 8;
    const std::size_t descriptorBytes = section.size() - 12;
    std::size_t offset = 0;
    while (offset + 2 <= descriptorBytes) {
        const std::uint8_t tag = descriptors[offset++];
        const std::size_t length = descriptors[offset++];
        if (offset + length > descriptorBytes) {
            error = "native remap encountered malformed CAT descriptors";
            return false;
        }
        if (tag == 0x09 && length >= 4) {
            const std::uint16_t pid = readPid(descriptors + offset + 2);
            if (pid < 0x1fff) {
                if (foundCount >= foundPids.size()) {
                    error = "native remap encountered too many CAT CA descriptors";
                    return false;
                }
                foundPids[foundCount++] = pid;
            }
        }
        offset += length;
    }
    if (offset != descriptorBytes) {
        error = "native remap encountered malformed CAT descriptors";
        return false;
    }

    if (remapReady_) allowedPids_[0x01] = true;
    for (std::size_t index = 0; index < foundCount; ++index) {
        const std::uint16_t pid = foundPids[index];
        caPids_[pid] = true;
        if (remapReady_) allowedPids_[pid] = true;
    }
    return true;
}
'''

replace_once("src/media/MpegTsRemapper.cpp", old, new)
