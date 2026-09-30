#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dvbstreamer5::media {

struct Mp2EncoderConfig {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t bitrate = 192000;
};

class Mp2Encoder {
public:
    Mp2Encoder();
    ~Mp2Encoder();

    Mp2Encoder(const Mp2Encoder&) = delete;
    Mp2Encoder& operator=(const Mp2Encoder&) = delete;
    Mp2Encoder(Mp2Encoder&&) noexcept;
    Mp2Encoder& operator=(Mp2Encoder&&) noexcept;

    bool initialize(const Mp2EncoderConfig& config, std::string& error);

    // Accepts interleaved signed 16-bit PCM; samplesPerChannel is not the
    // number of interleaved values. Encoded bytes are appended to output.
    bool encodeInterleaved(const std::int16_t* pcm,
                           std::size_t samplesPerChannel,
                           std::vector<std::uint8_t>& output,
                           std::string& error);

    // Flushes the final partial MPEG audio frame and ends this encoder.
    bool finish(std::vector<std::uint8_t>& output, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvbstreamer5::media
