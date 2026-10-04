#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one anchor, found {count}: {old[:180]!r}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.72";',
    'inline constexpr const char* kProgramVersion = "10.8.73";')

replace_once(
    "src/StreamManager.cpp",
    '    constexpr auto kIdleGrace = std::chrono::seconds(30);',
    '    constexpr auto kIdleGrace = std::chrono::seconds(5);')

replace_once(
    "src/StreamManager.cpp",
    '                          << " reason=no-clients idle_s=30" << std::endl;',
    '                          << " reason=no-clients idle_s=5" << std::endl;')

replace_once(
    "src/media/NativeHlsSegmenter.h",
    '    std::deque<SegmentInfo> liveSegments_;\n',
    '    std::deque<SegmentInfo> liveSegments_;\n'
    '    // Keep one previous live window on disk after it leaves the playlist.\n'
    '    // HLS clients can legally request segments from a slightly stale playlist;\n'
    '    // deleting an evicted segment immediately turns that race into a 404/stall.\n'
    '    std::deque<SegmentInfo> retiredSegments_;\n')

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '    liveSegments_.clear();\n'
    '    nextSequence_ = 0;\n',
    '    liveSegments_.clear();\n'
    '    retiredSegments_.clear();\n'
    '    nextSequence_ = 0;\n')

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '''void NativeHlsSegmenter::prune() {
    while (liveSegments_.size() > config_.liveWindowSegments) {
        const SegmentInfo old = liveSegments_.front();
        liveSegments_.pop_front();
        if (!config_.archiveEnabled) {
            std::error_code ec;
            std::filesystem::remove(config_.directory / old.fileName, ec);
        }
    }
    if (!config_.archiveEnabled) return;
''',
    '''void NativeHlsSegmenter::prune() {
    while (liveSegments_.size() > config_.liveWindowSegments) {
        const SegmentInfo old = liveSegments_.front();
        liveSegments_.pop_front();
        if (!config_.archiveEnabled) {
            retiredSegments_.push_back(old);
        }
    }
    if (!config_.archiveEnabled) {
        // Do not remove a segment at the exact instant it disappears from the
        // newest playlist. A player may still be working from the immediately
        // previous manifest and request it a moment later. Retaining one extra
        // playlist window keeps storage bounded while eliminating that 404 race.
        const std::size_t graceSegments = std::max<std::size_t>(3, config_.liveWindowSegments);
        while (retiredSegments_.size() > graceSegments) {
            const SegmentInfo old = retiredSegments_.front();
            retiredSegments_.pop_front();
            std::error_code ec;
            std::filesystem::remove(config_.directory / old.fileName, ec);
        }
        return;
    }
''')

checks = {
    "src/AppVersion.h": ['kProgramVersion = "10.8.73"'],
    "src/StreamManager.cpp": ["std::chrono::seconds(5)", "idle_s=5"],
    "src/media/NativeHlsSegmenter.h": ["retiredSegments_"],
    "src/media/NativeHlsSegmenter.cpp": ["graceSegments", "retiredSegments_.push_back(old)"],
}
for path, needles in checks.items():
    text = Path(path).read_text()
    for needle in needles:
        if needle not in text:
            raise SystemExit(f"{path}: missing expected marker {needle!r}")

print("V10.8.73 HLS grace patch applied")
