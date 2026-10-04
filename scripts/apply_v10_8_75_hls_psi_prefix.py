from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"pattern not found in {path}: {old[:120]!r}")
    text = text.replace(old, new, 1)
    p.write_text(text)

replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.74";',
    'inline constexpr const char* kProgramVersion = "10.8.75";')

replace_once(
    "src/media/NativeHlsSegmenter.h",
    '#include <string>\n',
    '#include <string>\n#include <vector>\n')

replace_once(
    "src/media/NativeHlsSegmenter.h",
    '    bool appendPacket(const mpegts::Packet& packet);\n    bool rotate(double durationSeconds);\n    bool openSegment();\n',
    '    bool appendPacket(const mpegts::Packet& packet);\n'
    '    void observePsi(const mpegts::Packet& packet, const mpegts::PacketInfo& info);\n'
    '    bool writePsiPrefix();\n'
    '    bool rotate(double durationSeconds);\n'
    '    bool openSegment();\n')

replace_once(
    "src/media/NativeHlsSegmenter.h",
    '    std::deque<SegmentInfo> retiredSegments_;\n    std::uint64_t nextSequence_ = 0;\n',
    '    std::deque<SegmentInfo> retiredSegments_;\n'
    '    // MPEG-TS HLS clients may open every segment with a fresh demuxer.\n'
    '    // Keep the latest complete PAT/PMT repetition and prepend it to each\n'
    '    // subsequent segment so audio/video PIDs are known before media bytes.\n'
    '    std::vector<mpegts::Packet> patCollecting_;\n'
    '    std::vector<mpegts::Packet> patPrefix_;\n'
    '    std::vector<mpegts::Packet> pmtCollecting_;\n'
    '    std::vector<mpegts::Packet> pmtPrefix_;\n'
    '    std::uint16_t pmtPid_ = mpegts::kNullPid;\n'
    '    std::uint64_t nextSequence_ = 0;\n')

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '    liveSegments_.clear();\n    retiredSegments_.clear();\n    nextSequence_ = 0;\n',
    '    liveSegments_.clear();\n'
    '    retiredSegments_.clear();\n'
    '    patCollecting_.clear();\n'
    '    patPrefix_.clear();\n'
    '    pmtCollecting_.clear();\n'
    '    pmtPrefix_.clear();\n'
    '    pmtPid_ = mpegts::kNullPid;\n'
    '    nextSequence_ = 0;\n')

