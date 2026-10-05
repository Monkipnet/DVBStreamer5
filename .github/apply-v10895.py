from pathlib import Path

ver = Path('src/AppVersion.h')
s = ver.read_text()
s = s.replace('kProgramVersion = "10.8.94"', 'kProgramVersion = "10.8.95"')
ver.write_text(s)

hdr = Path('src/media/NativeHlsSegmenter.h')
s = hdr.read_text()
needle = '''    std::vector<mpegts::Packet> h26xBoundaryPackets_;\n    std::vector<std::uint8_t> h26xBoundaryElementary_;\n'''
insert = '''    std::vector<mpegts::Packet> h26xBoundaryPackets_;
    std::vector<std::uint8_t> h26xBoundaryElementary_;
    // V10.8.95: cache only Annex-B decoder configuration NAL units from the
    // source (SPS/PPS for AVC, VPS/SPS/PPS for HEVC). Some broadcast encoders
    // do not repeat these before later IDR/IRAP frames, so fresh HLS segments
    // otherwise expose a video PID but no width/height to a new decoder.
    std::uint16_t h26xConfigPid_ = mpegts::kNullPid;
    std::uint8_t h26xConfigStreamType_ = 0;
    std::vector<std::uint8_t> h26xParameterSets_;
'''
assert needle in s
s = s.replace(needle, insert, 1)
hdr.write_text(s)

cpp = Path('src/media/NativeHlsSegmenter.cpp')
s = cpp.read_text()

helper_needle = '''H26xAccessUnitKind classifyH26xAccessUnit(
    const std::vector<std::uint8_t>& elementary,
    std::uint8_t streamType) {
'''
assert helper_needle in s
start = s.index(helper_needle)
end_marker = '''    return H26xAccessUnitKind::Undetermined;\n}\n\n} // namespace\n'''
end = s.index(end_marker, start)
insert_pos = end + len('''    return H26xAccessUnitKind::Undetermined;\n}\n\n''')
helpers = r'''std::vector<std::uint8_t> extractH26xParameterSets(
    const std::vector<std::uint8_t>& elementary,
    std::uint8_t streamType) {
    std::vector<std::uint8_t> out;
    bool haveVps = false;
    bool haveSps = false;
    bool havePps = false;

    auto startCode = [&](std::size_t pos, std::size_t& prefixSize) -> bool {
        prefixSize = 0;
        if (pos + 3U <= elementary.size() &&
            elementary[pos] == 0x00U && elementary[pos + 1U] == 0x00U &&
            elementary[pos + 2U] == 0x01U) {
            prefixSize = 3U;
            return true;
        }
        if (pos + 4U <= elementary.size() &&
            elementary[pos] == 0x00U && elementary[pos + 1U] == 0x00U &&
            elementary[pos + 2U] == 0x00U && elementary[pos + 3U] == 0x01U) {
            prefixSize = 4U;
            return true;
        }
        return false;
    };

    std::size_t pos = 0;
    while (pos + 4U <= elementary.size()) {
        std::size_t prefix = 0;
        if (!startCode(pos, prefix)) {
            ++pos;
            continue;
        }
        const std::size_t nal = pos + prefix;
        if (nal >= elementary.size()) break;

        std::size_t next = nal + 1U;
        for (; next + 3U <= elementary.size(); ++next) {
            std::size_t nextPrefix = 0;
            if (startCode(next, nextPrefix)) break;
        }
        if (next > elementary.size()) next = elementary.size();

        bool wanted = false;
        if (streamType == 0x1bU) {
            const std::uint8_t nalType = elementary[nal] & 0x1fU;
            if (nalType == 7U) { wanted = true; haveSps = true; }
            else if (nalType == 8U) { wanted = true; havePps = true; }
        } else if (streamType == 0x24U) {
            const std::uint8_t nalType = (elementary[nal] >> 1U) & 0x3fU;
            if (nalType == 32U) { wanted = true; haveVps = true; }
            else if (nalType == 33U) { wanted = true; haveSps = true; }
            else if (nalType == 34U) { wanted = true; havePps = true; }
        }
        if (wanted && next > pos) {
            out.insert(out.end(),
                       elementary.begin() + static_cast<std::ptrdiff_t>(pos),
                       elementary.begin() + static_cast<std::ptrdiff_t>(next));
        }
        pos = next;
    }

    const bool complete = streamType == 0x1bU
        ? (haveSps && havePps)
        : (haveVps && haveSps && havePps);
    if (!complete) out.clear();
    return out;
}

'''
s = s[:insert_pos] + helpers + s[insert_pos:]

