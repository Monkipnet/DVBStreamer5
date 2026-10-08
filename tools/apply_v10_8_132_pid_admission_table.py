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
    'inline constexpr const char* kProgramVersion = "10.8.131";',
    'inline constexpr const char* kProgramVersion = "10.8.132";',
)

replace_once(
    "src/media/MpegTsRemapper.h",
    '''    bool wantsInputPid(std::uint16_t pid) const noexcept {\n        // V10.8.131: this predicate is executed for every packet of the shared\n        // DVB MPTS before service selection. Keep it inline in the caller TU\n        // so the hot path avoids an out-of-line call (and the nested isAllowed\n        // call) for every foreign PID while preserving the exact V10.8.129/130\n        // admission semantics.\n        if (!initialized_) return true;\n        if (pid == 0x0000 || pid == 0x0001) return true;\n        if (pmtPid_ != kNullPid && pid == pmtPid_) return true;\n        if (!remapReady_) return false;\n        if (pid == 0x0011) return true;\n        return pid == kNullPid ||\n            (pid < allowedPids_.size() && allowedPids_[pid]) ||\n            pid == inputVideoPid_ || pid == inputAudioPid_;\n    }\n''',
    '''    bool wantsInputPid(std::uint16_t pid) const noexcept {\n        // V10.8.132: the shared-DVB hot path executes this predicate for every\n        // packet of the full multiplex. All state-dependent admission decisions\n        // are cached when PAT/CAT/PMT state changes, leaving one table lookup per\n        // packet while preserving the V10.8.131 admission semantics exactly.\n        if (!initialized_) return true;\n        return pid < inputAdmissionPids_.size() && inputAdmissionPids_[pid];\n    }\n''',
)

replace_once(
    "src/media/MpegTsRemapper.h",
    '''    std::array<bool, 8192> allowedPids_ {};\n    std::array<bool, 8192> caPids_ {};\n''',
    '''    std::array<bool, 8192> allowedPids_ {};\n    std::array<bool, 8192> caPids_ {};\n    std::array<bool, 8192> inputAdmissionPids_ {};\n''',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '''    allowedPids_.fill(false);\n    caPids_.fill(false);\n    remapReady_ = false;\n    initialized_ = true;\n''',
    '''    allowedPids_.fill(false);\n    caPids_.fill(false);\n    inputAdmissionPids_.fill(false);\n    inputAdmissionPids_[0x0000] = true;\n    inputAdmissionPids_[0x0001] = true;\n    remapReady_ = false;\n    initialized_ = true;\n''',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '''    if (remapReady_) allowedPids_[0x01] = true;\n    for (std::size_t index = 0; index < foundCount; ++index) {\n        const std::uint16_t pid = foundPids[index];\n        caPids_[pid] = true;\n        if (remapReady_) allowedPids_[pid] = true;\n    }\n''',
    '''    if (remapReady_) {\n        allowedPids_[0x01] = true;\n        inputAdmissionPids_[0x01] = true;\n    }\n    for (std::size_t index = 0; index < foundCount; ++index) {\n        const std::uint16_t pid = foundPids[index];\n        caPids_[pid] = true;\n        if (remapReady_) {\n            allowedPids_[pid] = true;\n            inputAdmissionPids_[pid] = true;\n        }\n    }\n''',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '''        nextPatPmtAt_ = {};\n        nextSdtAt_ = {};\n        allowedPids_.fill(false);\n        remapReady_ = false;\n''',
    '''        nextPatPmtAt_ = {};\n        nextSdtAt_ = {};\n        allowedPids_.fill(false);\n        inputAdmissionPids_.fill(false);\n        inputAdmissionPids_[0x0000] = true;\n        inputAdmissionPids_[0x0001] = true;\n        inputAdmissionPids_[pmtPid_] = true;\n        remapReady_ = false;\n''',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '''    allowedPids_ = allowed;\n    pmtOutputSection_ = std::move(section);\n    const bool becameReady = !remapReady_;\n    remapReady_ = true;\n''',
    '''    allowedPids_ = allowed;\n    inputAdmissionPids_ = allowed;\n    if (inputVideoPid_ < inputAdmissionPids_.size()) {\n        inputAdmissionPids_[inputVideoPid_] = true;\n    }\n    if (inputAudioPid_ < inputAdmissionPids_.size()) {\n        inputAdmissionPids_[inputAudioPid_] = true;\n    }\n    pmtOutputSection_ = std::move(section);\n    const bool becameReady = !remapReady_;\n    remapReady_ = true;\n''',
)
