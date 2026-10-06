from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}\n--- OLD ---\n{old}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.113";',
    'inline constexpr const char* kProgramVersion = "10.8.114";',
)

replace_once(
    "src/media/MpegTsRemapper.h",
    '#include <array>\n#include <cstdint>',
    '#include <array>\n#include <chrono>\n#include <cstdint>',
)

replace_once(
    "src/media/MpegTsRemapper.h",
    '    bool isAllowed(std::uint16_t pid) const noexcept;\n',
    '    void emitPeriodicPsi(std::vector<Packet>& output);\n    bool isAllowed(std::uint16_t pid) const noexcept;\n',
)

replace_once(
    "src/media/MpegTsRemapper.h",
    '    PsiSectionState pmtSection_;\n    PsiSectionState sdtSection_;\n    bool initialized_ = false;',
    '    PsiSectionState pmtSection_;\n    PsiSectionState sdtSection_;\n    std::vector<std::uint8_t> pmtOutputSection_;\n    std::chrono::steady_clock::time_point nextPatPmtAt_ {};\n    std::chrono::steady_clock::time_point nextSdtAt_ {};\n    bool initialized_ = false;',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    'namespace {\n\nstd::uint32_t sectionCrc32',
    'namespace {\n\nconstexpr auto kPatPmtInterval = std::chrono::milliseconds(100);\nconstexpr auto kSdtInterval = std::chrono::milliseconds(500);\n\nstd::uint32_t sectionCrc32',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '    pmtSection_ = {};\n    sdtSection_ = {};\n    allowedPids_.fill(false);',
    '    pmtSection_ = {};\n    sdtSection_ = {};\n    pmtOutputSection_.clear();\n    nextPatPmtAt_ = {};\n    nextSdtAt_ = {};\n    allowedPids_.fill(false);',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '        pmtSection_ = {};\n        pmtOutputContinuity_ = 0;\n        allowedPids_.fill(false);\n        remapReady_ = false;',
    '        pmtSection_ = {};\n        pmtOutputSection_.clear();\n        pmtOutputContinuity_ = 0;\n        nextPatPmtAt_ = {};\n        nextSdtAt_ = {};\n        allowedPids_.fill(false);\n        remapReady_ = false;',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    'bool Remapper::processPmtSection(\n    std::vector<std::uint8_t> section,\n    std::vector<Packet>& output,\n    std::string& error) {\n',
    'bool Remapper::processPmtSection(\n    std::vector<std::uint8_t> section,\n    std::vector<Packet>& output,\n    std::string& error) {\n    (void)output;\n',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '    const bool wasReady = remapReady_;\n',
    '',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '    allowedPids_ = allowed;\n    remapReady_ = true;\n    if (!wasReady) {\n        output.push_back(makePat(patOutputContinuity_));\n        patOutputContinuity_ =\n            static_cast<std::uint8_t>((patOutputContinuity_ + 1U) & 0x0fU);\n    }\n    packetizeSection(pmtPid_, section, pmtOutputContinuity_, output);\n    return true;\n}',
    '    allowedPids_ = allowed;\n    pmtOutputSection_ = std::move(section);\n    const bool becameReady = !remapReady_;\n    remapReady_ = true;\n    if (becameReady) {\n        // Emit a complete PSI/SI set on the very next TS packet, then keep\n        // repeating it from our own monotonic clock. This avoids long gaps\n        // when the provider repeats PAT/PMT/SDT irregularly.\n        nextPatPmtAt_ = {};\n        nextSdtAt_ = {};\n    }\n    return true;\n}',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    'bool Remapper::processSdtSection(\n    const std::vector<std::uint8_t>& section,\n    std::vector<Packet>& output,\n    std::string& error) {\n    (void)error;\n',
    'bool Remapper::processSdtSection(\n    const std::vector<std::uint8_t>& section,\n    std::vector<Packet>& output,\n    std::string& error) {\n    (void)output;\n    (void)error;\n',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '    originalNetworkId_ = static_cast<std::uint16_t>(\n        (section[8] << 8) | section[9]);\n    output.push_back(makeSdt(sdtOutputContinuity_));\n    sdtOutputContinuity_ =\n        static_cast<std::uint8_t>((sdtOutputContinuity_ + 1U) & 0x0fU);\n    return true;\n}\n\nbool Remapper::isAllowed',
    '    originalNetworkId_ = static_cast<std::uint16_t>(\n        (section[8] << 8) | section[9]);\n    // The regenerated SDT content changed; advertise it promptly instead of\n    // waiting for the normal 500 ms repeat deadline.\n    nextSdtAt_ = {};\n    return true;\n}\n\nvoid Remapper::emitPeriodicPsi(std::vector<Packet>& output) {\n    if (!remapReady_ || pmtPid_ == 0x1fff || pmtOutputSection_.empty()) return;\n\n    const auto now = std::chrono::steady_clock::now();\n    if (nextPatPmtAt_ == std::chrono::steady_clock::time_point{} ||\n        now >= nextPatPmtAt_) {\n        output.push_back(makePat(patOutputContinuity_));\n        patOutputContinuity_ =\n            static_cast<std::uint8_t>((patOutputContinuity_ + 1U) & 0x0fU);\n        packetizeSection(\n            pmtPid_, pmtOutputSection_, pmtOutputContinuity_, output);\n        nextPatPmtAt_ = now + kPatPmtInterval;\n    }\n\n    if (nextSdtAt_ == std::chrono::steady_clock::time_point{} ||\n        now >= nextSdtAt_) {\n        output.push_back(makeSdt(sdtOutputContinuity_));\n        sdtOutputContinuity_ =\n            static_cast<std::uint8_t>((sdtOutputContinuity_ + 1U) & 0x0fU);\n        nextSdtAt_ = now + kSdtInterval;\n    }\n}\n\nbool Remapper::isAllowed',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '    std::vector<std::vector<std::uint8_t>> sections;\n    if (info.pid == 0x0000) {',
    '    // Keep the service discoverable independently of the provider table\n    // cadence. The CBR pacer downstream absorbs this tiny deterministic PSI\n    // overhead with its normal NULL stuffing; media/PCR handling is untouched.\n    emitPeriodicPsi(output);\n\n    std::vector<std::vector<std::uint8_t>> sections;\n    if (info.pid == 0x0000) {',
)

replace_once(
    "src/media/MpegTsRemapper.cpp",
    '        bool sawPat = false;\n        for (const auto& section : sections) {\n            if (!section.empty() && section[0] == 0x00) {\n                sawPat = true;\n                if (!processPatSection(section, error)) return false;\n            }\n        }\n        if (remapReady_ && sawPat) {\n            output.push_back(makePat(patOutputContinuity_));\n            patOutputContinuity_ =\n                static_cast<std::uint8_t>((patOutputContinuity_ + 1U) & 0x0fU);\n        }\n        return true;',
    '        for (const auto& section : sections) {\n            if (!section.empty() && section[0] == 0x00) {\n                if (!processPatSection(section, error)) return false;\n            }\n        }\n        return true;',
)

print("V10.8.114 stable PSI/SI patch applied")
