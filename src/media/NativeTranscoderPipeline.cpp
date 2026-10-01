#include "media/NativeTranscoderPipeline.h"

#include "media/NativeVideoProcessing.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

namespace dvbstreamer5::media::transcode {
namespace {

mpegts::ElementaryCodec videoCodecFromName(const std::string& codec) {
    if (codec == "h264") return mpegts::ElementaryCodec::H264;
    if (codec == "hevc" || codec == "h265") return mpegts::ElementaryCodec::H265;
    return mpegts::ElementaryCodec::Unknown;
}

mpegts::ElementaryCodec audioCodecFromName(const std::string& codec) {
    if (codec == "aac") return mpegts::ElementaryCodec::AacAdts;
    if (codec == "mp2" || codec == "mp3") return mpegts::ElementaryCodec::MpegAudio;
    return mpegts::ElementaryCodec::Unknown;
}


struct StartupParameterSets {
    bool h264Sps = false;
    bool h264Pps = false;
    bool h264Idr = false;
    bool h265Vps = false;
    bool h265Sps = false;
    bool h265Pps = false;
    bool h265Irap = false;
};

StartupParameterSets inspectAnnexBStartup(const std::vector<std::uint8_t>& data,
                                          mpegts::ElementaryCodec codec) {
    StartupParameterSets result;
    auto startCode = [&](std::size_t i, std::size_t& width) {
        if (i + 3 <= data.size() && data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            width = 3; return true;
        }
        if (i + 4 <= data.size() && data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            width = 4; return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < data.size();) {
        std::size_t sc = 0;
        if (!startCode(i, sc)) { ++i; continue; }
        const std::size_t nal = i + sc;
        if (nal >= data.size()) break;
        if (codec == mpegts::ElementaryCodec::H264) {
            const unsigned type = data[nal] & 0x1fU;
            if (type == 7) result.h264Sps = true;
            else if (type == 8) result.h264Pps = true;
            else if (type == 5) result.h264Idr = true;
        } else if (codec == mpegts::ElementaryCodec::H265) {
            const unsigned type = (data[nal] >> 1) & 0x3fU;
            if (type == 32) result.h265Vps = true;
            else if (type == 33) result.h265Sps = true;
            else if (type == 34) result.h265Pps = true;
            if (type >= 16 && type <= 23) result.h265Irap = true;
        }
        i = nal + 1;
    }
    return result;
}

std::uint64_t videoDuration90k(double fps) {
    if (fps < 1.0) fps = 25.0;
    return static_cast<std::uint64_t>(std::llround(90000.0 / fps));
}

// V10.2 diagnostic-only elementary-stream capture.  This is deliberately
// disabled unless DVBSTREAMER5_DUMP_H264_ES is set.  It writes the exact
// H.264 bytes handed from NativeTsDemux to the decoder, without changing
// runtime packetization or adding any multimedia dependency.
void dumpH264ElementarySample(const mpegts::DemuxSample& sample) {
    if (sample.stream.codec != mpegts::ElementaryCodec::H264 || sample.data.empty()) return;
    const char* env = std::getenv("DVBSTREAMER5_DUMP_H264_ES");
    if (!env || !*env) return;

    struct State {
        std::mutex mutex;
        std::string path;
        std::ofstream es;
        std::ofstream index;
        std::uint64_t samples = 0;
        std::uint64_t bytes = 0;
        bool limitLogged = false;
    };
    static State state;
    constexpr std::uint64_t kMaxBytes = 64ULL * 1024ULL * 1024ULL;

    std::lock_guard<std::mutex> lock(state.mutex);
    const std::string path(env);
    if (state.path != path || !state.es.is_open()) {
        if (state.es.is_open()) state.es.close();
        if (state.index.is_open()) state.index.close();
        state.path = path;
        state.samples = 0;
        state.bytes = 0;
        state.limitLogged = false;
        state.es.open(path, std::ios::binary | std::ios::trunc);
        state.index.open(path + ".index.csv", std::ios::trunc);
        if (state.index) state.index << "sample,size,pts90k,dts90k,has_pts,has_dts,random\n";
        std::cerr << "NATIVE AVC ES DUMP open path=" << path
                  << " limit_mb=64" << std::endl;
    }
    if (!state.es) return;
    if (state.bytes >= kMaxBytes) {
        if (!state.limitLogged) {
            std::cerr << "NATIVE AVC ES DUMP limit reached bytes=" << state.bytes << std::endl;
            state.limitLogged = true;
        }
        return;
    }

    const std::uint64_t remaining = kMaxBytes - state.bytes;
    const std::size_t toWrite = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, sample.data.size()));
    state.es.write(reinterpret_cast<const char*>(sample.data.data()),
                   static_cast<std::streamsize>(toWrite));
    if (!state.es) {
        std::cerr << "NATIVE AVC ES DUMP write failed path=" << path << std::endl;
        state.es.close();
        return;
    }
    ++state.samples;
    state.bytes += toWrite;
    if (state.index) {
        state.index << state.samples << ',' << toWrite << ','
                    << sample.pts90k << ',' << sample.dts90k << ','
                    << (sample.hasPts ? 1 : 0) << ','
                    << (sample.hasDts ? 1 : 0) << ','
                    << (sample.randomAccess ? 1 : 0) << '\n';
        state.index.flush();
    }
    state.es.flush();
    if (state.samples <= 4 || (state.samples % 100U) == 0U) {
        std::cerr << "NATIVE AVC ES DUMP samples=" << state.samples
                  << " bytes=" << state.bytes
                  << " last=" << toWrite << std::endl;
    }
}

} // namespace

