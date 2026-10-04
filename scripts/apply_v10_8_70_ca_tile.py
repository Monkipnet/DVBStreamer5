from pathlib import Path

h = Path('src/StreamManager.h')
s = h.read_text()
old = '''    std::atomic<uint64_t> outputCcErrors{0};
    std::atomic<uint64_t> outputCcErrorsDelta{0};
    std::atomic<uint64_t> outputTsPayloadPacketsDelta{0};
    std::atomic<uint64_t> outputTsScrambledPacketsDelta{0};
    std::atomic<uint64_t> outputTsClearPesStartsDelta{0};
'''
new = '''    std::atomic<uint64_t> outputCcErrors{0};
    std::atomic<uint64_t> outputCcErrorsDelta{0};
    // V10.8.70: cumulative counters sampled from the finished production TS.
    // /api/state consumes the per-second Delta fields below for the dashboard
    // CA indicator, so collect them after remap/CA/transcode, not from input TS.
    std::atomic<uint64_t> outputTsPayloadPackets{0};
    std::atomic<uint64_t> outputTsScrambledPackets{0};
    std::atomic<uint64_t> outputTsClearPesStarts{0};
    std::atomic<uint64_t> outputTsPayloadPacketsDelta{0};
    std::atomic<uint64_t> outputTsScrambledPacketsDelta{0};
    std::atomic<uint64_t> outputTsClearPesStartsDelta{0};
'''
if old not in s:
    raise SystemExit('StreamManager.h output telemetry anchor not found')
h.write_text(s.replace(old, new, 1))

p = Path('src/StreamManager.cpp')
s = p.read_text()
old = '''    const std::string streamId = streamConfig.id;
    relay.observeTransport = [httpHub, previewHub, previewPassthrough, hlsSegmenter, cmafSegmenter, mpts, srtOutputs, rtspOutputs, rtmpOutputs, streamId](const uint8_t* data, std::size_t size) {
        if (hlsSegmenter) hlsSegmenter->push(data, size);
'''
new = '''    const std::string streamId = streamConfig.id;
    StreamState* outputStatsState = state.get();
    relay.observeTransport = [httpHub, previewHub, previewPassthrough, hlsSegmenter, cmafSegmenter, mpts, srtOutputs, rtspOutputs, rtmpOutputs, streamId, outputStatsState](const uint8_t* data, std::size_t size) {
        // V10.8.70: dashboard decode state must describe the transport that
        // clients actually receive. Count payload/scrambling/PES only here,
        // after remap + CA descrambling + optional production transcoding.
        if (outputStatsState && data && size >= 188U) {
            std::uint64_t payloadPackets = 0;
            std::uint64_t scrambledPackets = 0;
            std::uint64_t clearPesStarts = 0;

            for (std::size_t offset = 0; offset + 188U <= size; offset += 188U) {
                const std::uint8_t* packet = data + offset;
                if (packet[0] != 0x47U) continue;

                const std::uint16_t pid = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8) | packet[2]);
                // Common PSI/SI and null packets do not describe A/V decode health.
                if (pid == 0x0000U || pid == 0x0001U ||
                    pid == 0x0011U || pid == 0x1fffU) {
                    continue;
                }

                const std::uint8_t adaptationControl =
                    static_cast<std::uint8_t>((packet[3] >> 4) & 0x03U);
                if ((adaptationControl & 0x01U) == 0) continue;

                std::size_t payloadOffset = 4U;
                if ((adaptationControl & 0x02U) != 0) {
                    payloadOffset = 5U + packet[4];
                    if (payloadOffset >= 188U) continue;
                }

                ++payloadPackets;

                const std::uint8_t scramblingControl =
                    static_cast<std::uint8_t>((packet[3] >> 6) & 0x03U);
                if (scramblingControl == 2U || scramblingControl == 3U) {
                    ++scrambledPackets;
                }

                if ((packet[1] & 0x40U) != 0 &&
                    scramblingControl == 0U &&
                    payloadOffset + 3U <= 188U &&
                    packet[payloadOffset] == 0x00U &&
                    packet[payloadOffset + 1U] == 0x00U &&
                    packet[payloadOffset + 2U] == 0x01U) {
                    ++clearPesStarts;
                }
            }

            outputStatsState->outputTsPayloadPackets.fetch_add(
                payloadPackets, std::memory_order_relaxed);
            outputStatsState->outputTsScrambledPackets.fetch_add(
                scrambledPackets, std::memory_order_relaxed);
            outputStatsState->outputTsClearPesStarts.fetch_add(
                clearPesStarts, std::memory_order_relaxed);
        }
        if (hlsSegmenter) hlsSegmenter->push(data, size);
'''
if old not in s:
    raise SystemExit('StreamManager.cpp observeTransport anchor not found')
s = s.replace(old, new, 1)

old = '''void StreamManager::monitorNativeStream(StreamState* state) {
    uint64_t lastIn = 0, lastOut = 0, lastPayloadOut = 0, lastCc = 0;
'''
new = '''void StreamManager::monitorNativeStream(StreamState* state) {
    uint64_t lastIn = 0, lastOut = 0, lastPayloadOut = 0, lastCc = 0;
    uint64_t lastTsPayloadPackets = 0;
    uint64_t lastTsScrambledPackets = 0;
    uint64_t lastTsClearPesStarts = 0;
'''
if old not in s:
    raise SystemExit('monitorNativeStream header anchor not found')
s = s.replace(old, new, 1)

old = '''        state->inputCcErrors.store(cc);
        state->inputCcErrorsDelta.store(cc - lastCc);
        lastIn = in;
        lastOut = out;
        lastPayloadOut = payloadOut;
        lastCc = cc;
'''
new = '''        state->inputCcErrors.store(cc);
        state->inputCcErrorsDelta.store(cc - lastCc);

        const uint64_t tsPayloadPackets =
            state->outputTsPayloadPackets.load(std::memory_order_relaxed);
        const uint64_t tsScrambledPackets =
            state->outputTsScrambledPackets.load(std::memory_order_relaxed);
        const uint64_t tsClearPesStarts =
            state->outputTsClearPesStarts.load(std::memory_order_relaxed);
        state->outputTsPayloadPacketsDelta.store(
            tsPayloadPackets - lastTsPayloadPackets, std::memory_order_relaxed);
        state->outputTsScrambledPacketsDelta.store(
            tsScrambledPackets - lastTsScrambledPackets, std::memory_order_relaxed);
        state->outputTsClearPesStartsDelta.store(
            tsClearPesStarts - lastTsClearPesStarts, std::memory_order_relaxed);
        lastTsPayloadPackets = tsPayloadPackets;
        lastTsScrambledPackets = tsScrambledPackets;
        lastTsClearPesStarts = tsClearPesStarts;

        lastIn = in;
        lastOut = out;
        lastPayloadOut = payloadOut;
        lastCc = cc;
'''
if old not in s:
    raise SystemExit('monitorNativeStream delta anchor not found')
s = s.replace(old, new, 1)
p.write_text(s)

v = Path('src/AppVersion.h')
s = v.read_text()
if '"10.8.69"' not in s:
    raise SystemExit('version anchor not found')
v.write_text(s.replace('"10.8.69"', '"10.8.70"', 1))
