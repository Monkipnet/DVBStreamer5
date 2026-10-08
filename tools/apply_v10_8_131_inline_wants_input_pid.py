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
    'inline constexpr const char* kProgramVersion = "10.8.130";',
    'inline constexpr const char* kProgramVersion = "10.8.131";',
)

replace_once(
    "src/media/MpegTsRemapper.h",
    '    bool wantsInputPid(std::uint16_t pid) const noexcept;\n',
    '''    bool wantsInputPid(std::uint16_t pid) const noexcept {\n        // V10.8.131: this predicate is executed for every packet of the shared\n        // DVB MPTS before service selection. Keep it inline in the caller TU\n        // so the hot path avoids an out-of-line call (and the nested isAllowed\n        // call) for every foreign PID while preserving the exact V10.8.129/130\n        // admission semantics.\n        if (!initialized_) return true;\n        if (pid == 0x0000 || pid == 0x0001) return true;\n        if (pmtPid_ != kNullPid && pid == pmtPid_) return true;\n        if (!remapReady_) return false;\n        if (pid == 0x0011) return true;\n        return pid == kNullPid ||\n            (pid < allowedPids_.size() && allowedPids_[pid]) ||\n            pid == inputVideoPid_ || pid == inputAudioPid_;\n    }\n''',
)

old_cpp = '''bool Remapper::wantsInputPid(std::uint16_t pid) const noexcept {\n    if (!initialized_) return true;\n\n    // PAT and CAT are always needed to discover the selected PMT and CA/EMM PIDs.\n    if (pid == 0x0000 || pid == 0x0001) return true;\n\n    // Once PAT has selected the service, its PMT must keep flowing so dynamic\n    // PMT/PID changes are observed without restarting the channel.\n    if (pmtPid_ != kNullPid && pid == pmtPid_) return true;\n\n    // Before PMT is complete, the remaining multiplex payload cannot contribute\n    // to the selected service and Remapper::process() would discard it anyway.\n    if (!remapReady_) return false;\n\n    // SDT is regenerated for the selected service and may change at runtime.\n    if (pid == 0x0011) return true;\n\n    // Keep every already-learned selected-service PID, CA/EMM PID and NULL PID.\n    // Input video/audio are checked explicitly because output PID remapping can\n    // remove their original PID bits from allowedPids_.\n    return isAllowed(pid) || pid == inputVideoPid_ || pid == inputAudioPid_;\n}\n\n'''
replace_once("src/media/MpegTsRemapper.cpp", old_cpp, "")