NativeTranscoderPipeline::NativeTranscoderPipeline() = default;
NativeTranscoderPipeline::~NativeTranscoderPipeline() {
    std::lock_guard<std::mutex> lock(mutex_);
    reset();
}

bool NativeTranscoderPipeline::initialize(const NativeTranscoderConfig& config, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    reset();
    config_ = config;
    if (config_.videoCodec != "copy" && videoCodecFromName(config_.videoCodec) == mpegts::ElementaryCodec::Unknown) {
        error = "native transcoder: unsupported video output codec";
        return false;
    }
    if (config_.audioCodec != "copy" && config_.audioCodec != "aac" && config_.audioCodec != "mp2") {
        error = "native transcoder: supported audio output codecs are copy, aac and mp2";
        return false;
    }
    if (config_.videoCodec != "copy" && ((config_.width & 1) || (config_.height & 1) || config_.width <= 0 || config_.height <= 0)) {
        error = "native transcoder: video resolution must use positive even dimensions";
        return false;
    }

    mpegts::NativeMuxConfig muxConfig;
    muxConfig.serviceId = config_.serviceId ? config_.serviceId : 1;
    muxConfig.videoPid = config_.videoPid ? config_.videoPid : 0x0100;
    muxConfig.audioPid = config_.audioPid ? config_.audioPid : 0x0101;
    muxConfig.targetBitrate = config_.muxBitrate;
    muxConfig.serviceName = config_.serviceName.empty() ? "DVBStreamer5" : config_.serviceName;
    muxConfig.serviceProvider = config_.serviceProvider.empty() ? "DVBStreamer5" : config_.serviceProvider;
    if (!mux_.initialize(muxConfig, error)) return false;
    if (config_.videoCodec != "copy" && !mux_.setCodec(mpegts::ElementaryKind::Video, videoCodecFromName(config_.videoCodec), error)) return false;
    if (config_.audioCodec != "copy" && !mux_.setCodec(mpegts::ElementaryKind::Audio, audioCodecFromName(config_.audioCodec), error)) return false;

    demux_.setProgramCallback([this](const std::vector<mpegts::DemuxStreamInfo>& streams) { onProgram(streams); });
    demux_.setSampleCallback([this](mpegts::DemuxSample&& sample) { onSample(std::move(sample)); });
    initialized_ = true;
    startWorkers();
    std::cerr << "NATIVE ASYNC TRANSCODER start video_queue=" << kMaxVideoQueue
              << " audio_queue=" << kMaxAudioQueue
              << " mux_video_queue=" << kMaxMuxVideoQueue
              << " mux_audio_queue=" << kMaxMuxAudioQueue
              << " separate_workers=1 av_mux_scheduler=1 max_av_lead_ms=150 strict_peer_lead=1 live_queue_drop=1" << std::endl;
    return true;
}

void NativeTranscoderPipeline::reset() {
    stopWorkers();
    demux_.reset();
    {
        std::lock_guard<std::mutex> lock(muxMutex_);
        mux_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(videoCodecMutex_);
        videoDecoder_.reset();
        videoEncoder_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(audioCodecMutex_);
        audioDecoder_.reset();
        aacEncoder_.reset();
        mp2Encoder_.reset();
    }
    inputVideoCodec_ = mpegts::ElementaryCodec::Unknown;
    inputAudioCodec_ = mpegts::ElementaryCodec::Unknown;
    inputVideoPid_ = 0xffff;
    inputAudioPid_ = 0xffff;
    videoEncoderConfigured_ = false;
    audioEncoderConfigured_ = false;
    videoStartupReady_ = false;
    videoStartupDropped_ = 0;
    inputVideoSamples_ = 0;
    inputAudioSamples_ = 0;
    decodedVideoFrames_ = 0;
    decodedAudioFrames_ = 0;
    ignoredVideoSamples_ = 0;
    ignoredAudioSamples_ = 0;
    initialized_ = false;
    failed_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(failureMutex_);
        failure_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        pendingOutput_.clear();
    }
    droppedVideoSamples_ = 0;
    droppedAudioSamples_ = 0;
    muxedVideoSamples_.store(0, std::memory_order_release);
    muxedAudioSamples_.store(0, std::memory_order_release);
    muxLateVideo_.store(0, std::memory_order_release);
    muxLateAudio_.store(0, std::memory_order_release);
    muxDroppedVideo_.store(0, std::memory_order_release);
    muxDroppedAudio_.store(0, std::memory_order_release);
    lastMuxVideoClock90k_.store(0, std::memory_order_release);
    lastMuxAudioClock90k_.store(0, std::memory_order_release);
    videoDropUntilRandomAccess_ = false;
    videoResetRequested_.store(false, std::memory_order_release);
}