insert_before = 'bool NativeHlsSegmenter::appendPacket(const mpegts::Packet& packet) {\n'
helper = r'''void NativeHlsSegmenter::observePsi(const mpegts::Packet& packet,
                                      const mpegts::PacketInfo& info) {
    constexpr std::size_t kMaxPsiPackets = 32;
    if (!info.hasPayload || info.payloadOffset >= mpegts::kPacketSize) return;

    if (info.pid == 0x0000U) {
        if (info.payloadUnitStart) {
            if (!patCollecting_.empty()) patPrefix_ = patCollecting_;
            patCollecting_.clear();

            const std::uint8_t* payload = packet.data() + info.payloadOffset;
            const std::size_t payloadSize = mpegts::kPacketSize - info.payloadOffset;
            if (payloadSize >= 1U) {
                const std::size_t sectionStart = 1U + static_cast<std::size_t>(payload[0]);
                if (sectionStart + 12U <= payloadSize && payload[sectionStart] == 0x00U) {
                    const std::size_t sectionLength =
                        (static_cast<std::size_t>(payload[sectionStart + 1U] & 0x0fU) << 8U) |
                        static_cast<std::size_t>(payload[sectionStart + 2U]);
                    const std::size_t sectionSize = 3U + sectionLength;
                    if (sectionSize >= 12U && sectionStart + sectionSize <= payloadSize) {
                        const std::size_t entriesEnd = sectionStart + sectionSize - 4U;
                        std::uint16_t discovered = mpegts::kNullPid;
                        for (std::size_t off = sectionStart + 8U; off + 4U <= entriesEnd; off += 4U) {
                            const std::uint16_t service = static_cast<std::uint16_t>(
                                (static_cast<std::uint16_t>(payload[off]) << 8U) |
                                payload[off + 1U]);
                            const std::uint16_t pid = static_cast<std::uint16_t>(
                                (static_cast<std::uint16_t>(payload[off + 2U] & 0x1fU) << 8U) |
                                payload[off + 3U]);
                            if (service != 0U && pid < mpegts::kNullPid) {
                                discovered = pid;
                                break;
                            }
                        }
                        if (discovered != mpegts::kNullPid && discovered != pmtPid_) {
                            pmtPid_ = discovered;
                            pmtCollecting_.clear();
                            pmtPrefix_.clear();
                        }
                    }
                }
            }
        }
        if ((info.payloadUnitStart || !patCollecting_.empty()) &&
            patCollecting_.size() < kMaxPsiPackets) {
            patCollecting_.push_back(packet);
        }
        return;
    }

    if (pmtPid_ != mpegts::kNullPid && info.pid == pmtPid_) {
        if (info.payloadUnitStart) {
            if (!pmtCollecting_.empty()) pmtPrefix_ = pmtCollecting_;
            pmtCollecting_.clear();
        }
        if ((info.payloadUnitStart || !pmtCollecting_.empty()) &&
            pmtCollecting_.size() < kMaxPsiPackets) {
            pmtCollecting_.push_back(packet);
        }
    }
}

bool NativeHlsSegmenter::writePsiPrefix() {
    auto writePackets = [this](const std::vector<mpegts::Packet>& packets) {
        for (const auto& packet : packets) {
            segment_.write(reinterpret_cast<const char*>(packet.data()),
                           static_cast<std::streamsize>(packet.size()));
            if (!segment_) return false;
        }
        return true;
    };
    if (!patPrefix_.empty() && !writePackets(patPrefix_)) return false;
    if (!pmtPrefix_.empty() && !writePackets(pmtPrefix_)) return false;
    return true;
}

'''
replace_once("src/media/NativeHlsSegmenter.cpp", insert_before, helper + insert_before)

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '    if (!mpegts::inspectPacket(packet.data(), packet.size(), info)) return true;\n\n    if (waitingForIndependentStart_) {\n',
    '    if (!mpegts::inspectPacket(packet.data(), packet.size(), info)) return true;\n'
    '    observePsi(packet, info);\n\n'
    '    if (waitingForIndependentStart_) {\n')

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '        const bool targetReached = segmentHasPackets_ && elapsed >= config_.targetDurationSeconds;\n'
    '        const bool hardLimit = !config_.independentSegments &&\n'
    '            segmentHasPackets_ && elapsed >= config_.targetDurationSeconds * 3.0;\n'
    '        if (targetReached && (info.randomAccess || hardLimit)) {\n',
    '        const bool targetReached = segmentHasPackets_ && elapsed >= config_.targetDurationSeconds;\n'
    '        // DVB passthrough encoders do not always set random_access_indicator.\n'
    '        // Prefer a PCR-bearing PES boundary instead of waiting six seconds\n'
    '        // and then cutting at an arbitrary PCR in the middle of transport.\n'
    '        const bool passthroughPesBoundary = !config_.independentSegments && info.payloadUnitStart;\n'
    '        const bool hardLimit = !config_.independentSegments &&\n'
    '            segmentHasPackets_ && elapsed >= config_.targetDurationSeconds * 3.0;\n'
    '        if (targetReached && (info.randomAccess || passthroughPesBoundary || hardLimit)) {\n')

replace_once(
    "src/media/NativeHlsSegmenter.cpp",
    '    segment_.open(segmentPath_, std::ios::binary | std::ios::trunc);\n'
    '    if (!segment_.is_open()) { fail("failed to create HLS segment " + segmentPath_.string()); return false; }\n'
    '    return true;\n',
    '    segment_.open(segmentPath_, std::ios::binary | std::ios::trunc);\n'
    '    if (!segment_.is_open()) { fail("failed to create HLS segment " + segmentPath_.string()); return false; }\n'
    '    // The first segment naturally starts with remapper PSI. For every later\n'
    '    // segment prepend the last complete PAT/PMT repetition so a fresh HLS\n'
    '    // demuxer never sees audio/video packets before program metadata.\n'
    '    if (completedSegments_ > 0 && !writePsiPrefix()) {\n'
    '        fail("failed to write HLS PAT/PMT prefix");\n'
    '        return false;\n'
    '    }\n'
    '    return true;\n')

print("V10.8.75 patch applied")
