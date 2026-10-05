from pathlib import Path

ver = Path('src/AppVersion.h')
s = ver.read_text()
s = s.replace('kProgramVersion = "10.8.93"', 'kProgramVersion = "10.8.94"')
ver.write_text(s)

hdr = Path('src/media/NativeHlsSegmenter.h')
s = hdr.read_text()
needle = '    std::array<bool, 8192> firstSegmentPidStarted_{};\n'
insert = '''    std::array<bool, 8192> firstSegmentPidStarted_{};
    // V10.8.94: once the target duration is reached, hold the beginning of a
    // passthrough AVC/HEVC PES until its first VCL NAL is known. Broadcast
    // encoders frequently place the IDR/IRAP NAL in the second or later TS
    // packet and omit random_access_indicator, so a one-packet probe cannot
    // choose a decoder-safe segment boundary.
    bool h26xCutArmed_ = false;
    bool h26xBoundaryPending_ = false;
    std::uint16_t h26xBoundaryPid_ = mpegts::kNullPid;
    std::uint8_t h26xBoundaryStreamType_ = 0;
    double h26xBoundaryDuration_ = 0.0;
    std::uint64_t h26xBoundaryPcr_ = 0;
    std::vector<mpegts::Packet> h26xBoundaryPackets_;
    std::vector<std::uint8_t> h26xBoundaryElementary_;
'''
assert needle in s
s = s.replace(needle, insert, 1)
hdr.write_text(s)

cpp = Path('src/media/NativeHlsSegmenter.cpp')
s = cpp.read_text()

ns_needle = '} // namespace\n\nnamespace dvbstreamer5::media::hls {\n'
helper = '''enum class H26xAccessUnitKind {
    Undetermined,
    RandomAccess,
    NonRandomAccess
};

H26xAccessUnitKind classifyH26xAccessUnit(
    const std::vector<std::uint8_t>& elementary,
    std::uint8_t streamType) {
    for (std::size_t pos = 0; pos + 4U <= elementary.size(); ++pos) {
        std::size_t nal = elementary.size();
        if (elementary[pos] == 0x00U && elementary[pos + 1U] == 0x00U &&
            elementary[pos + 2U] == 0x01U) {
            nal = pos + 3U;
        } else if (pos + 5U <= elementary.size() &&
                   elementary[pos] == 0x00U && elementary[pos + 1U] == 0x00U &&
                   elementary[pos + 2U] == 0x00U && elementary[pos + 3U] == 0x01U) {
            nal = pos + 4U;
        }
        if (nal >= elementary.size()) continue;

        if (streamType == 0x1bU) {
            const std::uint8_t nalType = elementary[nal] & 0x1fU;
            // AVC VCL NALs are types 1..5. Type 5 is IDR.
            if (nalType >= 1U && nalType <= 5U) {
                return nalType == 5U
                    ? H26xAccessUnitKind::RandomAccess
                    : H26xAccessUnitKind::NonRandomAccess;
            }
        } else if (streamType == 0x24U) {
            const std::uint8_t nalType = (elementary[nal] >> 1U) & 0x3fU;
            // HEVC VCL NALs are types 0..31; IRAP is 16..23.
            if (nalType <= 31U) {
                return (nalType >= 16U && nalType <= 23U)
                    ? H26xAccessUnitKind::RandomAccess
                    : H26xAccessUnitKind::NonRandomAccess;
            }
        }
    }
    return H26xAccessUnitKind::Undetermined;
}

} // namespace

namespace dvbstreamer5::media::hls {
'''
assert ns_needle in s
s = s.replace(ns_needle, helper, 1)

reset_needle = '    firstSegmentPidStarted_.fill(false);\n    lastError_.clear();\n'
reset_insert = '''    firstSegmentPidStarted_.fill(false);
    h26xCutArmed_ = false;
    h26xBoundaryPending_ = false;
    h26xBoundaryPid_ = mpegts::kNullPid;
    h26xBoundaryStreamType_ = 0;
    h26xBoundaryDuration_ = 0.0;
    h26xBoundaryPcr_ = 0;
    h26xBoundaryPackets_.clear();
    h26xBoundaryElementary_.clear();
    lastError_.clear();
'''
assert reset_needle in s
s = s.replace(reset_needle, reset_insert, 1)

