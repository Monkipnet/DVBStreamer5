from pathlib import Path

app = Path('src/AppVersion.h')
text = app.read_text()
old = 'inline constexpr const char* kProgramVersion = "10.8.98";'
new = 'inline constexpr const char* kProgramVersion = "10.8.99";'
if old not in text:
    raise SystemExit('version marker not found')
app.write_text(text.replace(old, new, 1))

p = Path('src/media/NativeHlsSegmenter.cpp')
text = p.read_text()

# Revert the V10.8.98 video-first startup barrier: the live test proved it can
# produce a tiny 4.6 KB first segment that is not independently probeable.
start = text.find('    // V10.8.98: for passthrough AVC/HEVC')
end = text.find('    // V10.8.82: keep each elementary PID muted', start)
if start < 0 or end < 0:
    raise SystemExit('V10.8.98 barrier block not found')
text = text[:start] + text[end:]

start = text.find('    // The global H.26x startup barrier may have been released by this packet.')
end = text.find('    // V10.8.96: cache decoder configuration only from a complete H.26x PES.', start)
if start < 0 or end < 0:
    raise SystemExit('V10.8.98 PCR reset block not found')
text = text[:start] + text[end:]

old_rotate = '''    liveSegments_.push_back(info);\n    ++completedSegments_;\n    segmentHasPackets_ = false;\n    segmentPath_.clear();\n    prune();\n    if (!writePlaylist(false)) return false;\n    return true;\n'''
new_rotate = '''    liveSegments_.push_back(info);\n    ++completedSegments_;\n    segmentHasPackets_ = false;\n    segmentPath_.clear();\n    prune();\n\n    // V10.8.99: do not publish the initial live manifest for passthrough\n    // H.264/H.265 after only one completed segment. FFmpeg's live HLS stream\n    // probe can lock codec parameters from that first startup segment before\n    // the next decoder-safe segment exists, even though the same two files are\n    // decoded correctly when they are already listed together in a static HLS\n    // playlist. Wait for two finalized H.26x segments before making video.m3u8\n    // visible; later rotations keep the normal rolling playlist cadence.\n    const bool passthroughH26x = !config_.independentSegments &&\n        std::any_of(elementaryStreamType_.begin(), elementaryStreamType_.end(),\n                    [](std::uint8_t type) { return type == 0x1bU || type == 0x24U; });\n    if (passthroughH26x && completedSegments_ < 2U) return true;\n\n    if (!writePlaylist(false)) return false;\n    return true;\n'''
if old_rotate not in text:
    raise SystemExit('rotate tail marker not found')
text = text.replace(old_rotate, new_rotate, 1)
p.write_text(text)
