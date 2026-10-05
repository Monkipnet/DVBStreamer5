from pathlib import Path

root = Path('.')
ver = root / 'src/AppVersion.h'
cpp = root / 'src/media/NativeHlsSegmenter.cpp'
hdr = root / 'src/media/NativeHlsSegmenter.h'

s = ver.read_text()
assert 'kProgramVersion = "10.8.95"' in s
ver.write_text(s.replace('kProgramVersion = "10.8.95"', 'kProgramVersion = "10.8.96"', 1))

s = hdr.read_text()
old = '''    std::uint16_t h26xConfigPid_ = mpegts::kNullPid;\n    std::uint8_t h26xConfigStreamType_ = 0;\n    std::vector<std::uint8_t> h26xParameterSets_;\n'''
new = '''    std::uint16_t h26xConfigPid_ = mpegts::kNullPid;\n    std::uint8_t h26xConfigStreamType_ = 0;\n    std::vector<std::uint8_t> h26xParameterSets_;\n    // V10.8.96: collect one complete source video PES at a time before caching\n    // decoder configuration. Merely seeing a PPS NAL header in the first TS\n    // packet is insufficient because the PPS body may continue in later packets.\n    bool h26xConfigProbeActive_ = false;\n    std::uint16_t h26xConfigProbePid_ = mpegts::kNullPid;\n    std::uint8_t h26xConfigProbeStreamType_ = 0;\n    std::vector<std::uint8_t> h26xConfigProbeElementary_;\n'''
assert old in s
hdr.write_text(s.replace(old, new, 1))

s = cpp.read_text()
old = '''std::vector<std::uint8_t> extractH26xParameterSets(\n    const std::vector<std::uint8_t>& elementary,\n    std::uint8_t streamType) {'''
new = '''std::vector<std::uint8_t> extractH26xParameterSets(\n    const std::vector<std::uint8_t>& elementary,\n    std::uint8_t streamType,\n    bool terminalNalComplete) {'''
assert old in s
s = s.replace(old, new, 1)

old = '''        std::size_t next = nal + 1U;\n        for (; next + 3U <= elementary.size(); ++next) {\n            std::size_t nextPrefix = 0;\n            if (startCode(next, nextPrefix)) break;\n        }\n        if (next > elementary.size()) next = elementary.size();\n'''
new = '''        std::size_t next = nal + 1U;\n        bool foundNext = false;\n        for (; next + 3U <= elementary.size(); ++next) {\n            std::size_t nextPrefix = 0;\n            if (startCode(next, nextPrefix)) {\n                foundNext = true;\n                break;\n            }\n        }\n        if (!foundNext) {\n            // The caller may be probing a still-growing TS/PES buffer. The\n            // terminal NAL is not cacheable until the PES boundary is known.\n            if (!terminalNalComplete) break;\n            next = elementary.size();\n        }\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''    h26xConfigPid_ = mpegts::kNullPid;\n    h26xConfigStreamType_ = 0;\n    h26xParameterSets_.clear();\n'''
new = '''    h26xConfigPid_ = mpegts::kNullPid;\n    h26xConfigStreamType_ = 0;\n    h26xParameterSets_.clear();\n    h26xConfigProbeActive_ = false;\n    h26xConfigProbePid_ = mpegts::kNullPid;\n    h26xConfigProbeStreamType_ = 0;\n    h26xConfigProbeElementary_.clear();\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\n                        // V10.8.95: retain the source's decoder configuration\n                        // separately from the IDR/IRAP picture. Later HLS segments\n                        // can replay only VPS/SPS/PPS without duplicating an old\n                        // frame or timestamp.\n                        std::vector<std::uint8_t> firstElementary(\n                            packet.begin() + static_cast<std::ptrdiff_t>(elementary),\n                            packet.end());\n                        auto parameterSets = extractH26xParameterSets(firstElementary, streamType);\n                        if (!parameterSets.empty()) {\n                            h26xConfigPid_ = info.pid;\n                            h26xConfigStreamType_ = streamType;\n                            h26xParameterSets_ = std::move(parameterSets);\n                        }\n'''
assert old in s
s = s.replace(old, '\n', 1)

