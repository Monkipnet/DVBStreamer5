#pragma once

#include "media/NativeCodecRuntime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dvbstreamer5::media::codec {

struct NvidiaZeroCopyConfig {
    mpegts::ElementaryCodec inputCodec = mpegts::ElementaryCodec::Unknown;
    mpegts::ElementaryCodec outputCodec = mpegts::ElementaryCodec::Unknown;
    int width = 0;
    int height = 0;
    double fps = 25.0;
    std::uint64_t bitrate = 6000000;
    bool deinterlace = true;
    // Preserve an explicitly requested output raster even when it is larger
    // than the decoded raster. Browser preview uses 1280x720 square pixels
    // for anamorphic SD television sources.
    bool lockOutputGeometry = false;
};

class NvidiaZeroCopyTranscoder {
public:
    virtual ~NvidiaZeroCopyTranscoder() = default;

    virtual bool process(const std::uint8_t* data, std::size_t size,
                         std::uint64_t pts90k, bool hasPts,
                         std::vector<EncodedVideoFrame>& output,
                         std::string& error) = 0;
    virtual bool flush(std::vector<EncodedVideoFrame>& output,
                       std::string& error) = 0;

    virtual int sourceWidth() const noexcept = 0;
    virtual int sourceHeight() const noexcept = 0;
    virtual int outputWidth() const noexcept = 0;
    virtual int outputHeight() const noexcept = 0;
};

bool nvidiaZeroCopyRuntimeAvailable() noexcept;

std::unique_ptr<NvidiaZeroCopyTranscoder> createNvidiaZeroCopyTranscoder(
    const NvidiaZeroCopyConfig& config,
    std::string& error);

} // namespace dvbstreamer5::media::codec