reset_needle = '''    h26xBoundaryPackets_.clear();
    h26xBoundaryElementary_.clear();
    lastError_.clear();
'''
reset_insert = '''    h26xBoundaryPackets_.clear();
    h26xBoundaryElementary_.clear();
    h26xConfigPid_ = mpegts::kNullPid;
    h26xConfigStreamType_ = 0;
    h26xParameterSets_.clear();
    lastError_.clear();
'''
assert reset_needle in s
s = s.replace(reset_needle, reset_insert, 1)

cache_needle = '''                        const bool decoderConfigReady = streamType == 0x1bU
                            ? (haveSps && havePps && haveRandomAccess)
                            : (haveVps && haveSps && havePps && haveRandomAccess);
                        if (!decoderConfigReady) return true;
'''
cache_insert = '''                        const bool decoderConfigReady = streamType == 0x1bU
                            ? (haveSps && havePps && haveRandomAccess)
                            : (haveVps && haveSps && havePps && haveRandomAccess);
                        if (!decoderConfigReady) return true;

                        // V10.8.95: retain the source's decoder configuration
                        // separately from the IDR/IRAP picture. Later HLS segments
                        // can replay only VPS/SPS/PPS without duplicating an old
                        // frame or timestamp.
                        std::vector<std::uint8_t> firstElementary(
                            packet.begin() + static_cast<std::ptrdiff_t>(elementary),
                            packet.end());
                        auto parameterSets = extractH26xParameterSets(firstElementary, streamType);
                        if (!parameterSets.empty()) {
                            h26xConfigPid_ = info.pid;
                            h26xConfigStreamType_ = streamType;
                            h26xParameterSets_ = std::move(parameterSets);
                        }
'''
assert cache_needle in s
s = s.replace(cache_needle, cache_insert, 1)

