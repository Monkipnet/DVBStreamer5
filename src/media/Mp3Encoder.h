#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dvbstreamer5::media {

// Native MPEG-1 Layer III adapter. LAME is used directly (no FFmpeg/GStreamer).
// Compiled only when libmp3lame headers and library are available.
class Mp3Encoder {
public:
    Mp3Encoder();
    ~Mp3Encoder();

    Mp3Encoder(const Mp3Encoder&) = delete;
    Mp3Encoder& operator=(const Mp3Encoder&) = delete;

    static bool available() noexcept;
    bool initialize(std::uint32_t sampleRate, std::uint32_t channels,
                    std::uint32_t bitrate, std::string& error);
    // The input consists of signed, interleaved 16-bit PCM.
    bool encodeInterleaved(const std::int16_t* pcm, std::size_t samplesPerChannel,
                           std::vector<std::uint8_t>& output, std::string& error);
    bool finish(std::vector<std::uint8_t>& output, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvbstreamer5::media
