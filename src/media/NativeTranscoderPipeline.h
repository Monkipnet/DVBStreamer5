#pragma once

#include "media/Mp2Encoder.h"
#include "media/NativeCodecRuntime.h"
#include "media/NativeMpegTsMux.h"
#include "media/NativeTsDemux.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dvbstreamer5::media::transcode {

struct NativeTranscoderConfig {
    std::string videoCodec = "h264"; // h264 | hevc | copy
    std::string audioCodec = "aac";  // aac | mp2 | copy
    int width = 0;
    int height = 0;
    double fps = 25.0;
    std::uint64_t videoBitrate = 6000000;
    std::uint64_t audioBitrate = 192000;
    bool deinterlace = true;
    std::uint16_t serviceId = 1;
    std::uint16_t videoPid = 0x0100;
    std::uint16_t audioPid = 0x0101;
    std::uint64_t muxBitrate = 0;
    std::string serviceName = "DVBStreamer5";
    std::string serviceProvider = "DVBStreamer5";
};

class NativeTranscoderPipeline {
public:
    NativeTranscoderPipeline();
    ~NativeTranscoderPipeline();

    NativeTranscoderPipeline(const NativeTranscoderPipeline&) = delete;
    NativeTranscoderPipeline& operator=(const NativeTranscoderPipeline&) = delete;

    bool initialize(const NativeTranscoderConfig& config, std::string& error);
    bool process(const std::uint8_t* data, std::size_t size,
                 std::vector<std::uint8_t>& output, std::string& error);
    bool flush(std::vector<std::uint8_t>& output, std::string& error);
    void reset();

    std::string status() const;

private:
    void onProgram(const std::vector<mpegts::DemuxStreamInfo>& streams);
    void onSample(mpegts::DemuxSample&& sample);
    bool ensureVideoDecoder(mpegts::ElementaryCodec codec, std::string& error);
    bool ensureVideoEncoder(std::string& error);
    bool ensureAudioDecoder(mpegts::ElementaryCodec codec, std::string& error);
    bool ensureAudioEncoder(const codec::PcmAudioFrame& input, std::string& error);
    bool handleVideo(mpegts::DemuxSample&& sample, std::string& error);
    bool handleAudio(mpegts::DemuxSample&& sample, std::string& error);
    bool emitVideo(const codec::EncodedVideoFrame& frame, std::string& error);
    bool emitAudio(const codec::EncodedAudioFrame& frame, std::uint64_t duration90k,
                   std::string& error);
    bool emitCopy(mpegts::DemuxSample&& sample, std::string& error);
    void appendPackets(const std::vector<mpegts::Packet>& packets);

    mutable std::mutex mutex_;
    NativeTranscoderConfig config_;
    mpegts::NativeTsDemux demux_;
    mpegts::NativeMpegTsMux mux_;
    std::unique_ptr<codec::VideoDecoder> videoDecoder_;
    std::unique_ptr<codec::VideoEncoder> videoEncoder_;
    std::unique_ptr<codec::AudioDecoder> audioDecoder_;
    std::unique_ptr<codec::AudioEncoder> aacEncoder_;
    std::unique_ptr<Mp2Encoder> mp2Encoder_;
    mpegts::ElementaryCodec inputVideoCodec_ = mpegts::ElementaryCodec::Unknown;
    mpegts::ElementaryCodec inputAudioCodec_ = mpegts::ElementaryCodec::Unknown;
    bool videoEncoderConfigured_ = false;
    bool audioEncoderConfigured_ = false;
    bool initialized_ = false;
    bool failed_ = false;
    std::string failure_;
    std::vector<std::uint8_t> pendingOutput_;
};

} // namespace dvbstreamer5::media::transcode
