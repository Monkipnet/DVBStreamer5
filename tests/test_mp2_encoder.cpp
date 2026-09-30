#include "media/Mp2Encoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBitrate = 192000;
constexpr std::size_t kChannels = 2;
constexpr std::size_t kSamplesPerFrame = 1152;

bool fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    return false;
}

std::vector<std::int16_t> testPcm(std::size_t samplesPerChannel) {
    std::vector<std::int16_t> pcm(samplesPerChannel * kChannels);
    for (std::size_t sample = 0; sample < samplesPerChannel; ++sample) {
        const auto left = static_cast<std::int32_t>((sample * 127 + 19) % 30000) - 15000;
        const auto right = static_cast<std::int32_t>((sample * 313 + 71) % 30000) - 15000;
        pcm[sample * 2] = static_cast<std::int16_t>(left);
        pcm[sample * 2 + 1] = static_cast<std::int16_t>(right);
    }
    return pcm;
}

bool encode(const std::vector<std::int16_t>& pcm,
            bool chunked,
            std::vector<std::uint8_t>& output,
            std::string& error) {
    dvbstreamer5::media::Mp2Encoder encoder;
    if (!encoder.initialize({kSampleRate, kChannels, kBitrate}, error)) return false;

    const std::size_t totalSamples = pcm.size() / kChannels;
    std::size_t offset = 0;
    std::size_t chunkIndex = 0;
    constexpr std::array<std::size_t, 5> chunkSizes = {17, 1151, 9, 2047, 383};
    while (offset < totalSamples) {
        const std::size_t requested = chunked
            ? chunkSizes[chunkIndex++ % chunkSizes.size()]
            : totalSamples;
        const std::size_t count = std::min(requested, totalSamples - offset);
        if (!encoder.encodeInterleaved(
                pcm.data() + offset * kChannels, count, output, error)) {
            return false;
        }
        offset += count;
    }
    return encoder.finish(output, error);
}

bool checkMpeg1Layer2Frames(const std::vector<std::uint8_t>& bytes,
                            std::size_t expectedFrameCount) {
    const std::uint32_t bitrateKbps = kBitrate / 1000;
    const std::uint32_t expectedBitrateIndex = 10;
    const std::uint32_t expectedSampleRateIndex = 1;
    std::size_t offset = 0;
    std::size_t frameCount = 0;

    while (offset + 4 <= bytes.size()) {
        const std::uint32_t header =
            (static_cast<std::uint32_t>(bytes[offset]) << 24) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
            static_cast<std::uint32_t>(bytes[offset + 3]);
        const std::uint32_t sync = (header >> 21) & 0x7ff;
        const std::uint32_t version = (header >> 19) & 0x3;
        const std::uint32_t layer = (header >> 17) & 0x3;
        const std::uint32_t bitrateIndex = (header >> 12) & 0xf;
        const std::uint32_t sampleRateIndex = (header >> 10) & 0x3;
        const std::uint32_t padding = (header >> 9) & 0x1;

        if (sync != 0x7ff || version != 0x3 || layer != 0x2 ||
            bitrateIndex != expectedBitrateIndex ||
            sampleRateIndex != expectedSampleRateIndex) {
            return fail("output is not a 48 kHz, 192 kbit/s MPEG-1 Layer II stream");
        }

        const std::size_t frameSize =
            (144000u * bitrateKbps) / kSampleRate + padding;
        if (frameSize < 4 || offset + frameSize > bytes.size()) {
            return fail("MP2 stream ends in an incomplete frame");
        }
        offset += frameSize;
        ++frameCount;
    }

    if (offset != bytes.size()) return fail("MP2 stream has trailing non-frame bytes");
    if (frameCount != expectedFrameCount) return fail("unexpected number of encoded MP2 frames");
    return true;
}

} // namespace

int main() {
    constexpr std::size_t inputSamples = 30 * kSamplesPerFrame + 37;
    const auto pcm = testPcm(inputSamples);
    std::vector<std::uint8_t> oneCall;
    std::vector<std::uint8_t> irregularChunks;
    std::string error;

    if (!encode(pcm, false, oneCall, error)) {
        fail(error);
        return 1;
    }
    if (!encode(pcm, true, irregularChunks, error)) {
        fail(error);
        return 1;
    }
    if (oneCall.empty()) {
        fail("encoder produced no MP2 bytes");
        return 1;
    }
    if (oneCall != irregularChunks) {
        fail("output depends on PCM chunk boundaries");
        return 1;
    }
    if (!checkMpeg1Layer2Frames(oneCall, 31)) return 1;

    dvbstreamer5::media::Mp2Encoder unsupported;
    if (unsupported.initialize({48000, 6, 192000}, error)) {
        fail("unsupported channel layout was accepted");
        return 1;
    }

    std::cout << "PASS: deterministic PCM-to-MP2 encoding (TwoLAME MPEG-1 Layer II)"
              << std::endl;
    return 0;
}
