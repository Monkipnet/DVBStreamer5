from pathlib import Path

seg = Path('src/media/NativeHlsSegmenter.cpp')
text = seg.read_text()
marker = '// V10.8.89: an H.264/H.265 HLS segment must not start on an arbitrary'
idx = text.find(marker)
if idx < 0:
    raise SystemExit('V10.8.89 marker not found')
head = text[:idx]
tail = text[idx:]
old = '''                        bool haveVps = false;
                        bool haveSps = false;
                        bool havePps = false;
                        bool haveRandomAccess = info.randomAccess;
                        for (std::size_t pos = elementary; pos + 4U <= packet.size(); ++pos) {
                            std::size_t nal = packet.size();
                            if (packet[pos] == 0x00U && packet[pos + 1U] == 0x00U &&
                                packet[pos + 2U] == 0x01U) {
                                nal = pos + 3U;
                            } else if (pos + 5U <= packet.size() &&
                                       packet[pos] == 0x00U && packet[pos + 1U] == 0x00U &&
                                       packet[pos + 2U] == 0x00U && packet[pos + 3U] == 0x01U) {
                                nal = pos + 4U;
                            }
                            if (nal >= packet.size()) continue;
                            if (streamType == 0x1bU) {
                                const std::uint8_t nalType = packet[nal] & 0x1fU;
                                if (nalType == 7U) haveSps = true;
                                else if (nalType == 8U) havePps = true;
                                else if (nalType == 5U) haveRandomAccess = true;
                            } else {
                                const std::uint8_t nalType = (packet[nal] >> 1U) & 0x3fU;
                                if (nalType == 32U) haveVps = true;
                                else if (nalType == 33U) haveSps = true;
                                else if (nalType == 34U) havePps = true;
                                else if (nalType >= 16U && nalType <= 23U) haveRandomAccess = true;
                            }
                        }
                        h26xSafeBoundary = streamType == 0x1bU
                            ? (haveSps && havePps && haveRandomAccess)
                            : (haveVps && haveSps && havePps && haveRandomAccess);
'''
new = '''                        // V10.8.93: codec parameter sets are mandatory only for the
                        // initial cold-start gate above. Many broadcast AVC/HEVC
                        // encoders send SPS/PPS (and VPS for HEVC) once at startup
                        // rather than before every keyframe. Requiring them on every
                        // rotation can therefore prevent segment0 from ever closing.
                        // After the first segment has established decoder config,
                        // rotate only on a random-access access unit: IDR for AVC,
                        // IRAP for HEVC, or the TS random_access_indicator.
                        bool haveRandomAccess = info.randomAccess;
                        for (std::size_t pos = elementary; pos + 4U <= packet.size(); ++pos) {
                            std::size_t nal = packet.size();
                            if (packet[pos] == 0x00U && packet[pos + 1U] == 0x00U &&
                                packet[pos + 2U] == 0x01U) {
                                nal = pos + 3U;
                            } else if (pos + 5U <= packet.size() &&
                                       packet[pos] == 0x00U && packet[pos + 1U] == 0x00U &&
                                       packet[pos + 2U] == 0x00U && packet[pos + 3U] == 0x01U) {
                                nal = pos + 4U;
                            }
                            if (nal >= packet.size()) continue;
                            if (streamType == 0x1bU) {
                                if ((packet[nal] & 0x1fU) == 5U) haveRandomAccess = true;
                            } else {
                                const std::uint8_t nalType = (packet[nal] >> 1U) & 0x3fU;
                                if (nalType >= 16U && nalType <= 23U) haveRandomAccess = true;
                            }
                        }
                        h26xSafeBoundary = haveRandomAccess;
'''
if old not in tail:
    raise SystemExit('steady-state H26x boundary block not found')
tail = tail.replace(old, new, 1)
text = head + tail
seg.write_text(text)

ver = Path('src/AppVersion.h')
vtext = ver.read_text()
oldv = 'inline constexpr const char* kProgramVersion = "10.8.92";'
newv = 'inline constexpr const char* kProgramVersion = "10.8.93";'
if oldv not in vtext:
    raise SystemExit('version 10.8.92 not found')
ver.write_text(vtext.replace(oldv, newv, 1))