void NativeTranscoderPipeline::onProgram(const std::vector<mpegts::DemuxStreamInfo>& streams) {
    const auto pick = [&](mpegts::ElementaryKind kind, std::uint16_t currentPid)
        -> const mpegts::DemuxStreamInfo* {
        if (currentPid != 0xffff) {
            const auto keep = std::find_if(streams.begin(), streams.end(), [&](const auto& stream) {
                return stream.kind == kind && stream.pid == currentPid;
            });
            if (keep != streams.end()) return &*keep;
        }
        const auto first = std::find_if(streams.begin(), streams.end(), [&](const auto& stream) {
            return stream.kind == kind;
        });
        return first == streams.end() ? nullptr : &*first;
    };

    const auto* video = pick(mpegts::ElementaryKind::Video, inputVideoPid_);
    const auto* audio = pick(mpegts::ElementaryKind::Audio, inputAudioPid_);
    const std::uint16_t newVideoPid = video ? video->pid : 0xffff;
    const std::uint16_t newAudioPid = audio ? audio->pid : 0xffff;
    const auto newVideoCodec = video ? video->codec : mpegts::ElementaryCodec::Unknown;
    const auto newAudioCodec = audio ? audio->codec : mpegts::ElementaryCodec::Unknown;

    const bool videoChanged = newVideoPid != inputVideoPid_ || newVideoCodec != inputVideoCodec_;
    const bool audioChanged = newAudioPid != inputAudioPid_ || newAudioCodec != inputAudioCodec_;
    if (videoChanged) {
        { std::lock_guard<std::mutex> codecLock(videoCodecMutex_); videoDecoder_.reset(); }
        inputVideoPid_ = newVideoPid;
        inputVideoCodec_ = newVideoCodec;
        videoResetRequested_.store(false, std::memory_order_release);
    }
    if (audioChanged) {
        { std::lock_guard<std::mutex> codecLock(audioCodecMutex_); audioDecoder_.reset(); }
        inputAudioPid_ = newAudioPid;
        inputAudioCodec_ = newAudioCodec;
    }

    if (videoChanged || audioChanged) {
        std::cerr << "NATIVE TRANSCODER PROGRAM streams=" << streams.size()
                  << " video_pid=" << (inputVideoPid_ == 0xffff ? -1 : static_cast<int>(inputVideoPid_))
                  << " video_codec=" << static_cast<int>(inputVideoCodec_)
                  << " audio_pid=" << (inputAudioPid_ == 0xffff ? -1 : static_cast<int>(inputAudioPid_))
                  << " audio_codec=" << static_cast<int>(inputAudioCodec_)
                  << std::endl;
        for (const auto& stream : streams) {
            std::cerr << "NATIVE TRANSCODER STREAM pid=" << stream.pid
                      << " kind=" << (stream.kind == mpegts::ElementaryKind::Video ? "video" : "audio")
                      << " codec=" << static_cast<int>(stream.codec)
                      << " type=0x" << std::hex << static_cast<unsigned>(stream.streamType) << std::dec
                      << std::endl;
        }
    }
}

void NativeTranscoderPipeline::onSample(mpegts::DemuxSample&& sample) {
    if (failed_.load(std::memory_order_acquire)) return;
    if (sample.stream.kind == mpegts::ElementaryKind::Video) {
        if (inputVideoPid_ == 0xffff) {
            inputVideoPid_ = sample.stream.pid;
            inputVideoCodec_ = sample.stream.codec;
        } else if (sample.stream.pid != inputVideoPid_) {
            ++ignoredVideoSamples_;
            if (ignoredVideoSamples_ <= 4 || (ignoredVideoSamples_ % 100U) == 0U)
                std::cerr << "NATIVE TRANSCODER IGNORE video_pid=" << sample.stream.pid
                          << " selected=" << inputVideoPid_
                          << " count=" << ignoredVideoSamples_ << std::endl;
            return;
        }

        std::lock_guard<std::mutex> qlock(videoQueueMutex_);
        if (videoDropUntilRandomAccess_) {
            if (!sample.randomAccess) {
                ++droppedVideoSamples_;
                return;
            }
            videoDropUntilRandomAccess_ = false;
            videoResetRequested_.store(true, std::memory_order_release);
        }
        if (videoQueue_.size() >= kMaxVideoQueue) {
            droppedVideoSamples_ += videoQueue_.size();
            videoQueue_.clear();
            videoDropUntilRandomAccess_ = true;
            videoResetRequested_.store(true, std::memory_order_release);
            std::cerr << "NATIVE ASYNC VIDEO RESYNC dropped=" << droppedVideoSamples_
                      << " reason=queue_full wait_random_access=1" << std::endl;
            if (!sample.randomAccess) return;
            videoDropUntilRandomAccess_ = false;
        }
        videoQueue_.push_back(std::move(sample));
        videoQueueCv_.notify_one();
        return;
    }

    if (inputAudioPid_ == 0xffff) {
        inputAudioPid_ = sample.stream.pid;
        inputAudioCodec_ = sample.stream.codec;
    } else if (sample.stream.pid != inputAudioPid_) {
        ++ignoredAudioSamples_;
        if (ignoredAudioSamples_ <= 4 || (ignoredAudioSamples_ % 100U) == 0U)
            std::cerr << "NATIVE TRANSCODER IGNORE audio_pid=" << sample.stream.pid
                      << " selected=" << inputAudioPid_
                      << " count=" << ignoredAudioSamples_ << std::endl;
        return;
    }
    {
        std::lock_guard<std::mutex> qlock(audioQueueMutex_);
        while (audioQueue_.size() >= kMaxAudioQueue) {
            audioQueue_.pop_front();
            ++droppedAudioSamples_;
        }
        audioQueue_.push_back(std::move(sample));
    }
    audioQueueCv_.notify_one();
}

void NativeTranscoderPipeline::startWorkers() {
    workersStop_.store(false, std::memory_order_release);
    muxStop_.store(false, std::memory_order_release);
    muxWorker_ = std::thread([this] { muxWorkerLoop(); });
    videoWorker_ = std::thread([this] { videoWorkerLoop(); });
    audioWorker_ = std::thread([this] { audioWorkerLoop(); });
}

