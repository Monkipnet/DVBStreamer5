#pragma once

#include "media/Mp2Encoder.h"
#include "media/NativeCodecRuntime.h"
#include "media/NativeIntelZeroCopy.h"
#include "media/NativeNvidiaZeroCopy.h"
#include "media/NativeMpegTsMux.h"
#include "media/NativeTsDemux.h"

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
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
    // Preserve the explicitly configured output raster. Production transcode,
    // browser preview and ABR all provide an explicit target geometry; the
    // pipeline must not silently replace 1920x1080/1280x720 with a smaller
    // decoded source raster such as PAL 704x576.
    bool lockOutputGeometry = true;
    std::uint16_t serviceId = 1;
    std::uint16_t videoPid = 0x0100;
    std::uint16_t audioPid = 0x0101;
    std::uint64_t muxBitrate = 0;
    std::string serviceName = "DVBStreamer5";
    std::string serviceProvider = "DVBStreamer5";
};

// Live AAC timing guard used by the production transcoder.  The vendored FDK
// decoder/encoder deliberately keep their own continuous sample clocks, which
// is useful for raw elementary streams but becomes wrong after a live HLS/PES
// timing discontinuity or when container PTS cadence differs slightly from the
// decoded sample cadence.  The mux scheduler then sees audio hundreds of
// milliseconds behind video and repeatedly enters its 150/500 ms hold window.
//
// Keep the codec objects untouched and correct timing at the pipeline boundary:
//  * decoder output is re-anchored only when a PES PTS differs by >= 50 ms;
//  * AAC encoder output PTS follows the exact 1024-sample PCM chunks submitted
//    to FDK, including encoder delay, instead of a free-running first-PTS clock.
class PtsCorrectingAudioDecoderHandle {
public:
    PtsCorrectingAudioDecoderHandle() = default;

    PtsCorrectingAudioDecoderHandle& operator=(
        std::unique_ptr<codec::AudioDecoder> decoder) {
        decoder_ = std::move(decoder);
        resyncCount_ = 0;
        return *this;
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(decoder_);
    }

    void reset() {
        decoder_.reset();
        resyncCount_ = 0;
    }

    PtsCorrectingAudioDecoderHandle* operator->() noexcept { return this; }
    const PtsCorrectingAudioDecoderHandle* operator->() const noexcept { return this; }

    bool decode(const std::uint8_t* data, std::size_t size,
                std::uint64_t pts90k, bool hasPts,
                std::vector<codec::PcmAudioFrame>& output,
                std::string& error) {
        if (!decoder_) {
            error = "native audio decoder is not configured";
            return false;
        }

        const std::size_t firstOutput = output.size();
        if (!decoder_->decode(data, size, pts90k, hasPts, output, error))
            return false;
        if (!hasPts || output.size() == firstOutput)
            return true;

        auto& first = output[firstOutput];
        const std::uint64_t anchor = pts90k & kPtsMask;
        if (!first.hasPts) {
            std::uint64_t current = anchor;
            for (std::size_t i = firstOutput; i < output.size(); ++i) {
                auto& frame = output[i];
                frame.pts90k = current;
                frame.hasPts = true;
                if (frame.sampleRate > 0 && frame.channels > 0) {
                    const std::size_t frames = frame.samples.size() /
                        static_cast<std::size_t>(frame.channels);
                    current = (current +
                        static_cast<std::uint64_t>(frames) * 90000ULL /
                        static_cast<unsigned>(frame.sampleRate)) & kPtsMask;
                }
            }
            return true;
        }

        const std::int64_t delta = signedPtsDelta(anchor, first.pts90k);
        const std::int64_t magnitude = delta < 0 ? -delta : delta;
        if (magnitude < static_cast<std::int64_t>(kResyncThreshold90k))
            return true;

        for (std::size_t i = firstOutput; i < output.size(); ++i) {
            auto& frame = output[i];
            if (frame.hasPts)
                frame.pts90k = shiftPts(frame.pts90k, delta);
        }

        ++resyncCount_;
        if (resyncCount_ <= 4 || (resyncCount_ % 100U) == 0U) {
            std::cerr << "NATIVE AUDIO PTS RESYNC stage=decode count="
                      << resyncCount_
                      << " delta_ms=" << (delta / 90)
                      << " anchor=" << anchor
                      << std::endl;
        }
        return true;
    }

private:
    static constexpr std::uint64_t kPtsMask = (1ULL << 33U) - 1ULL;
    static constexpr std::uint64_t kPtsHalf = 1ULL << 32U;
    static constexpr std::uint64_t kResyncThreshold90k = 4500ULL; // 50 ms

    static std::int64_t signedPtsDelta(std::uint64_t target,
                                       std::uint64_t current) noexcept {
        target &= kPtsMask;
        current &= kPtsMask;
        const std::uint64_t forward = (target - current) & kPtsMask;
        if (forward < kPtsHalf)
            return static_cast<std::int64_t>(forward);
        return -static_cast<std::int64_t>((current - target) & kPtsMask);
    }

    static std::uint64_t shiftPts(std::uint64_t value,
                                  std::int64_t delta) noexcept {
        value &= kPtsMask;
        if (delta >= 0)
            return (value + static_cast<std::uint64_t>(delta)) & kPtsMask;
        return (value - static_cast<std::uint64_t>(-delta)) & kPtsMask;
    }

    std::unique_ptr<codec::AudioDecoder> decoder_;
    std::uint64_t resyncCount_ = 0;
};

class PtsCorrectingAudioEncoderHandle {
public:
    PtsCorrectingAudioEncoderHandle() = default;