start = s.index('    if (info.hasPcr) {\n        lastPcr_ = info.pcrBase90k;')
end = s.index('    if (!segment_.is_open() && !openSegment()) return false;', start)
new_rotation = r'''    // Track PCR continuously, including while a candidate H.26x PES is held
    // in the boundary buffer. If that candidate becomes the next segment, its
    // first PCR becomes the new segment clock origin.
    if (info.hasPcr) {
        lastPcr_ = info.pcrBase90k;
        if (!haveFirstPcr_) {
            firstPcr_ = lastPcr_;
            haveFirstPcr_ = true;
        }
    }

    const bool hasH26xVideo = std::any_of(
        elementaryStreamType_.begin(), elementaryStreamType_.end(),
        [](std::uint8_t type) { return type == 0x1bU || type == 0x24U; });
    const double segmentTarget = completedSegments_ == 0
        ? std::min(config_.targetDurationSeconds, 1.0)
        : config_.targetDurationSeconds;
    const double elapsed = haveFirstPcr_
        ? pcrDeltaSeconds(firstPcr_, lastPcr_)
        : 0.0;
    const bool targetReached = segmentHasPackets_ && haveFirstPcr_ &&
        elapsed >= segmentTarget;

    auto writeBufferedPacket = [this](const mpegts::Packet& buffered) -> bool {
        if (!segment_.is_open() && !openSegment()) return false;
        segment_.write(reinterpret_cast<const char*>(buffered.data()),
                       static_cast<std::streamsize>(buffered.size()));
        if (!segment_) {
            fail("failed to write buffered H.26x HLS packet");
            return false;
        }
        segmentHasPackets_ = true;
        return true;
    };

    auto clearBoundary = [this]() {
        h26xBoundaryPending_ = false;
        h26xBoundaryPid_ = mpegts::kNullPid;
        h26xBoundaryStreamType_ = 0;
        h26xBoundaryDuration_ = 0.0;
        h26xBoundaryPcr_ = 0;
        h26xBoundaryPackets_.clear();
        h26xBoundaryElementary_.clear();
    };

    auto flushBoundary = [&](bool randomAccess) -> bool {
        if (randomAccess) {
            if (!rotate(std::max(0.001, h26xBoundaryDuration_))) return false;
            firstPcr_ = h26xBoundaryPcr_;
            haveFirstPcr_ = true;
        }
        for (const auto& buffered : h26xBoundaryPackets_) {
            if (!writeBufferedPacket(buffered)) return false;
        }
        clearBoundary();
        if (randomAccess) h26xCutArmed_ = false;
        return true;
    };

    auto appendBoundaryElementary = [this](const mpegts::Packet& candidate,
                                            const mpegts::PacketInfo& candidateInfo,
                                            bool pesStart) {
        if (candidateInfo.pid != h26xBoundaryPid_ || !candidateInfo.hasPayload ||
            candidateInfo.payloadOffset >= mpegts::kPacketSize) {
            return;
        }
        std::size_t begin = candidateInfo.payloadOffset;
        if (pesStart) {
            if (begin + 9U > candidate.size() ||
                candidate[begin] != 0x00U || candidate[begin + 1U] != 0x00U ||
                candidate[begin + 2U] != 0x01U ||
                candidate[begin + 3U] < 0xe0U || candidate[begin + 3U] > 0xefU) {
                return;
            }
            begin += 9U + static_cast<std::size_t>(candidate[begin + 8U]);
            if (begin > candidate.size()) return;
        }
        constexpr std::size_t kMaxProbeBytes = 512U * 1024U;
        if (begin < candidate.size() && h26xBoundaryElementary_.size() < kMaxProbeBytes) {
            const std::size_t remaining = kMaxProbeBytes - h26xBoundaryElementary_.size();
            const std::size_t count = std::min(remaining, candidate.size() - begin);
            h26xBoundaryElementary_.insert(
                h26xBoundaryElementary_.end(),
                candidate.begin() + static_cast<std::ptrdiff_t>(begin),
                candidate.begin() + static_cast<std::ptrdiff_t>(begin + count));
        }
    };

    if (!config_.independentSegments && hasH26xVideo) {
        if (targetReached) h26xCutArmed_ = true;

        // A pending candidate owns every TS packet from its video PES start
        // until the first VCL NAL is identified. Buffering interleaved audio/PSI
        // preserves packet order if this PES becomes the start of the next segment.
        if (h26xBoundaryPending_) {
            const bool nextVideoPes = info.pid == h26xBoundaryPid_ &&
                info.payloadUnitStart && !info.scrambled;
            if (nextVideoPes) {
                // No VCL decision was found in the previous PES. Keep it in the
                // current segment and immediately evaluate this new PES instead.
                if (!flushBoundary(false)) return false;
                h26xCutArmed_ = true;
            } else {
                h26xBoundaryPackets_.push_back(packet);
                appendBoundaryElementary(packet, info, false);
                const auto kind = classifyH26xAccessUnit(
                    h26xBoundaryElementary_, h26xBoundaryStreamType_);
                if (kind == H26xAccessUnitKind::RandomAccess) {
                    return flushBoundary(true);
                }
                if (kind == H26xAccessUnitKind::NonRandomAccess ||
                    h26xBoundaryPackets_.size() >= 2048U ||
                    h26xBoundaryElementary_.size() >= 512U * 1024U) {
                    return flushBoundary(false);
                }
                return true;
            }
        }

        const std::uint8_t streamType = info.pid < elementaryStreamType_.size()
            ? elementaryStreamType_[info.pid]
            : 0U;
        const bool h26xVideoPesStart = h26xCutArmed_ && info.payloadUnitStart &&
            info.hasPayload && !info.scrambled &&
            (streamType == 0x1bU || streamType == 0x24U);
        if (h26xVideoPesStart) {
            const std::size_t off = info.payloadOffset;
            const bool videoPes = off + 9U <= packet.size() &&
                packet[off] == 0x00U && packet[off + 1U] == 0x00U &&
                packet[off + 2U] == 0x01U &&
                packet[off + 3U] >= 0xe0U && packet[off + 3U] <= 0xefU;
            if (videoPes) {
                h26xBoundaryPending_ = true;
                h26xBoundaryPid_ = info.pid;
                h26xBoundaryStreamType_ = streamType;
                h26xBoundaryDuration_ = std::max(0.001, elapsed);
                h26xBoundaryPcr_ = info.hasPcr ? info.pcrBase90k : lastPcr_;
                h26xBoundaryPackets_.clear();
                h26xBoundaryElementary_.clear();
                h26xBoundaryPackets_.push_back(packet);
                appendBoundaryElementary(packet, info, true);

                if (info.randomAccess) {
                    return flushBoundary(true);
                }
                const auto kind = classifyH26xAccessUnit(
                    h26xBoundaryElementary_, h26xBoundaryStreamType_);
                if (kind == H26xAccessUnitKind::RandomAccess) {
                    return flushBoundary(true);
                }
                if (kind == H26xAccessUnitKind::NonRandomAccess) {
                    return flushBoundary(false);
                }
                return true;
            }
        }
    } else if (info.hasPcr) {
        // ABR/transcoded HLS already carries random_access_indicator. Legacy
        // non-H.26x passthrough keeps its established PES-boundary fallback.
        const bool hardLimit = !config_.independentSegments &&
            segmentHasPackets_ && elapsed >= segmentTarget * 3.0;
        const bool rotateBoundary = config_.independentSegments
            ? info.randomAccess
            : (info.payloadUnitStart || hardLimit);
        if (targetReached && rotateBoundary) {
            if (!rotate(std::max(0.001, elapsed))) return false;
            firstPcr_ = lastPcr_;
            haveFirstPcr_ = true;
        }
    }
'''
s = s[:start] + new_rotation + s[end:]

stop_needle = '''void NativeHlsSegmenter::stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    if (segmentHasPackets_ && segment_.is_open()) {
'''
stop_insert = '''void NativeHlsSegmenter::stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    // Do not discard a partially probed H.26x PES on shutdown. It belongs to
    // the current segment unless a keyframe decision already rotated it.
    if (h26xBoundaryPending_ && !h26xBoundaryPackets_.empty()) {
        if (!segment_.is_open()) (void)openSegment();
        if (segment_.is_open()) {
            for (const auto& buffered : h26xBoundaryPackets_) {
                segment_.write(reinterpret_cast<const char*>(buffered.data()),
                               static_cast<std::streamsize>(buffered.size()));
                if (!segment_) break;
                segmentHasPackets_ = true;
            }
        }
        h26xBoundaryPending_ = false;
        h26xBoundaryPackets_.clear();
        h26xBoundaryElementary_.clear();
    }
    if (segmentHasPackets_ && segment_.is_open()) {
'''
assert stop_needle in s
s = s.replace(stop_needle, stop_insert, 1)
cpp.write_text(s)