void NativeTranscoderPipeline::stopWorkers() {
    workersStop_.store(true, std::memory_order_release);
    // First stop decoder/encoder workers. They are allowed to finish their
    // currently active sample and may enqueue one last encoded AU.
    { std::lock_guard<std::mutex> lock(videoQueueMutex_); videoQueue_.clear(); }
    { std::lock_guard<std::mutex> lock(audioQueueMutex_); audioQueue_.clear(); }
    videoQueueCv_.notify_all();
    audioQueueCv_.notify_all();
    if (videoWorker_.joinable()) videoWorker_.join();
    if (audioWorker_.joinable()) audioWorker_.join();
    videoWorkerActive_.store(false, std::memory_order_release);
    audioWorkerActive_.store(false, std::memory_order_release);

    // Only after producers are gone may the mux worker be stopped. For a hard
    // restart queued encoded media is stale, so discard it rather than delaying
    // reconfiguration.
    muxStop_.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lock(muxQueueMutex_); muxVideoQueue_.clear(); muxAudioQueue_.clear(); }
    muxQueueCv_.notify_all();
    if (muxWorker_.joinable()) muxWorker_.join();
    muxWorkerActive_.store(false, std::memory_order_release);
}

void NativeTranscoderPipeline::setFailure(std::string error) {
    if (error.empty()) error = "native transcoder async worker failed";
    {
        std::lock_guard<std::mutex> lock(failureMutex_);
        failure_ = std::move(error);
    }
    failed_.store(true, std::memory_order_release);
}

void NativeTranscoderPipeline::videoWorkerLoop() {
    for (;;) {
        mpegts::DemuxSample sample;
        {
            std::unique_lock<std::mutex> lock(videoQueueMutex_);
            videoQueueCv_.wait(lock, [this] {
                return workersStop_.load(std::memory_order_acquire) || !videoQueue_.empty();
            });
            if (workersStop_.load(std::memory_order_acquire) && videoQueue_.empty()) break;
            sample = std::move(videoQueue_.front());
            videoQueue_.pop_front();
            videoWorkerActive_.store(true, std::memory_order_release);
        }
        std::string error;
        {
            std::lock_guard<std::mutex> codecLock(videoCodecMutex_);
            if (videoResetRequested_.exchange(false, std::memory_order_acq_rel)) {
                videoDecoder_.reset();
                videoStartupReady_ = false;
            }
            if (!handleVideo(std::move(sample), error)) setFailure(error);
        }
        videoWorkerActive_.store(false, std::memory_order_release);
        idleCv_.notify_all();
        if (failed_.load(std::memory_order_acquire)) break;
    }
}

void NativeTranscoderPipeline::audioWorkerLoop() {
    for (;;) {
        mpegts::DemuxSample sample;
        {
            std::unique_lock<std::mutex> lock(audioQueueMutex_);
            audioQueueCv_.wait(lock, [this] {
                return workersStop_.load(std::memory_order_acquire) || !audioQueue_.empty();
            });
            if (workersStop_.load(std::memory_order_acquire) && audioQueue_.empty()) break;
            sample = std::move(audioQueue_.front());
            audioQueue_.pop_front();
            audioWorkerActive_.store(true, std::memory_order_release);
        }
        std::string error;
        {
            std::lock_guard<std::mutex> codecLock(audioCodecMutex_);
            if (!handleAudio(std::move(sample), error)) setFailure(error);
        }
        audioWorkerActive_.store(false, std::memory_order_release);
        idleCv_.notify_all();
        if (failed_.load(std::memory_order_acquire)) break;
    }
}

std::uint64_t NativeTranscoderPipeline::muxClockOf(const MuxQueuedSample& sample) noexcept {
    return sample.hasDts ? sample.dts90k : (sample.hasPts ? sample.pts90k : 0);
}