marker = '''    // Track PCR continuously, including while a candidate H.26x PES is held\n'''
assert marker in s
probe = '''    // V10.8.96: cache decoder configuration only from a complete H.26x PES.\n    // SPS/PPS/VPS NAL units frequently cross a 188-byte TS packet boundary;\n    // V10.8.95 could therefore replay a truncated PPS even though its NAL type\n    // byte had already been visible in the first packet.\n    const std::uint8_t observedStreamType = info.pid < elementaryStreamType_.size()\n        ? elementaryStreamType_[info.pid]\n        : 0U;\n    const bool observedH26x = observedStreamType == 0x1bU || observedStreamType == 0x24U;\n    if (!config_.independentSegments && observedH26x && info.hasPayload && !info.scrambled) {\n        constexpr std::size_t kMaxConfigProbeBytes = 128U * 1024U;\n\n        auto commitConfigProbe = [this]() {\n            if (!h26xConfigProbeActive_ || h26xConfigProbeElementary_.empty()) return;\n            auto parameterSets = extractH26xParameterSets(\n                h26xConfigProbeElementary_, h26xConfigProbeStreamType_, true);\n            if (!parameterSets.empty()) {\n                h26xConfigPid_ = h26xConfigProbePid_;\n                h26xConfigStreamType_ = h26xConfigProbeStreamType_;\n                h26xParameterSets_ = std::move(parameterSets);\n            }\n        };\n\n        if (info.payloadUnitStart) {\n            if (h26xConfigProbeActive_ && info.pid == h26xConfigProbePid_) {\n                commitConfigProbe();\n            }\n            h26xConfigProbeActive_ = false;\n            h26xConfigProbePid_ = mpegts::kNullPid;\n            h26xConfigProbeStreamType_ = 0;\n            h26xConfigProbeElementary_.clear();\n\n            const std::size_t off = info.payloadOffset;\n            if (off + 9U <= packet.size() &&\n                packet[off] == 0x00U && packet[off + 1U] == 0x00U &&\n                packet[off + 2U] == 0x01U &&\n                packet[off + 3U] >= 0xe0U && packet[off + 3U] <= 0xefU) {\n                const std::size_t begin =\n                    off + 9U + static_cast<std::size_t>(packet[off + 8U]);\n                if (begin <= packet.size()) {\n                    h26xConfigProbeActive_ = true;\n                    h26xConfigProbePid_ = info.pid;\n                    h26xConfigProbeStreamType_ = observedStreamType;\n                    const std::size_t count = std::min(\n                        kMaxConfigProbeBytes, packet.size() - begin);\n                    h26xConfigProbeElementary_.insert(\n                        h26xConfigProbeElementary_.end(),\n                        packet.begin() + static_cast<std::ptrdiff_t>(begin),\n                        packet.begin() + static_cast<std::ptrdiff_t>(begin + count));\n                }\n            }\n        } else if (h26xConfigProbeActive_ && info.pid == h26xConfigProbePid_ &&\n                   info.payloadOffset < packet.size() &&\n                   h26xConfigProbeElementary_.size() < kMaxConfigProbeBytes) {\n            const std::size_t remaining =\n                kMaxConfigProbeBytes - h26xConfigProbeElementary_.size();\n            const std::size_t count = std::min(\n                remaining, packet.size() - info.payloadOffset);\n            h26xConfigProbeElementary_.insert(\n                h26xConfigProbeElementary_.end(),\n                packet.begin() + static_cast<std::ptrdiff_t>(info.payloadOffset),\n                packet.begin() + static_cast<std::ptrdiff_t>(info.payloadOffset + count));\n        }\n    }\n\n'''
s = s.replace(marker, probe + marker, 1)

old = '''            auto currentConfig = extractH26xParameterSets(\n                h26xBoundaryElementary_, h26xBoundaryStreamType_);'''
new = '''            auto currentConfig = extractH26xParameterSets(\n                h26xBoundaryElementary_, h26xBoundaryStreamType_, false);'''
assert old in s
s = s.replace(old, new, 1)

cpp.write_text(s)
