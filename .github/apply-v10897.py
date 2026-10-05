from pathlib import Path

version = Path('src/AppVersion.h')
text = version.read_text()
old = 'inline constexpr const char* kProgramVersion = "10.8.96";'
new = 'inline constexpr const char* kProgramVersion = "10.8.97";'
assert old in text
version.write_text(text.replace(old, new, 1))

cpp = Path('src/media/NativeHlsSegmenter.cpp')
text = cpp.read_text()
old = '''        constexpr std::size_t kPayloadMax = mpegts::kPacketSize - 4U;
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
'''
new = '''        constexpr std::size_t kPayloadMax = mpegts::kPacketSize - 4U;
        // V10.8.97: the synthetic decoder-config PES consumes continuity
        // counters that do not exist in the source stream. A player that keeps
        // MPEG-TS PID state across HLS segments can otherwise reject these
        // packets as duplicate/out-of-order when the counter appears to move
        // backwards at the segment boundary. Force an adaptation field on the
        // first synthetic packet and set discontinuity_indicator so the demuxer
        // explicitly resets continuity before accepting SPS/PPS/VPS.
        constexpr std::size_t kFirstPayloadMax = mpegts::kPacketSize - 6U;
        const std::size_t firstPayload = std::min(kFirstPayloadMax, pes.size());
        const std::size_t remainingPayload = pes.size() - firstPayload;
        const std::size_t packetCount = 1U +
            (remainingPayload + kPayloadMax - 1U) / kPayloadMax;
        std::uint8_t continuity = static_cast<std::uint8_t>(
            (static_cast<unsigned>(firstInfo.continuityCounter) + 16U -
             static_cast<unsigned>(packetCount & 0x0fU)) & 0x0fU);

        std::size_t offset = 0;
        for (std::size_t index = 0; index < packetCount; ++index) {
            const bool discontinuity = index == 0U;
            const std::size_t capacity = discontinuity ? kFirstPayloadMax : kPayloadMax;
            const std::size_t payload = std::min(capacity, pes.size() - offset);
            mpegts::Packet prefix{};
            prefix.fill(0xffU);
            prefix[0] = mpegts::kSyncByte;
            prefix[1] = static_cast<std::uint8_t>(
                (index == 0U ? 0x40U : 0x00U) |
                ((h26xBoundaryPid_ >> 8U) & 0x1fU));
            prefix[2] = static_cast<std::uint8_t>(h26xBoundaryPid_);

            std::size_t payloadOffset = 4U;
            if (!discontinuity && payload == kPayloadMax) {
                prefix[3] = static_cast<std::uint8_t>(0x10U | continuity);
            } else {
                prefix[3] = static_cast<std::uint8_t>(0x30U | continuity);
                const std::size_t adaptationLength = 183U - payload;
                prefix[4] = static_cast<std::uint8_t>(adaptationLength);
                if (adaptationLength > 0U) {
                    prefix[5] = discontinuity ? 0x80U : 0x00U;
                }
                payloadOffset = 5U + adaptationLength;
            }
            continuity = static_cast<std::uint8_t>((continuity + 1U) & 0x0fU);
            std::copy_n(pes.begin() + static_cast<std::ptrdiff_t>(offset),
                        payload,
                        prefix.begin() + static_cast<std::ptrdiff_t>(payloadOffset));
            offset += payload;
            if (!writeBufferedPacket(prefix)) return false;
        }
'''
assert old in text
cpp.write_text(text.replace(old, new, 1))