void NativeTranscoderPipeline::enqueueMuxSample(MuxQueuedSample&& sample) {
    sample.queuedAt = std::chrono::steady_clock::now();
    bool dropped = false;
    bool droppedVideo = false;
    bool droppedAudio = false;
    std::size_t queueSize = 0;
    {
        std::lock_guard<std::mutex> lock(muxQueueMutex_);
        auto& queue = sample.kind == mpegts::ElementaryKind::Video ? muxVideoQueue_ : muxAudioQueue_;
        const std::size_t limit = sample.kind == mpegts::ElementaryKind::Video
            ? kMaxMuxVideoQueue : kMaxMuxAudioQueue;

        // V10.8.3 live backpressure policy: a slow peer encoder must never take the
        // whole live HLS/TS output OFFLINE.  Keep the newest encoded samples and
        // discard stale queue head entries when a bounded mux queue is exhausted.
        // For audio this is especially important when video encoding (or one ABR
        // rendition) temporarily falls behind: strict A/V lead intentionally holds
        // audio, so treating a full audio queue as fatal creates a dead stream.
        if (queue.size() >= limit) {
            queue.pop_front();
            dropped = true;
            if (sample.kind == mpegts::ElementaryKind::Video) {
                droppedVideo = true;
                muxDroppedVideo_.fetch_add(1, std::memory_order_relaxed);
            } else {
                droppedAudio = true;
                muxDroppedAudio_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        queue.push_back(std::move(sample));
        queueSize = queue.size();
    }

    if (dropped) {
        const auto count = droppedVideo
            ? muxDroppedVideo_.load(std::memory_order_relaxed)
            : muxDroppedAudio_.load(std::memory_order_relaxed);
        if (count <= 4 || (count % 100U) == 0U) {
            std::cerr << "NATIVE AV MUX DROP kind="
                      << (droppedVideo ? "video" : "audio")
                      << " reason=queue_full keep_live=1 dropped=" << count
                      << " queue=" << queueSize << std::endl;
        }
    }
    (void)droppedAudio;
    muxQueueCv_.notify_one();
}

bool NativeTranscoderPipeline::writeMuxSample(MuxQueuedSample&& queued, std::string& error) {
    mpegts::ElementarySample sample;
    sample.data = queued.data.data();
    sample.size = queued.data.size();
    sample.pts90k = queued.pts90k;
    sample.dts90k = queued.dts90k;
    sample.duration90k = queued.duration90k;
    sample.hasPts = queued.hasPts;
    sample.hasDts = queued.hasDts;
    sample.randomAccess = queued.randomAccess;
    std::vector<mpegts::Packet> packets;
    {
        std::lock_guard<std::mutex> lock(muxMutex_);
        if (!mux_.write(queued.kind, sample, packets, error)) return false;
    }
    appendPackets(packets);
    return true;
}

void NativeTranscoderPipeline::muxWorkerLoop() {
    for (;;) {
        MuxQueuedSample sample;
        std::size_t vqAfter = 0, aqAfter = 0;
        {
            std::unique_lock<std::mutex> lock(muxQueueMutex_);
            muxQueueCv_.wait(lock, [this] {
                return muxStop_.load(std::memory_order_acquire) ||
                       !muxVideoQueue_.empty() || !muxAudioQueue_.empty();
            });
            if (muxStop_.load(std::memory_order_acquire) &&
                muxVideoQueue_.empty() && muxAudioQueue_.empty()) break;

            for (;;) {
                const bool haveVideo = !muxVideoQueue_.empty();
                const bool haveAudio = !muxAudioQueue_.empty();
                if (!haveVideo && !haveAudio) break;

                bool chooseVideo = false;
                bool ready = false;
                if (haveVideo && haveAudio) {
                    chooseVideo = muxClockOf(muxVideoQueue_.front()) <= muxClockOf(muxAudioQueue_.front());
                    ready = true;
                } else {
                    const auto& only = haveVideo ? muxVideoQueue_.front() : muxAudioQueue_.front();
                    const std::uint64_t clock = muxClockOf(only);
                    const std::uint64_t otherClock = haveVideo
                        ? lastMuxAudioClock90k_.load(std::memory_order_acquire)
                        : lastMuxVideoClock90k_.load(std::memory_order_acquire);
                    const auto waited = std::chrono::steady_clock::now() - only.queuedAt;
                    // At startup, give the peer stream time to produce its first encoded sample.
                    // Afterwards keep A/V close, but never let one slow encoder block forever.
                    if (otherClock == 0) {
                        // Before the peer stream has produced its first sample we may
                        // wait briefly, but do not use queue depth as permission to run
                        // one stream far ahead of the other.
                        ready = waited >= kMuxStartupPeerWait;
                    } else {
                        // Once both streams are established, enforce the A/V lead bound.
                        // V10.8 allowed sameQueueDepth>=2 to bypass this check, which let
                        // audio advance by many seconds while video encoding lagged.
                        ready = clock <= otherClock + kMaxAvLead90k;
                    }
                    chooseVideo = haveVideo;
                    if (!ready) {
                        const auto deadline = otherClock == 0
                            ? only.queuedAt + kMuxStartupPeerWait
                            : std::chrono::steady_clock::now() + kMuxSingleStreamWait;
                        muxQueueCv_.wait_until(lock, deadline);
                        if (muxStop_.load(std::memory_order_acquire) &&
                            muxVideoQueue_.empty() && muxAudioQueue_.empty()) break;
                        continue;
                    }
                }

                if (!ready) continue;
                if (chooseVideo) {
                    sample = std::move(muxVideoQueue_.front());
                    muxVideoQueue_.pop_front();
                } else {
                    sample = std::move(muxAudioQueue_.front());
                    muxAudioQueue_.pop_front();
                }
                vqAfter = muxVideoQueue_.size();
                aqAfter = muxAudioQueue_.size();
                muxWorkerActive_.store(true, std::memory_order_release);
                break;
            }
        }

        if (sample.data.empty()) {
            muxWorkerActive_.store(false, std::memory_order_release);
            idleCv_.notify_all();
            if (muxStop_.load(std::memory_order_acquire)) break;
            continue;
        }

        const std::uint64_t clock = muxClockOf(sample);
        if (sample.kind == mpegts::ElementaryKind::Video) {
            const std::uint64_t audioClock = lastMuxAudioClock90k_.load(std::memory_order_acquire);
            if (audioClock != 0 && clock + kMaxAvLead90k < audioClock)
                muxLateVideo_.fetch_add(1, std::memory_order_relaxed);
            lastMuxVideoClock90k_.store(clock, std::memory_order_release);
            muxedVideoSamples_.fetch_add(1, std::memory_order_relaxed);
        } else {
            const std::uint64_t videoClock = lastMuxVideoClock90k_.load(std::memory_order_acquire);
            if (videoClock != 0 && clock + kMaxAvLead90k < videoClock)
                muxLateAudio_.fetch_add(1, std::memory_order_relaxed);
            lastMuxAudioClock90k_.store(clock, std::memory_order_release);
            muxedAudioSamples_.fetch_add(1, std::memory_order_relaxed);
        }

        std::string error;
        if (!writeMuxSample(std::move(sample), error)) setFailure(error);

        const std::uint64_t muxedVideo = muxedVideoSamples_.load(std::memory_order_acquire);
        const std::uint64_t muxedAudio = muxedAudioSamples_.load(std::memory_order_acquire);
        const std::uint64_t total = muxedVideo + muxedAudio;
        if (total != 0 && (total % 250U) == 0U) {
            long long avDeltaMs = 0;
            const std::uint64_t lastVideoClock = lastMuxVideoClock90k_.load(std::memory_order_acquire);
            const std::uint64_t lastAudioClock = lastMuxAudioClock90k_.load(std::memory_order_acquire);
            if (lastVideoClock != 0 && lastAudioClock != 0) {
                avDeltaMs = static_cast<long long>(lastVideoClock) -
                            static_cast<long long>(lastAudioClock);
                avDeltaMs /= 90;
            }
            std::cerr << "NATIVE AV MUX vq=" << vqAfter
                      << " aq=" << aqAfter
                      << " video=" << muxedVideo
                      << " audio=" << muxedAudio
                      << " av_delta_ms=" << avDeltaMs
                      << " late_video=" << muxLateVideo_.load(std::memory_order_acquire)
                      << " late_audio=" << muxLateAudio_.load(std::memory_order_acquire)
                      << " drop_video=" << muxDroppedVideo_.load(std::memory_order_acquire)
                      << " drop_audio=" << muxDroppedAudio_.load(std::memory_order_acquire)
                      << std::endl;
        }

        muxWorkerActive_.store(false, std::memory_order_release);
        idleCv_.notify_all();
        if (failed_.load(std::memory_order_acquire)) break;
    }
}

bool NativeTranscoderPipeline::waitForIdle() {
    for (int i = 0; i < 500; ++i) {
        bool videoEmpty = false, audioEmpty = false, muxVideoEmpty = false, muxAudioEmpty = false;
        { std::lock_guard<std::mutex> lock(videoQueueMutex_); videoEmpty = videoQueue_.empty(); }
        { std::lock_guard<std::mutex> lock(audioQueueMutex_); audioEmpty = audioQueue_.empty(); }
        { std::lock_guard<std::mutex> lock(muxQueueMutex_); muxVideoEmpty = muxVideoQueue_.empty(); muxAudioEmpty = muxAudioQueue_.empty(); }
        if (videoEmpty && audioEmpty && muxVideoEmpty && muxAudioEmpty &&
            !videoWorkerActive_.load(std::memory_order_acquire) &&
            !audioWorkerActive_.load(std::memory_order_acquire) &&
            !muxWorkerActive_.load(std::memory_order_acquire)) return true;
        if (failed_.load(std::memory_order_acquire)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void NativeTranscoderPipeline::drainPendingOutput(std::vector<std::uint8_t>& output) {
    std::lock_guard<std::mutex> lock(outputMutex_);
    output.swap(pendingOutput_);
}

bool NativeTranscoderPipeline::ensureVideoDecoder(mpegts::ElementaryCodec codecType, std::string& error) {
    if (videoDecoder_ && inputVideoCodec_ == codecType) return true;
    videoDecoder_.reset();
    inputVideoCodec_ = codecType;
    videoDecoder_ = codec::createVideoDecoder(codecType, error);
    return static_cast<bool>(videoDecoder_);
}

bool NativeTranscoderPipeline::ensureVideoEncoder(std::string& error) {
    if (videoEncoderConfigured_ && videoEncoder_) return true;
    videoEncoder_ = codec::createVideoEncoder(videoCodecFromName(config_.videoCodec), config_.videoEncoder, error);
    if (!videoEncoder_) return false;
    if (!videoEncoder_->configure(config_.width, config_.height, config_.fps,
                                  config_.videoBitrate, error)) return false;
    videoEncoderConfigured_ = true;
    return true;
}

bool NativeTranscoderPipeline::ensureAudioDecoder(mpegts::ElementaryCodec codecType, std::string& error) {
    if (audioDecoder_ && inputAudioCodec_ == codecType) return true;
    audioDecoder_.reset();
    inputAudioCodec_ = codecType;
    audioDecoder_ = codec::createAudioDecoder(codecType, error);
    return static_cast<bool>(audioDecoder_);
}

bool NativeTranscoderPipeline::ensureAudioEncoder(const codec::PcmAudioFrame&, std::string& error) {
    if (audioEncoderConfigured_) return true;
    if (config_.audioCodec == "aac") {
        aacEncoder_ = codec::createAacEncoder(error);
        if (!aacEncoder_) return false;
        if (!aacEncoder_->configure(48000, 2, config_.audioBitrate, error)) return false;
    } else if (config_.audioCodec == "mp2") {
        mp2Encoder_ = std::make_unique<Mp2Encoder>();
        Mp2EncoderConfig cfg;
        cfg.sampleRate = 48000;
        cfg.channels = 2;
        cfg.bitrate = static_cast<std::uint32_t>(config_.audioBitrate);
        if (!mp2Encoder_->initialize(cfg, error)) return false;
    } else {
        error = "native audio encoder is not configured";
        return false;
    }
    audioEncoderConfigured_ = true;
    return true;
}

bool NativeTranscoderPipeline::handleVideo(mpegts::DemuxSample&& sample, std::string& error) {
    ++inputVideoSamples_;
    // Capture exactly what the selected live H.264 decoder receives.
    // No-op in production unless DVBSTREAMER5_DUMP_H264_ES is explicitly set.
    if (sample.stream.pid == inputVideoPid_) dumpH264ElementarySample(sample);
    if ((inputVideoSamples_ % 100U) == 0U) {
        std::cerr << "NATIVE TRANSCODER VIDEO samples=" << inputVideoSamples_
                  << " decoded=" << decodedVideoFrames_
                  << " pid=" << sample.stream.pid
                  << " bytes=" << sample.data.size()
                  << " pts=" << (sample.hasPts ? std::to_string(sample.pts90k) : std::string("-"))
                  << " dts=" << (sample.hasDts ? std::to_string(sample.dts90k) : std::string("-"))
                  << " random=" << (sample.randomAccess ? 1 : 0)
                  << std::endl;
    }
    if (config_.videoCodec == "copy") return emitCopy(std::move(sample), error);
    if (!ensureVideoDecoder(sample.stream.codec, error) || !ensureVideoEncoder(error)) return false;
    std::vector<codec::RawVideoFrame> decoded;
    if (!videoDecoder_->decode(sample.data.data(), sample.data.size(), sample.pts90k, sample.hasPts, decoded, error)) return false;
    decodedVideoFrames_ += decoded.size();
    for (auto& raw : decoded) {
        if (config_.deinterlace) codec::deinterlaceBlendI420(raw);
        codec::RawVideoFrame scaled;
        const codec::RawVideoFrame* source = &raw;
        if (raw.width != config_.width || raw.height != config_.height) {
            if (!codec::scaleI420(raw, config_.width, config_.height, scaled, error)) return false;
            source = &scaled;
        }
        std::vector<codec::EncodedVideoFrame> encoded;
        if (!videoEncoder_->encode(*source, encoded, error)) return false;
        for (const auto& frame : encoded) if (!emitVideo(frame, error)) return false;
    }
    return true;
}

bool NativeTranscoderPipeline::handleAudio(mpegts::DemuxSample&& sample, std::string& error) {
    ++inputAudioSamples_;
    if ((inputAudioSamples_ % 100U) == 0U) {
        std::cerr << "NATIVE TRANSCODER AUDIO samples=" << inputAudioSamples_
                  << " decoded=" << decodedAudioFrames_
                  << " pid=" << sample.stream.pid
                  << " bytes=" << sample.data.size()
                  << " pts=" << (sample.hasPts ? std::to_string(sample.pts90k) : std::string("-"))
                  << std::endl;
    }
    if (config_.audioCodec == "copy") return emitCopy(std::move(sample), error);
    if (!ensureAudioDecoder(sample.stream.codec, error)) return false;
    std::vector<codec::PcmAudioFrame> decoded;
    if (!audioDecoder_->decode(sample.data.data(), sample.data.size(), sample.pts90k, sample.hasPts, decoded, error)) return false;
    decodedAudioFrames_ += decoded.size();
    for (const auto& pcm : decoded) {
        if (!ensureAudioEncoder(pcm, error)) return false;
        codec::PcmAudioFrame normalized;
        if (!codec::resamplePcm16(pcm, 48000, 2, normalized, error)) return false;
        if (config_.audioCodec == "aac") {
            std::vector<codec::EncodedAudioFrame> encoded;
            if (!aacEncoder_->encode(normalized, encoded, error)) return false;
            for (const auto& frame : encoded) if (!emitAudio(frame, 1920, error)) return false;
        } else {
            const std::size_t frames = normalized.samples.size() / 2U;
            std::vector<std::uint8_t> bytes;
            if (!mp2Encoder_->encodeInterleaved(normalized.samples.data(), frames, bytes, error)) return false;
            if (!bytes.empty()) {
                codec::EncodedAudioFrame frame;
                frame.data = std::move(bytes);
                frame.hasPts = normalized.hasPts;
                frame.pts90k = normalized.pts90k;
                if (!emitAudio(frame, 2160, error)) return false;
            }
        }
    }
    return true;
}

bool NativeTranscoderPipeline::emitVideo(const codec::EncodedVideoFrame& frame, std::string& error) {
    const auto outputCodec = videoCodecFromName(config_.videoCodec);
    if (!videoStartupReady_ && outputCodec != mpegts::ElementaryCodec::Unknown) {
        const auto sets = inspectAnnexBStartup(frame.data, outputCodec);
        const bool ready = outputCodec == mpegts::ElementaryCodec::H264
            ? (frame.keyFrame && sets.h264Idr && sets.h264Sps && sets.h264Pps)
            : (frame.keyFrame && sets.h265Irap && sets.h265Vps && sets.h265Sps && sets.h265Pps);
        if (!ready) {
            ++videoStartupDropped_;
            // Never publish an undecodable first video PES. Audio/PSI may flow
            // while the encoder reaches its first self-contained random-access AU.
            return true;
        }
        videoStartupReady_ = true;
    }
    MuxQueuedSample sample;
    sample.kind = mpegts::ElementaryKind::Video;
    sample.data = frame.data;
    sample.pts90k = frame.pts90k;
    sample.dts90k = frame.dts90k;
    sample.duration90k = videoDuration90k(config_.fps);
    sample.hasPts = frame.hasPts;
    sample.hasDts = frame.hasDts;
    sample.randomAccess = frame.keyFrame;
    enqueueMuxSample(std::move(sample));
    if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> failLock(failureMutex_); error = failure_; return false;
    }
    return true;
}

bool NativeTranscoderPipeline::emitAudio(const codec::EncodedAudioFrame& frame,
                                         std::uint64_t duration90k, std::string& error) {
    MuxQueuedSample sample;
    sample.kind = mpegts::ElementaryKind::Audio;
    sample.data = frame.data;
    sample.pts90k = frame.pts90k;
    sample.dts90k = frame.pts90k;
    sample.duration90k = duration90k;
    sample.hasPts = frame.hasPts;
    sample.hasDts = frame.hasPts;
    enqueueMuxSample(std::move(sample));
    if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> failLock(failureMutex_); error = failure_; return false;
    }
    return true;
}

bool NativeTranscoderPipeline::emitCopy(mpegts::DemuxSample&& sample, std::string& error) {
    if (sample.stream.codec == mpegts::ElementaryCodec::Unknown) return true;
    {
        std::lock_guard<std::mutex> lock(muxMutex_);
        if (!mux_.setCodec(sample.stream.kind, sample.stream.codec, error)) return false;
    }
    MuxQueuedSample queued;
    queued.kind = sample.stream.kind;
    queued.data = std::move(sample.data);
    queued.pts90k = sample.pts90k;
    queued.dts90k = sample.dts90k;
    queued.hasPts = sample.hasPts;
    queued.hasDts = sample.hasDts;
    queued.randomAccess = sample.randomAccess;
    queued.duration90k = sample.stream.kind == mpegts::ElementaryKind::Video
        ? videoDuration90k(config_.fps) : 1920;
    enqueueMuxSample(std::move(queued));
    if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> failLock(failureMutex_); error = failure_; return false;
    }
    return true;
}

void NativeTranscoderPipeline::appendPackets(const std::vector<mpegts::Packet>& packets) {
    std::lock_guard<std::mutex> lock(outputMutex_);
    const std::size_t old = pendingOutput_.size();
    pendingOutput_.resize(old + packets.size() * mpegts::kPacketSize);
    for (std::size_t i = 0; i < packets.size(); ++i)
        std::copy(packets[i].begin(), packets[i].end(), pendingOutput_.begin() + static_cast<std::ptrdiff_t>(old + i * mpegts::kPacketSize));
}

bool NativeTranscoderPipeline::process(const std::uint8_t* data, std::size_t size,
                                       std::vector<std::uint8_t>& output, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear(); output.clear();
    if (!initialized_) { error = "native transcoder is not initialized"; return false; }
    if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> failLock(failureMutex_); error = failure_; return false;
    }
    if (!demux_.push(data, size, error)) return false;
    if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> failLock(failureMutex_); error = failure_; return false;
    }
    drainPendingOutput(output);
    return true;
}

