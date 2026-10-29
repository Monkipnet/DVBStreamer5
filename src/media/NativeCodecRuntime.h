#pragma once

#include "media/NativeMpegTsMux.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dvbstreamer5::media::codec {

struct RawVideoFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> i420;
    std::uint64_t pts90k = 0;
    std::uint64_t dts90k = 0;
    bool hasPts = false;
    bool hasDts = false;
    bool keyFrame = false;
    bool interlaced = false;
};

struct PcmAudioFrame {
    int sampleRate = 0;
    int channels = 0;
    std::vector<std::int16_t> samples; // interleaved
    std::uint64_t pts90k = 0;
    bool hasPts = false;
};

struct EncodedVideoFrame {
    std::vector<std::uint8_t> data; // Annex-B
    std::uint64_t pts90k = 0;
    std::uint64_t dts90k = 0;
    bool hasPts = false;
    bool hasDts = false;
    bool keyFrame = false;
};

struct EncodedAudioFrame {
    std::vector<std::uint8_t> data; // ADTS AAC or MPEG audio frame
    std::uint64_t pts90k = 0;
    bool hasPts = false;
};

struct RuntimeCapabilities {
    bool h264Decoder = false;
    bool h264Encoder = false;
    bool hevcDecoder = false;
    bool hevcEncoder = false;
    bool aacDecoder = false;
    bool aacEncoder = false;
    bool mpegAudioDecoder = false;
    std::string h264Library;
    std::string hevcDecoderLibrary;
    std::string hevcEncoderLibrary;
    std::string aacDecoderLibrary;
    std::string aacEncoderLibrary;
    std::string mpegAudioLibrary;
    bool vaapiRuntime = false;
    bool qsvEncoder = false;
    bool qsvH264Encoder = false;
    bool qsvHevcEncoder = false;
    bool nvencEncoder = false;
    bool nvencH264Encoder = false;
    bool nvencHevcEncoder = false;
    std::string intelHardwareLibrary;
    std::string nvencHardwareLibrary;
};

RuntimeCapabilities inspectRuntimeCapabilities();

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    virtual bool decode(const std::uint8_t* data, std::size_t size,
                        std::uint64_t pts90k, bool hasPts,
                        std::vector<RawVideoFrame>& output,
                        std::string& error) = 0;
    virtual void reset() = 0;
};

struct VideoEncoderOptions {
    std::string preset = "balanced";
    std::string profile = "auto";
    std::string level = "auto";
    std::uint32_t gopFrames = 50;
    std::uint32_t bFrames = 0;
    std::uint32_t refFrames = 0;
    bool openGop = false;
    std::uint32_t aqStrength = 8;
};

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    virtual void setOptions(const VideoEncoderOptions&) {}
    virtual bool configure(int width, int height, double fps,
                           std::uint64_t bitrate, std::string& error) = 0;
    virtual bool encode(const RawVideoFrame& input,
                        std::vector<EncodedVideoFrame>& output,
                        std::string& error) = 0;
    virtual bool flush(std::vector<EncodedVideoFrame>& output, std::string& error) = 0;
    // Hardware encoders may accept a source geometry that differs from the
    // configured output geometry and perform scaling on-device.
    virtual bool supportsNativeScaling() const noexcept { return false; }
};

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    virtual bool decode(const std::uint8_t* data, std::size_t size,
                        std::uint64_t pts90k, bool hasPts,
                        std::vector<PcmAudioFrame>& output,
                        std::string& error) = 0;
    virtual void reset() = 0;
};

class AudioEncoder {
public:
    virtual ~AudioEncoder() = default;
    virtual bool configure(int sampleRate, int channels, std::uint64_t bitrate,
                           std::string& error) = 0;
    virtual bool encode(const PcmAudioFrame& input,
                        std::vector<EncodedAudioFrame>& output,
                        std::string& error) = 0;
    virtual bool flush(std::vector<EncodedAudioFrame>& output, std::string& error) = 0;
};

std::unique_ptr<VideoDecoder> createVideoDecoder(mpegts::ElementaryCodec codec, std::string& error);
// Diagnostic-only OpenH264 selection. Returns an error if unavailable rather
// than silently selecting Ittiam and invalidating the comparison.
std::unique_ptr<VideoDecoder> createOpenH264VideoDecoder(std::string& error);
std::unique_ptr<VideoEncoder> createVideoEncoder(mpegts::ElementaryCodec codec, std::string& error);
std::unique_ptr<VideoEncoder> createVideoEncoder(mpegts::ElementaryCodec codec, const std::string& backend, std::string& error);
std::unique_ptr<AudioDecoder> createAudioDecoder(mpegts::ElementaryCodec codec, std::string& error);
std::unique_ptr<AudioEncoder> createAacEncoder(std::string& error);

} // namespace dvbstreamer5::media::codec
