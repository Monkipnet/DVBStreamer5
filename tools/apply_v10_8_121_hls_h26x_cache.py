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
    'inline constexpr const char* kProgramVersion = "10.8.120";',
    'inline constexpr const char* kProgramVersion = "10.8.121";',
)

replace_once(
    "src/media/NativeHlsSegmenter.h",
    '''    std::array<std::uint8_t, 8192> elementaryStreamType_{};\n    std::uint64_t nextSequence_ = 0;\n''',
    '''    std::array<std::uint8_t, 8192> elementaryStreamType_{};\n    // V10.8.121: PMT-derived cache. appendPacket() is a per-TS-packet hot path;\n    // scanning all 8192 PID slots there multiplied CPU cost by every HLS stream.\n    bool hasH26xVideo_ = false;\n    std::uint64_t nextSequence_ = 0;\n''',
)

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '''    pmtPid_ = mpegts::kNullPid;\n    elementaryStreamType_.fill(0);\n    nextSequence_ = 0;\n''',
    '''    pmtPid_ = mpegts::kNullPid;\n    elementaryStreamType_.fill(0);\n    hasH26xVideo_ = false;\n    nextSequence_ = 0;\n''',
)

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '''                            pmtCollecting_.clear();\n                            pmtPrefix_.clear();\n                            elementaryStreamType_.fill(0);\n''',
    '''                            pmtCollecting_.clear();\n                            pmtPrefix_.clear();\n                            elementaryStreamType_.fill(0);\n                            hasH26xVideo_ = false;\n''',
)

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '''            pmtCollecting_.push_back(packet);\n            (void)parsePmtStreamTypes(pmtCollecting_, elementaryStreamType_);\n''',
    '''            pmtCollecting_.push_back(packet);\n            if (parsePmtStreamTypes(pmtCollecting_, elementaryStreamType_)) {\n                // PMT changes are rare compared with TS packet rate. Compute the\n                // 8192-entry H.26x presence scan here once per completed PMT,\n                // not from appendPacket() for every packet.\n                hasH26xVideo_ = std::any_of(\n                    elementaryStreamType_.begin(), elementaryStreamType_.end(),\n                    [](std::uint8_t type) { return type == 0x1bU || type == 0x24U; });\n            }\n''',
)

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '''    const bool hasH26xVideo = std::any_of(\n        elementaryStreamType_.begin(), elementaryStreamType_.end(),\n        [](std::uint8_t type) { return type == 0x1bU || type == 0x24U; });\n''',
    '''    // V10.8.121: hasH26xVideo_ is maintained when PMT state changes.\n''',
)

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '''    if (!config_.independentSegments && hasH26xVideo) {\n''',
    '''    if (!config_.independentSegments && hasH26xVideo_) {\n''',
)

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '''    const bool passthroughH26x = !config_.independentSegments &&\n        std::any_of(elementaryStreamType_.begin(), elementaryStreamType_.end(),\n                    [](std::uint8_t type) { return type == 0x1bU || type == 0x24U; });\n''',
    '''    const bool passthroughH26x = !config_.independentSegments && hasH26xVideo_;\n''',
)

print("V10.8.121 HLS H26x PMT cache applied")
