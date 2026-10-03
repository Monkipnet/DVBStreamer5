#pragma once

#include "media/Mp2Encoder.h"
#include "media/NativeCodecRuntime.h"
#include "media/NativeNvidiaZeroCopy.h"
#include "media/NativeMpegTsMux.h"
#include "media/NativeTsDemux.h"

#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dvbstreamer5::media::transcode {

struct NativeTranscoderConfig {
    std::string videoCodec = "h264"; // h264 | hevc | copy
    std::string videoEncoder = "auto"; // auto | cpu | nvenc | qsv | vaapi
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

    // ABR shared-media mode: the primary pipeline decodes/deinterlaces video
    // and encodes audio once. Raw video frames and encoded audio are then
    // fanned out to rendition muxers/encoders without duplicate source demux,
    // video decode or audio transcode work.
    using DecodedVideoObserver =
        std::function<void(std::shared_ptr<const codec::RawVideoFrame>)>;
    using EncodedAudioObserver =
        std::function<void(const codec::EncodedAudioFrame&, std::uint64_t)>;
    void setExternalVideoInput(bool enabled);
    void setDecodedVideoObserver(DecodedVideoObserver observer);
    bool pushDecodedVideoFrame(std::shared_ptr<const codec::RawVideoFrame> frame);
    bool setOutputGeometryIfUnconfigured(int width, int height);
    std::pair<int,int> configuredOutputGeometry() const;
    void setExternalAudioInput(bool enabled);
    void setEncodedAudioObserver(EncodedAudioObserver observer);
    bool pushEncodedAudioFrame(const codec::EncodedAudioFrame& frame,
                               std::uint64_t duration90k);
    bool pollOutput(std::vector<std::uint8_t>& output, std::string& error);

    std::string status() const;

private:
    void onProgram(const std::vector<mpegts::DemuxStreamInfo>& streams);
    void onSample(mpegts::DemuxSample&& sample);
    void startWorkers();
    void stopWorkers();
    void videoWorkerLoop();
    void videoEncodeWorkerLoop();
    void audioWorkerLoop();
    void muxWorkerLoop();
    void setFailure(std::string error);
    bool waitForIdle();
    void drainPendingOutput(std::vector<std::uint8_t>& output);
    bool ensureVideoDecoder(mpegts::ElementaryCodec codec, std::string& error);
    bool ensureVideoEncoder(std::string& error);
    bool ensureAudioDecoder(mpegts::ElementaryCodec codec, std::string& error);
    bool ensureAudioEncoder(const codec::PcmAudioFrame& input, std::string& error);
    bool handleVideo(mpegts::DemuxSample&& sample, std::string& error);
    bool handleDecodedVideoFrame(const codec::RawVideoFrame& raw, std::string& error);
    bool handleAudio(mpegts::DemuxSample&& sample, std::string& error);
    bool emitVideo(const codec::EncodedVideoFrame& frame, std::string& error);
    bool emitAudio(const codec::EncodedAudioFrame& frame, std::uint64_t duration90k,
                   std::string& error);
    bool emitCopy(mpegts::DemuxSample&& sample, std::string& error);
    void appendPackets(const std::vector<mpegts::Packet>& packets);

    struct MuxQueuedSample {
        mpegts::ElementaryKind kind = mpegts::ElementaryKind::Video;
        std::vector<std::uint8_t> data;
        std::uint64_t pts90k = 0;
        std::uint64_t dts90k = 0;
        std::uint64_t duration90k = 0;
        bool hasPts = false;
        bool hasDts = false;
        bool randomAccess = false;
        std::chrono::steady_clock::time_point queuedAt;
    };
    void enqueueMuxSample(MuxQueuedSample&& sample);
    bool writeMuxSample(MuxQueuedSample&& sample, std::string& error);
    static std::uint64_t muxClockOf(const MuxQueuedSample& sample) noexcept;