bool NativeTranscoderPipeline::flush(std::vector<std::uint8_t>& output, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear(); output.clear();
    if (!initialized_) return true;
    demux_.flush();
    if (!waitForIdle()) {
        if (failed_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> failLock(failureMutex_); error = failure_;
        } else error = "native transcoder async queues did not drain";
        return false;
    }
    {
        std::lock_guard<std::mutex> codecLock(videoCodecMutex_);
        if (videoEncoder_) {
            std::vector<codec::EncodedVideoFrame> encoded;
            if (!videoEncoder_->flush(encoded, error)) return false;
            for (const auto& frame : encoded) if (!emitVideo(frame, error)) return false;
        }
    }
    {
        std::lock_guard<std::mutex> codecLock(audioCodecMutex_);
        if (aacEncoder_) {
            std::vector<codec::EncodedAudioFrame> encoded;
            if (!aacEncoder_->flush(encoded, error)) return false;
            for (const auto& frame : encoded) if (!emitAudio(frame, 1920, error)) return false;
        }
        if (mp2Encoder_) {
            std::vector<std::uint8_t> bytes;
            if (!mp2Encoder_->finish(bytes, error)) return false;
            if (!bytes.empty()) {
                codec::EncodedAudioFrame frame; frame.data = std::move(bytes);
                if (!emitAudio(frame, 2160, error)) return false;
            }
        }
    }
    if (!waitForIdle()) {
        if (failed_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> failLock(failureMutex_); error = failure_;
        } else error = "native transcoder AV mux scheduler did not drain";
        return false;
    }
    drainPendingOutput(output);
    return true;
}