    PtsCorrectingAudioEncoderHandle& operator=(
        std::unique_ptr<codec::AudioEncoder> encoder) {
        encoder_ = std::move(encoder);
        resetTiming();
        return *this;
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(encoder_);
    }

    void reset() {
        encoder_.reset();
        resetTiming();
    }

    PtsCorrectingAudioEncoderHandle* operator->() noexcept { return this; }
    const PtsCorrectingAudioEncoderHandle* operator->() const noexcept { return this; }

    bool configure(int sampleRate, int channels, std::uint64_t bitrate,
                   std::string& error) {
        resetTiming();
        if (!encoder_) {
            error = "native AAC encoder is not configured";
            return false;
        }
        if (!encoder_->configure(sampleRate, channels, bitrate, error))
            return false;
        sampleRate_ = sampleRate;
        channels_ = channels;
        return true;
    }

    bool encode(const codec::PcmAudioFrame& input,
                std::vector<codec::EncodedAudioFrame>& output,
                std::string& error) {
        if (!encoder_) {
            error = "native AAC encoder is not configured";
            return false;
        }

        queueInputTiming(input);
        const std::size_t firstOutput = output.size();
        if (!encoder_->encode(input, output, error)) {
            resetTiming();
            return false;
        }
        applyOutputTiming(output, firstOutput);
        return true;
    }

    bool flush(std::vector<codec::EncodedAudioFrame>& output,
               std::string& error) {
        if (!encoder_) return true;

        // FDK's wrapper pads one final partial 1024-sample frame on flush.
        // Queue its start timestamp before asking the underlying encoder to
        // emit it so delayed encoder output still receives the correct tag.
        if (queuedPcmFrames_ != 0 && !pcmSpans_.empty()) {
            submittedPts_.push_back({pcmSpans_.front().pts90k,
                                     pcmSpans_.front().hasPts});
            pcmSpans_.clear();
            queuedPcmFrames_ = 0;
        }

        const std::size_t firstOutput = output.size();
        if (!encoder_->flush(output, error))
            return false;
        applyOutputTiming(output, firstOutput);
        return true;
    }

private:
    static constexpr std::size_t kAacFrameSamples = 1024;
    static constexpr std::uint64_t kPtsMask = (1ULL << 33U) - 1ULL;

    struct PcmSpan {
        std::size_t frames = 0;
        std::uint64_t pts90k = 0;
        bool hasPts = false;
    };

    struct PtsTag {
        std::uint64_t pts90k = 0;
        bool hasPts = false;
    };

    void resetTiming() {
        sampleRate_ = 0;
        channels_ = 0;
        queuedPcmFrames_ = 0;
        pcmSpans_.clear();
        submittedPts_.clear();
    }

    void queueInputTiming(const codec::PcmAudioFrame& input) {
        if (sampleRate_ <= 0 || channels_ <= 0 ||
            input.channels != channels_ || input.sampleRate != sampleRate_ ||
            input.samples.empty()) {
            return;
        }

        const std::size_t frames = input.samples.size() /
            static_cast<std::size_t>(channels_);
        if (frames == 0) return;

        pcmSpans_.push_back({frames, input.pts90k & kPtsMask, input.hasPts});
        queuedPcmFrames_ += frames;

        while (queuedPcmFrames_ >= kAacFrameSamples && !pcmSpans_.empty()) {
            submittedPts_.push_back({pcmSpans_.front().pts90k,
                                     pcmSpans_.front().hasPts});
            consumePcmFrames(kAacFrameSamples);
            queuedPcmFrames_ -= kAacFrameSamples;
        }
    }

    void consumePcmFrames(std::size_t count) {
        std::size_t remaining = count;
        while (remaining != 0 && !pcmSpans_.empty()) {
            auto& span = pcmSpans_.front();
            const std::size_t take = std::min(remaining, span.frames);
            if (span.hasPts && sampleRate_ > 0) {
                span.pts90k = (span.pts90k +
                    static_cast<std::uint64_t>(take) * 90000ULL /
                    static_cast<unsigned>(sampleRate_)) & kPtsMask;
            }
            span.frames -= take;
            remaining -= take;
            if (span.frames == 0)
                pcmSpans_.pop_front();
        }
    }

    void applyOutputTiming(std::vector<codec::EncodedAudioFrame>& output,
                           std::size_t firstOutput) {
        for (std::size_t i = firstOutput;
             i < output.size() && !submittedPts_.empty(); ++i) {
            const PtsTag tag = submittedPts_.front();
            submittedPts_.pop_front();
            if (!tag.hasPts) continue;
            output[i].pts90k = tag.pts90k;
            output[i].hasPts = true;
        }
    }

    std::unique_ptr<codec::AudioEncoder> encoder_;
    int sampleRate_ = 0;
    int channels_ = 0;
    std::size_t queuedPcmFrames_ = 0;
    std::deque<PcmSpan> pcmSpans_;
    std::deque<PtsTag> submittedPts_;
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
    std::unique_ptr<codec::IntelZeroCopyTranscoder> intelZeroCopy_;
    bool intelZeroCopyAttempted_ = false;
    bool intelZeroCopyFallbackLogged_ = false;
    std::atomic<bool> intelZeroCopyActive_{false};
    std::unique_ptr<codec::NvidiaZeroCopyTranscoder> nvidiaZeroCopy_;
    bool nvidiaZeroCopyAttempted_ = false;
    bool nvidiaZeroCopyFallbackLogged_ = false;
    std::atomic<bool> nvidiaZeroCopyActive_{false};
    std::unique_ptr<codec::VideoEncoder> videoEncoder_;
    PtsCorrectingAudioDecoderHandle audioDecoder_;
    PtsCorrectingAudioEncoderHandle aacEncoder_;
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