    mutable std::mutex mutex_;
    mutable std::mutex failureMutex_;
    mutable std::mutex outputMutex_;
    mutable std::mutex muxMutex_;
    mutable std::mutex videoDecoderMutex_;
    mutable std::mutex videoEncoderMutex_;
    mutable std::mutex audioCodecMutex_;
    mutable std::mutex videoQueueMutex_;
    mutable std::mutex audioQueueMutex_;
    mutable std::mutex muxQueueMutex_;
    mutable std::mutex decodedVideoObserverMutex_;
    mutable std::mutex encodedAudioObserverMutex_;
    std::condition_variable videoQueueCv_;
    std::condition_variable videoEncodeQueueCv_;
    std::condition_variable audioQueueCv_;
    std::condition_variable idleCv_;
    std::condition_variable muxQueueCv_;
    std::thread videoWorker_;
    std::thread videoEncodeWorker_;
    std::thread audioWorker_;
    std::thread muxWorker_;
    std::deque<mpegts::DemuxSample> videoQueue_;
    std::deque<std::shared_ptr<const codec::RawVideoFrame>> externalVideoQueue_;
    std::deque<mpegts::DemuxSample> audioQueue_;
    std::deque<MuxQueuedSample> muxVideoQueue_;
    std::deque<MuxQueuedSample> muxAudioQueue_;
    std::atomic<bool> workersStop_{false};
    std::atomic<bool> muxStop_{false};
    std::atomic<bool> videoWorkerActive_{false};
    std::atomic<bool> videoEncodeWorkerActive_{false};
    std::atomic<bool> audioWorkerActive_{false};
    std::atomic<bool> muxWorkerActive_{false};
    std::atomic<bool> videoResetRequested_{false};
    std::atomic<bool> externalVideoInput_{false};
    std::atomic<bool> externalAudioInput_{false};
    bool videoDropUntilRandomAccess_ = false;
    std::uint64_t droppedVideoSamples_ = 0;
    std::uint64_t droppedExternalVideoFrames_ = 0;
    std::uint64_t decodedVideoQueueDrops_ = 0;
    bool videoRateClockValid_ = false;
    std::uint64_t videoRateNextPts90k_ = 0;
    std::uint64_t videoRateDroppedFrames_ = 0;
    bool sourceGeometryResolved_ = false;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    DecodedVideoObserver decodedVideoObserver_;
    EncodedAudioObserver encodedAudioObserver_;
    std::uint64_t droppedAudioSamples_ = 0;
    std::atomic<std::uint64_t> muxedVideoSamples_{0};
    std::atomic<std::uint64_t> muxedAudioSamples_{0};
    std::atomic<std::uint64_t> muxLateVideo_{0};
    std::atomic<std::uint64_t> muxLateAudio_{0};
    std::atomic<std::uint64_t> muxDroppedVideo_{0};
    std::atomic<std::uint64_t> muxDroppedAudio_{0};
    std::atomic<std::uint64_t> lastMuxVideoClock90k_{0};
    std::atomic<std::uint64_t> lastMuxAudioClock90k_{0};
    static constexpr std::size_t kMaxVideoQueue = 128;
    static constexpr std::size_t kMaxExternalVideoQueue = 12;
    static constexpr std::size_t kMaxAudioQueue = 512;
    static constexpr std::size_t kMaxMuxVideoQueue = 64;
    static constexpr std::size_t kMaxMuxAudioQueue = 512;
    static constexpr std::uint64_t kMaxAvLead90k = 13500; // 150 ms preferred
    static constexpr std::uint64_t kMaxAvHardLead90k = 45000; // 500 ms absolute
    static constexpr auto kMuxSingleStreamWait = std::chrono::milliseconds(120);
    static constexpr auto kMuxSingleStreamMaxHold = std::chrono::milliseconds(350);
    // Joining an arbitrary live GOP can take up to roughly one GOP before the
    // decoder emits a usable IDR. Do not release seconds of audio first.
    static constexpr auto kMuxStartupPeerWait = std::chrono::milliseconds(2500);
    NativeTranscoderConfig config_;
    mpegts::NativeTsDemux demux_;
    mpegts::NativeMpegTsMux mux_;
    std::unique_ptr<codec::VideoDecoder> videoDecoder_;
    std::unique_ptr<codec::NvidiaZeroCopyTranscoder> nvidiaZeroCopy_;
    bool nvidiaZeroCopyAttempted_ = false;
    bool nvidiaZeroCopyFallbackLogged_ = false;
    std::atomic<bool> nvidiaZeroCopyActive_{false};
    std::unique_ptr<codec::VideoEncoder> videoEncoder_;
    std::unique_ptr<codec::AudioDecoder> audioDecoder_;
    std::unique_ptr<codec::AudioEncoder> aacEncoder_;
    std::unique_ptr<Mp2Encoder> mp2Encoder_;
    mpegts::ElementaryCodec inputVideoCodec_ = mpegts::ElementaryCodec::Unknown;
    mpegts::ElementaryCodec inputAudioCodec_ = mpegts::ElementaryCodec::Unknown;
    std::uint16_t inputVideoPid_ = 0xffff;
    std::uint16_t inputAudioPid_ = 0xffff;
    bool videoEncoderConfigured_ = false;
    bool audioEncoderConfigured_ = false;
    bool videoStartupReady_ = false;
    std::uint64_t videoStartupDropped_ = 0;
    std::uint64_t inputVideoSamples_ = 0;
    std::uint64_t inputAudioSamples_ = 0;
    std::uint64_t decodedVideoFrames_ = 0;
    std::uint64_t decodedAudioFrames_ = 0;
    std::uint64_t ignoredVideoSamples_ = 0;
    std::uint64_t ignoredAudioSamples_ = 0;
    bool initialized_ = false;
    std::atomic<bool> failed_{false};
    std::string failure_;
    std::vector<std::uint8_t> pendingOutput_;
};

} // namespace dvbstreamer5::media::transcode