std::string NativeTranscoderPipeline::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> failLock(failureMutex_);
        return failure_;
    }
    if (!initialized_) return "not initialized";
    std::size_t vq = 0, aq = 0, mvq = 0, maq = 0;
    std::uint64_t vdrop = 0, adrop = 0;
    {
        std::lock_guard<std::mutex> qlock(videoQueueMutex_);
        vq = videoQueue_.size();
        vdrop = droppedVideoSamples_;
    }
    {
        std::lock_guard<std::mutex> qlock(audioQueueMutex_);
        aq = audioQueue_.size();
        adrop = droppedAudioSamples_;
    }
    {
        std::lock_guard<std::mutex> qlock(muxQueueMutex_);
        mvq = muxVideoQueue_.size();
        maq = muxAudioQueue_.size();
    }
    long long avDeltaMs = 0;
    const std::uint64_t statusVideoClock = lastMuxVideoClock90k_.load(std::memory_order_acquire);
    const std::uint64_t statusAudioClock = lastMuxAudioClock90k_.load(std::memory_order_acquire);
    if (statusVideoClock != 0 && statusAudioClock != 0) {
        avDeltaMs = (static_cast<long long>(statusVideoClock) -
                     static_cast<long long>(statusAudioClock)) / 90;
    }
    return "running async vq=" + std::to_string(vq) +
           " aq=" + std::to_string(aq) +
           " mux_vq=" + std::to_string(mvq) +
           " mux_aq=" + std::to_string(maq) +
           " av_delta_ms=" + std::to_string(avDeltaMs) +
           " late_video=" + std::to_string(muxLateVideo_.load(std::memory_order_acquire)) +
           " late_audio=" + std::to_string(muxLateAudio_.load(std::memory_order_acquire)) +
           " vdrop=" + std::to_string(vdrop) +
           " adrop=" + std::to_string(adrop);
}

} // namespace dvbstreamer5::media::transcode