write_needle = '''    auto clearBoundary = [this]() {
        h26xBoundaryPending_ = false;
'''
assert write_needle in s
write_prefix = r'''    auto writeH26xConfigPrefix = [&]() -> bool {
        if (h26xParameterSets_.empty() ||
            h26xConfigPid_ != h26xBoundaryPid_ ||
            h26xConfigStreamType_ != h26xBoundaryStreamType_ ||
            h26xBoundaryPackets_.empty()) {
            return true;
        }

        mpegts::PacketInfo firstInfo;
        if (!mpegts::inspectPacket(h26xBoundaryPackets_.front().data(),
                                   h26xBoundaryPackets_.front().size(), firstInfo) ||
            firstInfo.pid != h26xBoundaryPid_) {
            return true;
        }

        // Build a short video PES containing only decoder configuration NALs.
        // It carries no PTS/DTS; the following real IDR/IRAP PES keeps the
        // source timestamps. This avoids duplicating an old picture.
        std::vector<std::uint8_t> pes;
        pes.reserve(9U + h26xParameterSets_.size());
        pes.push_back(0x00U);
        pes.push_back(0x00U);
        pes.push_back(0x01U);
        pes.push_back(0xe0U);
        const std::size_t afterLength = 3U + h26xParameterSets_.size();
        const std::uint16_t pesLength = afterLength <= 0xffffU
            ? static_cast<std::uint16_t>(afterLength)
            : 0U;
        pes.push_back(static_cast<std::uint8_t>(pesLength >> 8U));
        pes.push_back(static_cast<std::uint8_t>(pesLength));
        pes.push_back(0x80U);
        pes.push_back(0x00U);
        pes.push_back(0x00U);
        pes.insert(pes.end(), h26xParameterSets_.begin(), h26xParameterSets_.end());

        constexpr std::size_t kPayloadMax = mpegts::kPacketSize - 4U;
        const std::size_t packetCount = (pes.size() + kPayloadMax - 1U) / kPayloadMax;
        if (packetCount == 0U) return true;
        std::uint8_t continuity = static_cast<std::uint8_t>(
            (static_cast<unsigned>(firstInfo.continuityCounter) + 16U -
             static_cast<unsigned>(packetCount & 0x0fU)) & 0x0fU);

        std::size_t offset = 0;
        for (std::size_t index = 0; index < packetCount; ++index) {
            const std::size_t payload = std::min(kPayloadMax, pes.size() - offset);
            mpegts::Packet prefix{};
            prefix.fill(0xffU);
            prefix[0] = mpegts::kSyncByte;
            prefix[1] = static_cast<std::uint8_t>(
                (index == 0U ? 0x40U : 0x00U) |
                ((h26xBoundaryPid_ >> 8U) & 0x1fU));
            prefix[2] = static_cast<std::uint8_t>(h26xBoundaryPid_);

            std::size_t payloadOffset = 4U;
            if (payload == kPayloadMax) {
                prefix[3] = static_cast<std::uint8_t>(0x10U | continuity);
            } else {
                prefix[3] = static_cast<std::uint8_t>(0x30U | continuity);
                const std::size_t adaptationLength = 183U - payload;
                prefix[4] = static_cast<std::uint8_t>(adaptationLength);
                if (adaptationLength > 0U) prefix[5] = 0x00U;
                payloadOffset = 5U + adaptationLength;
            }
            continuity = static_cast<std::uint8_t>((continuity + 1U) & 0x0fU);
            std::copy_n(pes.begin() + static_cast<std::ptrdiff_t>(offset),
                        payload,
                        prefix.begin() + static_cast<std::ptrdiff_t>(payloadOffset));
            offset += payload;
            if (!writeBufferedPacket(prefix)) return false;
        }
        return true;
    };

'''
s = s.replace(write_needle, write_prefix + write_needle, 1)

flush_old = '''    auto flushBoundary = [&](bool randomAccess) -> bool {
        if (randomAccess) {
            if (!rotate(std::max(0.001, h26xBoundaryDuration_))) return false;
            firstPcr_ = h26xBoundaryPcr_;
            haveFirstPcr_ = true;
        }
        for (const auto& buffered : h26xBoundaryPackets_) {
            if (!writeBufferedPacket(buffered)) return false;
        }
'''
flush_new = '''    auto flushBoundary = [&](bool randomAccess) -> bool {
        bool candidateCarriesConfig = false;
        if (randomAccess) {
            auto currentConfig = extractH26xParameterSets(
                h26xBoundaryElementary_, h26xBoundaryStreamType_);
            if (!currentConfig.empty()) {
                h26xConfigPid_ = h26xBoundaryPid_;
                h26xConfigStreamType_ = h26xBoundaryStreamType_;
                h26xParameterSets_ = std::move(currentConfig);
                candidateCarriesConfig = true;
            }
            if (!rotate(std::max(0.001, h26xBoundaryDuration_))) return false;
            firstPcr_ = h26xBoundaryPcr_;
            haveFirstPcr_ = true;
            if (!candidateCarriesConfig && !writeH26xConfigPrefix()) return false;
        }
        for (const auto& buffered : h26xBoundaryPackets_) {
            if (!writeBufferedPacket(buffered)) return false;
        }
'''
assert flush_old in s
s = s.replace(flush_old, flush_new, 1)

cpp.write_text(s)
