from pathlib import Path

version = Path('src/AppVersion.h')
text = version.read_text()
old = 'inline constexpr const char* kProgramVersion = "10.8.97";'
new = 'inline constexpr const char* kProgramVersion = "10.8.98";'
assert old in text
version.write_text(text.replace(old, new, 1))

path = Path('src/media/NativeHlsSegmenter.cpp')
text = path.read_text()
needle = '''    // V10.8.82: keep each elementary PID muted until it reaches a decoder-safe\n    // PES start. V10.8.81 already required MPEG audio to begin on a real frame.\n'''
insert = '''    // V10.8.98: for passthrough AVC/HEVC, do not let audio/SI open the very\n    // first media segment before decoder-safe video has arrived. A standalone\n    // TS can recover from an audio lead, but a live HLS demuxer may finish its\n    // initial stream-info probe with MP2 known and H.264/H.265 still lacking\n    // width/height. Keep PSI and H.26x flowing until the first safe video PES;\n    // other elementary PIDs join from their next normal clean PES start.\n    bool waitingForFirstH26xMedia = false;\n    if (!config_.independentSegments) {\n        bool haveH26x = false;\n        bool h26xStarted = false;\n        for (std::size_t pid = 0; pid < elementaryStreamType_.size(); ++pid) {\n            const std::uint8_t type = elementaryStreamType_[pid];\n            if (type != 0x1bU && type != 0x24U) continue;\n            haveH26x = true;\n            if (firstSegmentPidStarted_[pid]) {\n                h26xStarted = true;\n                break;\n            }\n        }\n        waitingForFirstH26xMedia = haveH26x && !h26xStarted;\n        if (waitingForFirstH26xMedia) {\n            const bool psi = info.pid == 0x0000U || info.pid == pmtPid_;\n            const bool nullPacket = info.pid == mpegts::kNullPid;\n            const std::uint8_t type = info.pid < elementaryStreamType_.size()\n                ? elementaryStreamType_[info.pid]\n                : 0U;\n            const bool h26xVideo = type == 0x1bU || type == 0x24U;\n            if (!psi && !nullPacket && !h26xVideo) return true;\n        }\n    }\n\n    // V10.8.82: keep each elementary PID muted until it reaches a decoder-safe\n    // PES start. V10.8.81 already required MPEG audio to begin on a real frame.\n'''
assert needle in text
text = text.replace(needle, insert, 1)
needle2 = '''            firstSegmentPidStarted_[info.pid] = true;\n        }\n    }\n\n    // V10.8.96: cache decoder configuration only from a complete H.26x PES.\n'''
insert2 = '''            firstSegmentPidStarted_[info.pid] = true;\n        }\n    }\n\n    // The global H.26x startup barrier may have been released by this packet.\n    // Reset the segment clock origin to accepted media instead of retaining a\n    // PCR captured from the packet that merely ended waitingForCleanStart_.\n    if (waitingForFirstH26xMedia &&\n        info.pid < elementaryStreamType_.size() &&\n        (elementaryStreamType_[info.pid] == 0x1bU ||\n         elementaryStreamType_[info.pid] == 0x24U) &&\n        firstSegmentPidStarted_[info.pid]) {\n        if (info.hasPcr) {\n            firstPcr_ = info.pcrBase90k;\n            lastPcr_ = info.pcrBase90k;\n            haveFirstPcr_ = true;\n        } else {\n            haveFirstPcr_ = false;\n        }\n        programTime_ = std::chrono::system_clock::now();\n    }\n\n    // V10.8.96: cache decoder configuration only from a complete H.26x PES.\n'''
assert needle2 in text
text = text.replace(needle2, insert2, 1)
path.write_text(text)
