#include "media/NativeTranscoderPipeline.h"

#include "media/NativeVideoProcessing.h"
#include "media/DiagnosticI420Frame.h"
#include "media/DiagnosticColorI420.h"
#include "media/AvcDecoderSelection.h"
#include "media/NativeIttiamAvc.h"
#include "media/NativeIntelHardwareDecoder.h"
#include "media/NativeFfmpegAvcDecoder.h"
#include "media/PtsClock33.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
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

// V10.8.249: capture byte-exact Annex-B AVC access units for ONE explicitly
// selected stream only. The output path is a prefix and each stream/PID pair
// gets a separate file and CSV index; 64 MiB cap per file, no AU truncation.
// Never dump all configured TV channels when diagnostics are enabled.
void dumpH264ElementarySample(const mpegts::DemuxSample& sample,
                              const std::string& streamId) {
    if (sample.stream.codec != mpegts::ElementaryCodec::H264 ||
        sample.data.empty()) return;
    const char* env = std::getenv("DVBSTREAMER5_DUMP_H264_ES");
    if (!env || !*env) return;
    const char* target = std::getenv("DVBSTREAMER5_DUMP_H264_ES_STREAM_ID");
    if (!target || !*target || streamId.empty() || streamId != target) return;

    struct FileState {
        std::string path;
        std::ofstream es;
        std::ofstream index;
        std::uint64_t samples = 0;
        std::uint64_t bytes = 0;
        bool initialized = false;
        bool limitLogged = false;
    };
    struct DumpState {
        std::mutex mutex;
        std::map<std::string, FileState> files;
    };
    static DumpState dump;
    constexpr std::uint64_t kMaxBytes = 64ULL * 1024ULL * 1024ULL;

    // The same elementary PID can exist on distinct transponders; streamId
    // must be part of both the map key and path. Sanitize filename characters
    // and retain a stable FNV-1a suffix even when sanitization collides.
    const std::string key = streamId + "#" + std::to_string(sample.stream.pid);
    std::lock_guard<std::mutex> lock(dump.mutex);
    auto& file = dump.files[key];
    if (!file.initialized) {
        file.initialized = true;
        std::string safeId;
        safeId.reserve(std::min<std::size_t>(streamId.size(), 64U));
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char ch : streamId) {
            hash ^= ch;
            hash *= 1099511628211ULL;
            if (safeId.size() < 64U) {
                if ((ch >= 'a' && ch <= 'z') ||
                    (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
                    safeId.push_back(static_cast<char>(ch));
                } else {
                    safeId.push_back('_');
                }
            }
        }
        if (safeId.empty()) safeId = "stream";
        std::string base(env);
        if (base.size() >= 5U &&
            base.compare(base.size() - 5U, 5U, ".h264") == 0) {
            base.resize(base.size() - 5U);
        }
        file.path = base + "." + safeId + "." + std::to_string(hash) +
            ".pid-" + std::to_string(sample.stream.pid) + ".h264";
        file.es.open(file.path, std::ios::binary | std::ios::trunc);
        if (file.es) {
            file.index.open(file.path + ".index.csv", std::ios::trunc);
            if (file.index)
                file.index << "sample,size,pts90k,dts90k,has_pts,has_dts,random,pid,stream_id\n";
            std::cerr << "NATIVE AVC ES DUMP open path=" << file.path
                      << " stream_id=" << streamId
                      << " pid=" << sample.stream.pid
                      << " limit_mb=64 split_per_stream=1" << std::endl;
        } else {
            std::cerr << "NATIVE AVC ES DUMP open failed path="
                      << file.path << std::endl;
            return;
        }
    }
    if (!file.es.is_open() || file.limitLogged) return;
    if (sample.data.size() > kMaxBytes - file.bytes) {
        file.limitLogged = true;
        file.es.close();
        if (file.index.is_open()) file.index.close();
        std::cerr << "NATIVE AVC ES DUMP limit reached path=" << file.path
                  << " bytes=" << file.bytes
                  << " last_au_preserved=1" << std::endl;
        return;
    }
    file.es.write(reinterpret_cast<const char*>(sample.data.data()),
                  static_cast<std::streamsize>(sample.data.size()));
    if (!file.es) {
        std::cerr << "NATIVE AVC ES DUMP write failed path="
                  << file.path << std::endl;
        file.es.close();
        if (file.index.is_open()) file.index.close();
        return;
    }
    ++file.samples;
    file.bytes += sample.data.size();
    if (file.index) {
        file.index << file.samples << ',' << sample.data.size() << ','
                   << sample.pts90k << ',' << sample.dts90k << ','
                   << (sample.hasPts ? 1 : 0) << ','
                   << (sample.hasDts ? 1 : 0) << ','
                   << (sample.randomAccess ? 1 : 0) << ','
                   << sample.stream.pid << ',' << streamId << '\n';
        file.index.flush();
    }
    file.es.flush();
    if (file.samples <= 4U || (file.samples % 100U) == 0U) {
        std::cerr << "NATIVE AVC ES DUMP samples=" << file.samples
                  << " pid=" << sample.stream.pid
                  << " stream_id=" << streamId
                  << " bytes=" << file.bytes
                  << " last=" << sample.data.size() << std::endl;
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
    diagnosticI420Enabled_ = false;
    diagnosticColorI420Enabled_ = codec::forceOpenH264ForStream(
        std::getenv("DVBSTREAMER5_DIAG_COLOR_STREAM_ID"), config_.streamId);
    diagnosticColorI420Frames_ = 0;
    if (diagnosticColorI420Enabled_)
        std::cerr << "NATIVE VIDEO DIAG color-i420 enabled stream_id="
                  << config_.streamId << " stage=post-decoder pre-encoder"
                  << " layout=4-quadrants" << std::endl;
    selectedAvcDecoder_ = codec::chooseAvcDecoder(
        config_.videoDecoder, config_.streamId,
        std::getenv("DVBSTREAMER5_DIAG_OPENH264_STREAM_ID"));
    if (selectedAvcDecoder_ == "openh264") {
        std::cerr << "NATIVE AVC DECODER removed=openh264 remap=auto"
                  << " stream_id=" << config_.streamId << std::endl;
        selectedAvcDecoder_ = "auto";
        config_.videoDecoder = "auto";
    }
    ffmpegAvcMuxPolicy_.store(selectedAvcDecoder_ == "ffmpeg" ||
                              selectedAvcDecoder_ == "ittiam",
                              std::memory_order_release);
    softwareAvcSilentSamples_ = 0;
    softwareAvcFallbackUnavailable_ = false;
    diagnosticI420Frames_ = 0;
    if (selectedAvcDecoder_ != "auto") {
        std::cerr << "NATIVE AVC DECODER preference=" << selectedAvcDecoder_
                  << " stream_id=" << config_.streamId
                  << " origin=" << (config_.videoDecoder == "auto" ? "diagnostic-env" : "per-tile")
                  << " zero_copy_h264_bypass=1 output_encoder=unchanged"
                  << std::endl;
    }
    // Only the explicitly named stream is affected; never alter all streams
    // based on a global 'enable' boolean or modify the original DVB source.
    const char* diagnosticTarget = std::getenv("DVBSTREAMER5_DIAG_I420_STREAM_ID");
    if (diagnosticTarget && *diagnosticTarget && !config_.streamId.empty() &&
        config_.streamId == diagnosticTarget && config_.videoCodec != "copy") {
        diagnosticI420Enabled_ = true;
        std::cerr << "NATIVE VIDEO DIAG neutral-i420 enabled stream_id="
                  << config_.streamId
                  << " stage=post-decoder pre-encoder output=gray-grid"
                  << std::endl;
    }
    if (config_.videoCodec != "copy" && videoCodecFromName(config_.videoCodec) == mpegts::ElementaryCodec::Unknown) {
        error = "native transcoder: unsupported video output codec";
        return false;
    }
    if (config_.audioCodec != "copy" && config_.audioCodec != "aac" &&
        config_.audioCodec != "mp2" && config_.audioCodec != "mp3") {
        error = "native transcoder: supported audio output codecs are copy, aac, mp2 and mp3";
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
              << " separate_workers=1 split_decode_encode=1 fps_gate=1 av_mux_scheduler=1 max_av_lead_ms=150 strict_peer_lead=1 live_queue_drop=1" << std::endl;
    return true;
}

void NativeTranscoderPipeline::setExternalVideoInput(bool enabled) {
    externalVideoInput_.store(enabled, std::memory_order_release);
    videoQueueCv_.notify_all();
}

void NativeTranscoderPipeline::setDecodedVideoObserver(DecodedVideoObserver observer) {
    std::lock_guard<std::mutex> lock(decodedVideoObserverMutex_);
    decodedVideoObserver_ = std::move(observer);
}

bool NativeTranscoderPipeline::pushDecodedVideoFrame(
    std::shared_ptr<const codec::RawVideoFrame> frame) {
    if (!frame || !externalVideoInput_.load(std::memory_order_acquire) ||
        failed_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(videoQueueMutex_);
        if (externalVideoQueue_.size() >= kMaxExternalVideoQueue) {
            // Raw frames have no inter-frame decode dependency at this point.
            // Drop the oldest frame only; keeping latency bounded is more
            // important for live ABR than preserving a stale frame.
            externalVideoQueue_.pop_front();
            ++droppedExternalVideoFrames_;
            if (droppedExternalVideoFrames_ <= 4 ||
                (droppedExternalVideoFrames_ % 100U) == 0U) {
                std::cerr << "NATIVE ABR RAW DROP stream_id="
                          << config_.streamId << " dropped="
                          << droppedExternalVideoFrames_
                          << " queue=" << externalVideoQueue_.size()
                          << std::endl;
            }
        }
        externalVideoQueue_.push_back(std::move(frame));
    }
    videoEncodeQueueCv_.notify_one();
    return true;
}

bool NativeTranscoderPipeline::setOutputGeometryIfUnconfigured(
    int width, int height) {
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1))
        return false;
    std::lock_guard<std::mutex> lock(videoEncoderMutex_);
    if (videoEncoderConfigured_) return false;
    if (config_.width == width && config_.height == height) return true;
    config_.width = width;
    config_.height = height;
    std::cerr << "NATIVE VIDEO OUTPUT GEOMETRY runtime="
              << width << "x" << height << std::endl;
    return true;
}

std::pair<int,int> NativeTranscoderPipeline::configuredOutputGeometry() const {
    std::lock_guard<std::mutex> lock(videoEncoderMutex_);
    return {config_.width, config_.height};
}

void NativeTranscoderPipeline::setExternalAudioInput(bool enabled) {
    externalAudioInput_.store(enabled, std::memory_order_release);
}

void NativeTranscoderPipeline::setEncodedAudioObserver(EncodedAudioObserver observer) {
    std::lock_guard<std::mutex> lock(encodedAudioObserverMutex_);
    encodedAudioObserver_ = std::move(observer);
}

bool NativeTranscoderPipeline::pushEncodedAudioFrame(
    mpegts::ElementaryCodec codec,
    const codec::EncodedAudioFrame& frame, std::uint64_t duration90k) {
    if (!externalAudioInput_.load(std::memory_order_acquire) ||
        failed_.load(std::memory_order_acquire) ||
        codec == mpegts::ElementaryCodec::Unknown) return false;

    // Shared ABR audio bypasses this rendition's demux. Only audio=copy needs
    // live codec discovery from the primary PMT. AAC/MP2 transcode renditions
    // already configured their mux codec during initialize(); taking muxMutex_
    // and calling setCodec() for every 21/24-ms audio frame needlessly blocks
    // shared audio behind video packetization on every rendition.
    std::string localError;
    if (config_.audioCodec == "copy" && inputAudioCodec_ != codec) {
        std::lock_guard<std::mutex> lock(muxMutex_);
        if (!mux_.setCodec(mpegts::ElementaryKind::Audio, codec, localError)) {
            setFailure(localError);
            return false;
        }
        inputAudioCodec_ = codec;
    }
    return emitAudio(frame, duration90k, localError);
}

bool NativeTranscoderPipeline::pollOutput(
    std::vector<std::uint8_t>& output, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    output.clear();
    if (!initialized_) {
        error = "native transcoder is not initialized";
        return false;
    }
    if (failed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> failLock(failureMutex_);
        error = failure_;
        return false;
    }
    drainPendingOutput(output);
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
        std::lock_guard<std::mutex> lock(videoDecoderMutex_);
        intelZeroCopy_.reset();
        nvidiaZeroCopy_.reset();
        videoDecoder_.reset();
        intelZeroCopyAttempted_ = false;
        intelZeroCopyFallbackLogged_ = false;
        intelZeroCopyActive_.store(false, std::memory_order_release);
        nvidiaZeroCopyAttempted_ = false;
        nvidiaZeroCopyFallbackLogged_ = false;
        nvidiaZeroCopyActive_.store(false, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(videoEncoderMutex_);
        videoEncoder_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(videoQueueMutex_);
        externalVideoQueue_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(decodedVideoObserverMutex_);
        decodedVideoObserver_ = {};
    }
    {
        std::lock_guard<std::mutex> lock(encodedAudioObserverMutex_);
        encodedAudioObserver_ = {};
    }
    {
        std::lock_guard<std::mutex> lock(audioCodecMutex_);
        audioDecoder_.reset();
        aacEncoder_.reset();
        mp2Encoder_.reset();
        mp3Encoder_.reset();
        mp3NextPts90k_ = 0;
        mp3PtsValid_ = false;
    }
    inputVideoCodec_ = mpegts::ElementaryCodec::Unknown;
    inputAudioCodec_ = mpegts::ElementaryCodec::Unknown;
    inputVideoPid_ = 0xffff;
    inputAudioPid_ = 0xffff;
    videoEncoderConfigured_ = false;
    audioEncoderConfigured_ = false;
    unsupportedAudioPassthroughLogged_ = false;
    videoStartupReady_ = false;
    videoStartupH264Sps_ = false;
    videoStartupH264Pps_ = false;
    videoStartupH265Vps_ = false;
    videoStartupH265Sps_ = false;
    videoStartupH265Pps_ = false;
    videoStartupDropped_ = 0;
    inputVideoSamples_ = 0;
    inputAudioSamples_ = 0;
    videoPesQueuedTotal_.store(0, std::memory_order_release);
    lastTsInputAt_ = {};
    lastVideoPesArrivalAt_ = {};
    tsInputCallsWindow_ = 0;
    tsInputBytesWindow_ = 0;
    tsInputGapMaxMsWindow_ = 0;
    tsInputGapOver80MsWindow_ = 0;
    tsInputGapOver200MsWindow_ = 0;
    pesArrivalsWindow_ = 0;
    pesArrivalGapMaxMsWindow_ = 0;
    pesArrivalGapOver80MsWindow_ = 0;
    pesArrivalGapOver200MsWindow_ = 0;
    pesArrivalBurstUnder5MsWindow_ = 0;
    lastVideoPesPts90kWindow_ = 0;
    lastVideoPesHasPtsWindow_ = false;
    videoPesPtsGapOver80MsWindow_ = 0;
    videoPesPtsGapMaxMsWindow_ = 0;
    decodedVideoFrames_ = 0;
    videoRawPublishedTotal_ = 0;
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
    droppedExternalVideoFrames_ = 0;
    decodedVideoQueueDrops_ = 0;
    videoRateClockValid_ = false;
    videoRateNextPts90k_ = 0;
    videoRateDroppedFrames_ = 0;
    ffmpegPalPtsClock_.reset();
    ffmpegPalPtsCorrectedFrames_ = 0;
    sourceGeometryResolved_ = false;
    sourceWidth_ = 0;
    sourceHeight_ = 0;
    droppedAudioSamples_ = 0;
    muxedVideoSamples_.store(0, std::memory_order_release);
    muxedAudioSamples_.store(0, std::memory_order_release);
    muxLateVideo_.store(0, std::memory_order_release);
    muxLateAudio_.store(0, std::memory_order_release);
    muxDroppedVideo_.store(0, std::memory_order_release);
    muxDroppedAudio_.store(0, std::memory_order_release);
    lastMuxVideoClock90k_.store(0, std::memory_order_release);
    lastMuxAudioClock90k_.store(0, std::memory_order_release);
    lastInputVideoPts90k_.store(0, std::memory_order_release);
    lastInputAudioPts90k_.store(0, std::memory_order_release);
    lastDecodedVideoPts90k_.store(0, std::memory_order_release);
    lastEncodedAudioPts90k_.store(0, std::memory_order_release);
    videoDropUntilRandomAccess_ = false;
    videoResetRequested_.store(false, std::memory_order_release);
    externalVideoInput_.store(false, std::memory_order_release);
    externalAudioInput_.store(false, std::memory_order_release);
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
        {
            std::lock_guard<std::mutex> codecLock(videoDecoderMutex_);
            intelZeroCopy_.reset();
            nvidiaZeroCopy_.reset();
            videoDecoder_.reset();
            softwareAvcSilentSamples_ = 0;
            softwareAvcFallbackUnavailable_ = false;
            intelZeroCopyAttempted_ = false;
            intelZeroCopyFallbackLogged_ = false;
            intelZeroCopyActive_.store(false, std::memory_order_release);
            nvidiaZeroCopyAttempted_ = false;
            nvidiaZeroCopyFallbackLogged_ = false;
            nvidiaZeroCopyActive_.store(false, std::memory_order_release);
        }
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
        if (externalVideoInput_.load(std::memory_order_acquire)) {
            // ABR rendition receives the decoded/deinterlaced frame from the
            // primary pipeline. Still learn the source PID/codec from PMT but
            // never enqueue the same compressed picture for a second decode.
            if (inputVideoPid_ == 0xffff) {
                inputVideoPid_ = sample.stream.pid;
                inputVideoCodec_ = sample.stream.codec;
            }
            return;
        }
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

        // These timestamps are recorded at the demux->PES boundary, not
        // at decode completion. Compare them to TS process() ingress gaps
        // to distinguish input transport stalls from PES assembly stalls.
        // Both are serialized by the pipeline mutex_.
        const auto pesArrivalNow = std::chrono::steady_clock::now();
        if (lastVideoPesArrivalAt_.time_since_epoch().count() != 0) {
            const auto gapMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    pesArrivalNow - lastVideoPesArrivalAt_).count());
            pesArrivalGapMaxMsWindow_ =
                std::max(pesArrivalGapMaxMsWindow_, gapMs);
            if (gapMs > 80) ++pesArrivalGapOver80MsWindow_;
            if (gapMs > 200) ++pesArrivalGapOver200MsWindow_;
            if (gapMs < 5) ++pesArrivalBurstUnder5MsWindow_;
        }
        lastVideoPesArrivalAt_ = pesArrivalNow;
        if (sample.hasPts && lastVideoPesHasPtsWindow_) {
            constexpr std::uint64_t mask = (1ULL << 33U) - 1ULL;
            constexpr std::uint64_t half = (1ULL << 32U);
            const auto ptsDelta =
                (sample.pts90k - lastVideoPesPts90kWindow_) & mask;
            // Ignore negative/reordered source PTS in this forward-gap
            // metric. Normal 25fps video uses ~40ms forward increments.
            if (ptsDelta < half) {
                const std::uint64_t gapMs = ptsDelta / std::uint64_t{90};
                videoPesPtsGapMaxMsWindow_ =
                    std::max(videoPesPtsGapMaxMsWindow_, gapMs);
                if (gapMs > 80) ++videoPesPtsGapOver80MsWindow_;
            }
        }
        lastVideoPesHasPtsWindow_ = sample.hasPts;
        if (sample.hasPts) lastVideoPesPts90kWindow_ = sample.pts90k;

        if (++pesArrivalsWindow_ >= 125) {
            std::cerr << "NATIVE VIDEO INPUT CADENCE"
                      << " stream_id=" << config_.streamId
                      << " pes_window=" << pesArrivalsWindow_
                      << " pes_arrival_gap_max_ms="
                      << pesArrivalGapMaxMsWindow_
                      << " pes_gap_over_80ms="
                      << pesArrivalGapOver80MsWindow_
                      << " pes_gap_over_200ms="
                      << pesArrivalGapOver200MsWindow_
                      << " pes_burst_under_5ms="
                      << pesArrivalBurstUnder5MsWindow_
                      << " pes_pts_gap_max_ms="
                      << videoPesPtsGapMaxMsWindow_
                      << " pes_pts_gap_over_80ms="
                      << videoPesPtsGapOver80MsWindow_
                      << " ts_input_calls=" << tsInputCallsWindow_
                      << " ts_input_bytes=" << tsInputBytesWindow_
                      << " ts_input_gap_max_ms="
                      << tsInputGapMaxMsWindow_
                      << " ts_gap_over_80ms="
                      << tsInputGapOver80MsWindow_
                      << " ts_gap_over_200ms="
                      << tsInputGapOver200MsWindow_
                      << std::endl;
            pesArrivalsWindow_ = 0;
            pesArrivalGapMaxMsWindow_ = 0;
            pesArrivalGapOver80MsWindow_ = 0;
            pesArrivalGapOver200MsWindow_ = 0;
            pesArrivalBurstUnder5MsWindow_ = 0;
            videoPesPtsGapMaxMsWindow_ = 0;
            videoPesPtsGapOver80MsWindow_ = 0;
            tsInputCallsWindow_ = 0;
            tsInputBytesWindow_ = 0;
            tsInputGapMaxMsWindow_ = 0;
            tsInputGapOver80MsWindow_ = 0;
            tsInputGapOver200MsWindow_ = 0;
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
        if (sample.hasPts)
            lastInputVideoPts90k_.store(sample.pts90k & ((1ULL << 33U) - 1ULL),
                                       std::memory_order_relaxed);
        videoQueue_.push_back(std::move(sample));
        videoPesQueuedTotal_.fetch_add(1, std::memory_order_relaxed);
        videoQueueCv_.notify_one();
        return;
    }

    if (externalAudioInput_.load(std::memory_order_acquire)) {
        if (inputAudioPid_ == 0xffff) {
            inputAudioPid_ = sample.stream.pid;
            inputAudioCodec_ = sample.stream.codec;
        }
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
        if (sample.hasPts)
            lastInputAudioPts90k_.store(sample.pts90k & ((1ULL << 33U) - 1ULL),
                                       std::memory_order_relaxed);
        audioQueue_.push_back(std::move(sample));
    }
    audioQueueCv_.notify_one();
}

void NativeTranscoderPipeline::startWorkers() {
    workersStop_.store(false, std::memory_order_release);
    muxStop_.store(false, std::memory_order_release);
    muxWorker_ = std::thread([this] { muxWorkerLoop(); });
    videoWorker_ = std::thread([this] { videoWorkerLoop(); });
    videoEncodeWorker_ = std::thread([this] { videoEncodeWorkerLoop(); });
    audioWorker_ = std::thread([this] { audioWorkerLoop(); });
}

void NativeTranscoderPipeline::stopWorkers() {
    workersStop_.store(true, std::memory_order_release);
    // First stop decoder/encoder workers. They are allowed to finish their
    // currently active sample and may enqueue one last encoded AU.
    {
        std::lock_guard<std::mutex> lock(videoQueueMutex_);
        videoQueue_.clear();
        externalVideoQueue_.clear();
    }
    { std::lock_guard<std::mutex> lock(audioQueueMutex_); audioQueue_.clear(); }
    videoQueueCv_.notify_all();
    videoEncodeQueueCv_.notify_all();
    audioQueueCv_.notify_all();
    if (videoWorker_.joinable()) videoWorker_.join();
    if (videoEncodeWorker_.joinable()) videoEncodeWorker_.join();
    if (audioWorker_.joinable()) audioWorker_.join();
    videoWorkerActive_.store(false, std::memory_order_release);
    videoEncodeWorkerActive_.store(false, std::memory_order_release);
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
    // Distinguish source PES stalls, decoder processing time and PTS/FPS
    // gating from downstream RAW queue/encoder stalls. Observes state only;
    // it never changes queue limits, timestamps or the decoding algorithm.
    std::uint64_t windowPesProcessed = 0;
    std::uint64_t windowProcessTotalUs = 0;
    std::uint64_t windowProcessMaxUs = 0;
    std::uint64_t windowIdleWaitTotalUs = 0;
    std::uint64_t windowIdleWaitMaxUs = 0;
    std::uint64_t windowIdleWaitEvents = 0;
    std::size_t windowPeakPesQueue = 0;
    std::uint64_t lastPesQueuedTotal = 0;
    std::uint64_t lastDecodedFrames = 0;
    std::uint64_t lastPublishedFrames = 0;
    std::uint64_t lastFpsGateDrops = 0;
    auto windowBegin = std::chrono::steady_clock::now();
    for (;;) {
        mpegts::DemuxSample sample;
        const auto beforeDequeue = std::chrono::steady_clock::now();
        bool queueWasEmpty = false;
        {
            std::unique_lock<std::mutex> lock(videoQueueMutex_);
            queueWasEmpty = videoQueue_.empty();
            videoQueueCv_.wait(lock, [this] {
                return workersStop_.load(std::memory_order_acquire) ||
                       !videoQueue_.empty();
            });
            if (workersStop_.load(std::memory_order_acquire) &&
                videoQueue_.empty()) break;
            if (videoQueue_.empty()) continue;

            sample = std::move(videoQueue_.front());
            videoQueue_.pop_front();
            windowPeakPesQueue = std::max(windowPeakPesQueue,
                                          videoQueue_.size());
            videoWorkerActive_.store(true, std::memory_order_release);
        }
        const auto processBegin = std::chrono::steady_clock::now();
        if (queueWasEmpty) {
            const auto idleUs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    processBegin - beforeDequeue).count());
            windowIdleWaitTotalUs += idleUs;
            windowIdleWaitMaxUs = std::max(windowIdleWaitMaxUs, idleUs);
            ++windowIdleWaitEvents;
        }

        std::string error;
        {
            std::lock_guard<std::mutex> codecLock(videoDecoderMutex_);
            if (videoResetRequested_.exchange(false, std::memory_order_acq_rel)) {
                intelZeroCopy_.reset();
                nvidiaZeroCopy_.reset();
                videoDecoder_.reset();
                intelZeroCopyAttempted_ = false;
                intelZeroCopyFallbackLogged_ = false;
                intelZeroCopyActive_.store(false, std::memory_order_release);
                nvidiaZeroCopyAttempted_ = false;
                nvidiaZeroCopyFallbackLogged_ = false;
                nvidiaZeroCopyActive_.store(false, std::memory_order_release);
                videoStartupReady_ = false;
            }
            if (!handleVideo(std::move(sample), error))
                setFailure(error);
        }

        const auto processEnd = std::chrono::steady_clock::now();
        const auto processUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                processEnd - processBegin).count());
        windowProcessTotalUs += processUs;
        windowProcessMaxUs = std::max(windowProcessMaxUs, processUs);
        ++windowPesProcessed;
        if (windowPesProcessed >= 125) {
            std::size_t pesQueueNow = 0;
            std::uint64_t sourceQueueDrops = 0;
            std::uint64_t pesQueuedTotal = 0;
            {
                std::lock_guard<std::mutex> lock(videoQueueMutex_);
                pesQueueNow = videoQueue_.size();
                sourceQueueDrops = droppedVideoSamples_;
                pesQueuedTotal = videoPesQueuedTotal_.load(
                    std::memory_order_relaxed);
            }
            const auto elapsedMs = std::max<std::int64_t>(
                1, std::chrono::duration_cast<std::chrono::milliseconds>(
                    processEnd - windowBegin).count());
            const auto queuedWindow = pesQueuedTotal >= lastPesQueuedTotal
                ? pesQueuedTotal - lastPesQueuedTotal : 0ULL;
            const auto decodedWindow = decodedVideoFrames_ >= lastDecodedFrames
                ? decodedVideoFrames_ - lastDecodedFrames : 0ULL;
            const auto publishedWindow =
                videoRawPublishedTotal_ >= lastPublishedFrames
                    ? videoRawPublishedTotal_ - lastPublishedFrames : 0ULL;
            const auto gateWindow = videoRateDroppedFrames_ >= lastFpsGateDrops
                ? videoRateDroppedFrames_ - lastFpsGateDrops : 0ULL;
            const auto fps = [&](std::uint64_t frames) {
                return frames * 1000ULL /
                    static_cast<std::uint64_t>(elapsedMs);
            };
            std::cerr << "NATIVE VIDEO DECODE PERF"
                      << " stream_id=" << config_.streamId
                      << " pes_processed=" << windowPesProcessed
                      << " pes_in_fps=" << fps(queuedWindow)
                      << " pes_processed_fps=" << fps(windowPesProcessed)
                      << " decoded_fps=" << fps(decodedWindow)
                      << " raw_published_fps=" << fps(publishedWindow)
                      << " decoded_frames=" << decodedWindow
                      << " raw_published_frames=" << publishedWindow
                      << " fps_gate_drop_window=" << gateWindow
                      << " decode_avg_ms=" <<
                          (windowProcessTotalUs / windowPesProcessed / 1000ULL)
                      << " decode_max_ms=" << (windowProcessMaxUs / 1000ULL)
                      << " decode_empty_wait_ms=" <<
                          (windowIdleWaitTotalUs / 1000ULL)
                      << " decode_empty_wait_max_ms=" <<
                          (windowIdleWaitMaxUs / 1000ULL)
                      << " decode_empty_wait_events=" << windowIdleWaitEvents
                      << " pes_queue_now=" << pesQueueNow
                      << " pes_queue_peak=" << windowPeakPesQueue
                      << " pes_queue_drop_total=" << sourceQueueDrops
                      << std::endl;
            lastPesQueuedTotal = pesQueuedTotal;
            lastDecodedFrames = decodedVideoFrames_;
            lastPublishedFrames = videoRawPublishedTotal_;
            lastFpsGateDrops = videoRateDroppedFrames_;
            windowBegin = processEnd;
            windowPesProcessed = 0;
            windowProcessTotalUs = 0;
            windowProcessMaxUs = 0;
            windowIdleWaitTotalUs = 0;
            windowIdleWaitMaxUs = 0;
            windowIdleWaitEvents = 0;
            windowPeakPesQueue = 0;
        }

        videoWorkerActive_.store(false, std::memory_order_release);
        idleCv_.notify_all();
        if (failed_.load(std::memory_order_acquire)) break;
    }
}

void NativeTranscoderPipeline::videoEncodeWorkerLoop() {
    // Measure real encode/scaling throughput without changing scheduling,
    // queue sizes, output PTS or video encoder settings.
    std::uint64_t videoEncodeDiagFrames = 0;
    std::uint64_t videoEncodeDiagTotalUs = 0;
    std::uint64_t videoEncodeDiagMaxUs = 0;
    std::uint64_t videoScaleDiagTotalUs = 0;
    std::uint64_t videoScaleDiagMaxUs = 0;
    std::uint64_t videoCodecDiagTotalUs = 0;
    std::uint64_t videoCodecDiagMaxUs = 0;
    std::size_t videoEncodeDiagPeakRawQueue = 0;
    // Account for producer bursts and time actually blocked waiting for raw
    // frames. A 21-FPS worker with 26ms/encode is NOT necessarily CPU-bound.
    std::uint64_t rawEmptyWaitTotalUs = 0;
    std::uint64_t rawEmptyWaitMaxUs = 0;
    std::uint64_t rawEmptyWaitEvents = 0;
    std::uint64_t lastRawDrops = 0;
    std::size_t lastRawQueue = 0;
    auto videoEncodeDiagWindowStart = std::chrono::steady_clock::now();
    for (;;) {
        std::shared_ptr<const codec::RawVideoFrame> frame;
        const auto dequeueBegin = std::chrono::steady_clock::now();
        bool queueWasEmpty = false;
        {
            std::unique_lock<std::mutex> lock(videoQueueMutex_);
            queueWasEmpty = externalVideoQueue_.empty();
            videoEncodeQueueCv_.wait(lock, [this] {
                return workersStop_.load(std::memory_order_acquire) ||
                       !externalVideoQueue_.empty();
            });
            if (workersStop_.load(std::memory_order_acquire) &&
                externalVideoQueue_.empty()) break;
            if (externalVideoQueue_.empty()) continue;

            frame = std::move(externalVideoQueue_.front());
            externalVideoQueue_.pop_front();
            videoEncodeDiagPeakRawQueue = std::max(
                videoEncodeDiagPeakRawQueue, externalVideoQueue_.size());
            videoEncodeWorkerActive_.store(true, std::memory_order_release);
        }

        const auto encodeBegin = std::chrono::steady_clock::now();
        if (queueWasEmpty) {
            const auto waitedUs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    encodeBegin - dequeueBegin).count());
            rawEmptyWaitTotalUs += waitedUs;
            rawEmptyWaitMaxUs = std::max(rawEmptyWaitMaxUs, waitedUs);
            ++rawEmptyWaitEvents;
        }
        std::uint64_t scaleUs = 0;
        std::uint64_t codecUs = 0;
        std::string error;
        {
            std::lock_guard<std::mutex> codecLock(videoEncoderMutex_);
            if (frame && !handleDecodedVideoFrame(*frame, error,
                                                   &scaleUs, &codecUs))
                setFailure(error);
        }

        const auto encodeEnd = std::chrono::steady_clock::now();
        const auto encodeUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                encodeEnd - encodeBegin).count());
        videoEncodeDiagTotalUs += encodeUs;
        videoEncodeDiagMaxUs = std::max(videoEncodeDiagMaxUs, encodeUs);
        videoScaleDiagTotalUs += scaleUs;
        videoScaleDiagMaxUs = std::max(videoScaleDiagMaxUs, scaleUs);
        videoCodecDiagTotalUs += codecUs;
        videoCodecDiagMaxUs = std::max(videoCodecDiagMaxUs, codecUs);
        ++videoEncodeDiagFrames;
        if (videoEncodeDiagFrames >= 125) {
            std::size_t rawQueue = 0;
            std::uint64_t rawDrops = 0;
            {
                std::lock_guard<std::mutex> lock(videoQueueMutex_);
                rawQueue = externalVideoQueue_.size();
                rawDrops = decodedVideoQueueDrops_;
            }
            const auto elapsedMs = std::max<std::int64_t>(
                1, std::chrono::duration_cast<std::chrono::milliseconds>(
                    encodeEnd - videoEncodeDiagWindowStart).count());
            // Conservation: enqueued RAW = dequeued + dropped + change in
            // queue depth. This measures source burstiness with no producer
            // instrumentation or extra mutex operations in the hot path.
            const auto windowDrops = rawDrops >= lastRawDrops
                ? rawDrops - lastRawDrops : 0ULL;
            const auto rawReceived = static_cast<std::int64_t>(
                videoEncodeDiagFrames) + static_cast<std::int64_t>(windowDrops) +
                static_cast<std::int64_t>(rawQueue) -
                static_cast<std::int64_t>(lastRawQueue);
            const auto rawReceivedSafe = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, rawReceived));
            std::cerr << "NATIVE VIDEO ENCODE PERF"
                      << " stream_id=" << config_.streamId
                      << " frames=" << videoEncodeDiagFrames
                      << " fps=" << (videoEncodeDiagFrames * 1000ULL /
                                     static_cast<std::uint64_t>(elapsedMs))
                      << " avg_ms=" << (videoEncodeDiagTotalUs /
                                        videoEncodeDiagFrames / 1000ULL)
                      << " max_ms=" << (videoEncodeDiagMaxUs / 1000ULL)
                      << " scale_avg_ms=" << (videoScaleDiagTotalUs /
                                               videoEncodeDiagFrames / 1000ULL)
                      << " scale_max_ms=" << (videoScaleDiagMaxUs / 1000ULL)
                      << " encode_avg_ms=" << (videoCodecDiagTotalUs /
                                                videoEncodeDiagFrames / 1000ULL)
                      << " encode_max_ms=" << (videoCodecDiagMaxUs / 1000ULL)
                      << " raw_vq=" << rawQueue
                      << " raw_vq_peak=" << videoEncodeDiagPeakRawQueue
                      << " raw_drop_total=" << rawDrops
                      << " raw_in_fps=" << (rawReceivedSafe * 1000ULL /
                                          static_cast<std::uint64_t>(elapsedMs))
                      << " raw_in_frames=" << rawReceivedSafe
                      << " raw_drop_window=" << windowDrops
                      << " raw_empty_wait_ms=" << (rawEmptyWaitTotalUs / 1000ULL)
                      << " raw_empty_wait_max_ms=" << (rawEmptyWaitMaxUs / 1000ULL)
                      << " raw_empty_wait_events=" << rawEmptyWaitEvents
                      << " video_size=" << config_.width
                      << "x" << config_.height
                      << std::endl;
            videoEncodeDiagWindowStart = encodeEnd;
            lastRawDrops = rawDrops;
            lastRawQueue = rawQueue;
            rawEmptyWaitTotalUs = 0;
            rawEmptyWaitMaxUs = 0;
            rawEmptyWaitEvents = 0;
            videoEncodeDiagFrames = 0;
            videoEncodeDiagTotalUs = 0;
            videoEncodeDiagMaxUs = 0;
            videoScaleDiagTotalUs = 0;
            videoScaleDiagMaxUs = 0;
            videoCodecDiagTotalUs = 0;
            videoCodecDiagMaxUs = 0;
            videoEncodeDiagPeakRawQueue = 0;
        }
        videoEncodeWorkerActive_.store(false, std::memory_order_release);
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
    const char* dropPolicy = "drop_head";
    std::size_t queueSize = 0;
    {
        std::lock_guard<std::mutex> lock(muxQueueMutex_);
        auto& queue = sample.kind == mpegts::ElementaryKind::Video ? muxVideoQueue_ : muxAudioQueue_;
        const std::size_t limit = sample.kind == mpegts::ElementaryKind::Video
            ? kMaxMuxVideoQueue : kMaxMuxAudioQueue;

        // V10.8.223: keep live backpressure recovery monotonic in media time.
        // The old queue-full policy always popped the oldest sample.  When one
        // track was already ahead of its peer, that deleted the nearest PTS the
        // lagging track was trying to catch and moved the queue head even farther
        // into the future.  A transient ~500 ms skew could therefore become a
        // permanent full queue with ever-growing A/V drift and thousands of drops.
        //
        // If the full queue head is already more than the hard A/V envelope ahead
        // of the peer clock, preserve that nearest queued timestamp and discard the
        // newly arriving (even later) sample.  Otherwise retain the established
        // live policy and discard the stale queue head.
        bool dropIncoming = false;
        if (queue.size() >= limit) {
            const std::uint64_t peerClock =
                sample.kind == mpegts::ElementaryKind::Video
                    ? lastMuxAudioClock90k_.load(std::memory_order_acquire)
                    : lastMuxVideoClock90k_.load(std::memory_order_acquire);
            const std::uint64_t headClock = muxClockOf(queue.front());
            const bool headAheadOfPeer =
                peerClock != 0 && mpegts::ptsAheadOf90k(
                    headClock, peerClock, kMaxAvHardLead90k);

            if (headAheadOfPeer) {
                dropIncoming = true;
                dropPolicy = "drop_incoming_preserve_head";
            } else {
                queue.pop_front();
            }

            dropped = true;
            if (sample.kind == mpegts::ElementaryKind::Video) {
                droppedVideo = true;
                muxDroppedVideo_.fetch_add(1, std::memory_order_relaxed);
            } else {
                droppedAudio = true;
                muxDroppedAudio_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!dropIncoming) {
            queue.push_back(std::move(sample));
        }
        queueSize = queue.size();
    }

    if (dropped) {
        const auto count = droppedVideo
            ? muxDroppedVideo_.load(std::memory_order_relaxed)
            : muxDroppedAudio_.load(std::memory_order_relaxed);
        if (count <= 4 || (count % 100U) == 0U) {
            std::cerr << "NATIVE AV MUX DROP"
                      << " stream_id=" << config_.streamId
                      << " kind=" << (droppedVideo ? "video" : "audio")
                      << " reason=queue_full keep_live=1"
                      << " policy=" << dropPolicy
                      << " dropped=" << count
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
    // V304: opt-in hash of the H.264 access unit received by the TS mux.
    if (queued.kind == mpegts::ElementaryKind::Video && config_.videoCodec == "h264") {
        std::lock_guard<std::mutex> diagLock(h264PremuxDiagnosticMutex_);
        if (!muxIntegrityInitialized_) {
            muxIntegrityInitialized_ = true;
            const char* enabled = std::getenv("DVBSTREAMER5_DIAG_H264_MUX_INTEGRITY");
            if (enabled && std::string(enabled) == "1") {
                const std::string prefix = "/tmp/dvbstreamer5-mux-integrity-" +
                    std::to_string(reinterpret_cast<std::uintptr_t>(this));
                muxIntegrityDigestFile_.open(prefix + ".csv", std::ios::out | std::ios::trunc);
                muxIntegrityTsFile_.open(prefix + ".ts", std::ios::binary | std::ios::trunc);
                if (muxIntegrityDigestFile_ && muxIntegrityTsFile_) {
                    muxIntegrityDigestFile_ << "index,bytes,fnv1a64,pts90k,dts90k\n";
                    std::cerr << "NATIVE H264 MUX INTEGRITY csv=" << prefix
                              << ".csv ts=" << prefix << ".ts" << std::endl;
                } else {
                    muxIntegrityDigestFile_.close();
                    muxIntegrityTsFile_.close();
                    std::cerr << "NATIVE H264 MUX INTEGRITY open failed" << std::endl;
                }
            }
        }
        if (muxIntegrityDigestFile_) {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto byte : queued.data) {
                hash ^= byte;
                hash *= 1099511628211ULL;
            }
            muxIntegrityDigestFile_ << muxIntegritySampleIndex_++ << ","
                << queued.data.size() << "," << hash << ","
                << queued.pts90k << "," << queued.dts90k << "\n";
            muxIntegrityDigestFile_.flush();
        }
    }
    std::vector<mpegts::Packet> packets;
    {
        std::lock_guard<std::mutex> lock(muxMutex_);
        if (!mux_.write(queued.kind, sample, packets, error)) return false;
    }
    if (muxIntegrityTsFile_) {
        std::lock_guard<std::mutex> diagLock(h264PremuxDiagnosticMutex_);
        constexpr std::uint64_t kCap = 64ULL * 1024ULL * 1024ULL;
        const auto bytes = static_cast<std::uint64_t>(packets.size()) * mpegts::kPacketSize;
        if (muxIntegrityTsBytes_ + bytes <= kCap) {
            for (const auto& packet : packets)
                muxIntegrityTsFile_.write(
                    reinterpret_cast<const char*>(packet.data()),
                    static_cast<std::streamsize>(mpegts::kPacketSize));
            muxIntegrityTsBytes_ += bytes;
            muxIntegrityTsFile_.flush();
        }
    }
    // V315: compare the exact encoder H.264 access unit against the PES
    // payload reconstructed from the muxer's TS packets. Opt-in, read-only,
    // scoped by stream ID. No changes to encoding, muxing or network pacing.
    if (queued.kind == mpegts::ElementaryKind::Video &&
        config_.videoCodec == "h264") {
        const char* diagId = std::getenv("DVBSTREAMER5_DIAG_H264_PES_STREAM_ID");
        if (diagId && config_.streamId == diagId) {
            static thread_local const NativeTranscoderPipeline* integrityOwner = nullptr;
            static thread_local std::chrono::steady_clock::time_point integrityStart{};
            static thread_local std::uint64_t checked = 0;
            static thread_local std::uint64_t matched = 0;
            static thread_local std::uint64_t mismatched = 0;
            static thread_local std::uint64_t incomplete = 0;
            static thread_local std::uint64_t bytesChecked = 0;
            static thread_local std::uint64_t idrSeen = 0;
            static thread_local std::uint64_t spsSeen = 0;
            static thread_local std::uint64_t ppsSeen = 0;
            static thread_local std::uint64_t idrWithoutSets = 0;
            const auto now = std::chrono::steady_clock::now();
            if (integrityOwner != this) {
                integrityOwner = this;
                integrityStart = now;
                checked = matched = mismatched = incomplete = bytesChecked = 0;
                idrSeen = spsSeen = ppsSeen = idrWithoutSets = 0;
            }
            ++checked;
            // Parse Annex-B start codes to measure SPS/PPS presence per AU.
            bool hasSps = false, hasPps = false, hasIdr = false;
            for (std::size_t pos = 0; pos + 4 < queued.data.size(); ++pos) {
                if (queued.data[pos] != 0 || queued.data[pos + 1] != 0)
                    continue;
                std::size_t prefix = 0;
                if (queued.data[pos + 2] == 1) prefix = 3;
                else if (pos + 4 < queued.data.size() &&
                         queued.data[pos + 2] == 0 &&
                         queued.data[pos + 3] == 1) prefix = 4;
                if (!prefix || pos + prefix >= queued.data.size()) continue;
                const auto type = queued.data[pos + prefix] & 0x1fU;
                if (type == 7) hasSps = true;
                if (type == 8) hasPps = true;
                if (type == 5) hasIdr = true;
                pos += prefix;
            }
            if (hasSps) ++spsSeen;
            if (hasPps) ++ppsSeen;
            if (hasIdr) {
                ++idrSeen;
                if (!hasSps || !hasPps) ++idrWithoutSets;
            }

            // Find the video PES start (0x000001E0..0xEF), then compare
            // its ES bytes across TS boundaries with the original AU.
            std::size_t compared = 0;
            bool foundPes = false;
            bool bad = false;
            std::uint16_t videoPid = 0xffffU;
            for (const auto& ts : packets) {
                if (ts[0] != mpegts::kSyncByte) continue;
                const unsigned afc = (ts[3] >> 4U) & 3U;
                if (!(afc & 1U)) continue;
                const std::uint16_t pid =
                    static_cast<std::uint16_t>(((ts[1] & 0x1fU) << 8U) | ts[2]);
                std::size_t start = 4;
                if (afc & 2U) start += 1U + ts[4];
                if (start >= mpegts::kPacketSize) continue;
                const bool pusi = (ts[1] & 0x40U) != 0;
                if (!foundPes) {
                    if (!pusi || start + 9U > mpegts::kPacketSize) continue;
                    if (ts[start] != 0 || ts[start + 1] != 0 ||
                        ts[start + 2] != 1 ||
                        (ts[start + 3] & 0xf0U) != 0xe0U) continue;
                    const std::size_t header = 9U + ts[start + 8];
                    if (start + header > mpegts::kPacketSize) {
                        bad = true;
                        break;
                    }
                    videoPid = pid;
                    foundPes = true;
                    start += header;
                } else if (pid != videoPid) {
                    continue;
                }
                for (std::size_t j = start;
                     j < mpegts::kPacketSize && compared < queued.data.size(); ++j) {
                    if (ts[j] != queued.data[compared]) {
                        bad = true;
                        break;
                    }
                    ++compared;
                }
                if (bad || compared == queued.data.size()) break;
            }
            bytesChecked += compared;
            if (bad) {
                ++mismatched;
                if (mismatched <= 5) {
                    std::cerr << "NATIVE H264 PES INTEGRITY MISMATCH stream_id="
                              << config_.streamId << " au=" << checked
                              << " compared=" << compared
                              << " expected_bytes=" << queued.data.size()
                              << " pts90k=" << queued.pts90k << std::endl;
                }
            } else if (!foundPes || compared != queued.data.size()) {
                ++incomplete;
                if (incomplete <= 5) {
                    std::cerr << "NATIVE H264 PES INTEGRITY INCOMPLETE stream_id="
                              << config_.streamId << " au=" << checked
                              << " compared=" << compared
                              << " expected_bytes=" << queued.data.size()
                              << " pts90k=" << queued.pts90k << std::endl;
                }
            } else {
                ++matched;
            }
            if (now - integrityStart >= std::chrono::seconds(5)) {
                std::cerr << "NATIVE H264 PES INTEGRITY stream_id=" << config_.streamId
                          << " au_checked=" << checked
                          << " matched=" << matched
                          << " mismatched=" << mismatched
                          << " incomplete=" << incomplete
                          << " bytes_compared=" << bytesChecked
                          << " idr=" << idrSeen
                          << " sps_au=" << spsSeen
                          << " pps_au=" << ppsSeen
                          << " idr_missing_sps_pps=" << idrWithoutSets
                          << std::endl;
                integrityStart = now;
                checked = matched = mismatched = incomplete = bytesChecked = 0;
                idrSeen = spsSeen = ppsSeen = idrWithoutSets = 0;
            }
        }
    }
    // V316: encoded AU bitrate burst telemetry *before* MPEG-TS and CBR pacing.
    // Uses video PTS (90 kHz) for rolling 100-ms and 1-second windows.
    // This is intentionally opt-in and does not change encoded frames.
    if (queued.kind == mpegts::ElementaryKind::Video) {
        const char* burstId = std::getenv("DVBSTREAMER5_DIAG_VIDEO_BURSTS_STREAM_ID");
        if (burstId && config_.streamId == burstId) {
            static thread_local const NativeTranscoderPipeline* burstOwner = nullptr;
            static thread_local std::deque<std::pair<std::uint64_t, std::size_t>> burstSamples;
            static thread_local std::uint64_t burstBytes100 = 0;
            static thread_local std::uint64_t burstBytes1000 = 0;
            static thread_local std::uint64_t burstPeak100Kbps = 0;
            static thread_local std::uint64_t burstPeak1000Kbps = 0;
            static thread_local std::uint64_t burstLargestAuBytes = 0;
            static thread_local std::uint64_t burstLargestIdrBytes = 0;
            static thread_local std::uint64_t burstAuCount = 0;
            static thread_local std::chrono::steady_clock::time_point burstSince{};
            const auto burstNow = std::chrono::steady_clock::now();
            if (burstOwner != this) {
                burstOwner = this;
                burstSamples.clear();
                burstBytes100 = burstBytes1000 = 0;
                burstPeak100Kbps = burstPeak1000Kbps = 0;
                burstLargestAuBytes = burstLargestIdrBytes = burstAuCount = 0;
                burstSince = burstNow;
            }
            // Rolling 100-ms peak from the last 100 ms of video PTS.
            // Recalculate both sums over at most 26 entries (25 fps typical);
            // only on the selected diagnostic stream.
            if (queued.hasPts) {
                constexpr std::uint64_t kMask33 = (1ULL << 33U) - 1ULL;
                constexpr std::uint64_t kHalf33 = (1ULL << 32U);
                const auto pts = queued.pts90k & kMask33;
                if (!burstSamples.empty()) {
                    const auto step = (pts - burstSamples.back().first) & kMask33;
                    if (step >= kHalf33 || step > 90000ULL)
                        burstSamples.clear(); // PTS discontinuity: new window
                }
                burstSamples.emplace_back(pts, queued.data.size());
                while (!burstSamples.empty()) {
                    const auto age = (pts - burstSamples.front().first) & kMask33;
                    if (age < 90000ULL) break;
                    burstSamples.pop_front();
                }
                burstBytes100 = 0;
                burstBytes1000 = 0;
                for (const auto& entry : burstSamples) {
                    burstBytes1000 += entry.second;
                    const auto age = (pts - entry.first) & kMask33;
                    if (age < 9000ULL) burstBytes100 += entry.second;
                }
                burstPeak100Kbps = (std::max)(burstPeak100Kbps,
                    burstBytes100 * std::uint64_t{8} / std::uint64_t{100});
                burstPeak1000Kbps = (std::max)(burstPeak1000Kbps,
                    burstBytes1000 * std::uint64_t{8} / std::uint64_t{1000});
            }
            burstLargestAuBytes = (std::max)(burstLargestAuBytes,
                static_cast<std::uint64_t>(queued.data.size()));
            if (queued.randomAccess)
                burstLargestIdrBytes = (std::max)(burstLargestIdrBytes,
                    static_cast<std::uint64_t>(queued.data.size()));
            ++burstAuCount;
            if (burstNow - burstSince >= std::chrono::seconds(5)) {
                std::cerr << "NATIVE VIDEO BURST DIAG stream_id=" << config_.streamId
                          << " codec=" << config_.videoCodec
                          << " au=" << burstAuCount
                          << " peak_100ms_kbps=" << burstPeak100Kbps
                          << " peak_1s_kbps=" << burstPeak1000Kbps
                          << " max_au_bytes=" << burstLargestAuBytes
                          << " max_idr_au_bytes=" << burstLargestIdrBytes
                          << " encoder_target_kbps=" << (config_.videoBitrate / 1000ULL)
                          << std::endl;
                burstSince = burstNow;
                burstPeak100Kbps = burstPeak1000Kbps = 0;
                burstLargestAuBytes = burstLargestIdrBytes = burstAuCount = 0;
            }
        }
    }
    // V305: low-overhead real-time mux cadence measurements, opt-in.
    // Uses atomics-free thread-local counters: writeMuxSample runs on mux worker.
    if (queued.kind == mpegts::ElementaryKind::Video) {
        static thread_local const NativeTranscoderPipeline* diagOwner = nullptr;
        static thread_local std::chrono::steady_clock::time_point lastVideoMuxAt{};
        static thread_local std::uint64_t videoFrames = 0;
        static thread_local std::uint64_t maxGapMs = 0;
        static thread_local std::uint64_t over80Ms = 0;
        static thread_local std::uint64_t over200Ms = 0;
        static thread_local std::uint64_t maxMuxAgeMs = 0;
        static thread_local std::uint64_t lastPts90k = 0;
        static thread_local bool lastPtsValid = false;
        static thread_local std::uint64_t maxPtsDelta90k = 0;
        static thread_local std::uint64_t ptsOver60 = 0;
        static thread_local std::uint64_t ptsRepeated = 0;
        static thread_local std::uint64_t ptsBackward = 0;
        static thread_local std::chrono::steady_clock::time_point windowStart{};
        const char* diag = std::getenv("DVBSTREAMER5_DIAG_LIVE_CADENCE");
        if (diag && diag[0] == '1' && diag[1] == '\0') {
            const auto now = std::chrono::steady_clock::now();
            if (diagOwner != this) {
                diagOwner = this;
                lastVideoMuxAt = {};
                windowStart = now;
                videoFrames = maxGapMs = over80Ms = over200Ms = maxMuxAgeMs = 0;
                lastPtsValid = false;
                lastPts90k = maxPtsDelta90k = ptsOver60 = ptsRepeated = ptsBackward = 0;
            }
            if (lastVideoMuxAt.time_since_epoch().count() != 0) {
                const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastVideoMuxAt).count();
                if (gap > 0) maxGapMs = std::max(maxGapMs, static_cast<std::uint64_t>(gap));
                if (gap > 80) ++over80Ms;
                if (gap > 200) ++over200Ms;
            }
            lastVideoMuxAt = now;
            if (queued.queuedAt.time_since_epoch().count() != 0) {
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - queued.queuedAt).count();
                if (age > 0) maxMuxAgeMs = std::max(maxMuxAgeMs, static_cast<std::uint64_t>(age));
            }
            if (queued.hasPts) {
                constexpr std::uint64_t kMask = (1ULL << 33U) - 1ULL;
                const auto pts = queued.pts90k & kMask;
                if (lastPtsValid) {
                    const auto forward = (pts - lastPts90k) & kMask;
                    if (forward == 0) ++ptsRepeated;
                    else if (forward >= (1ULL << 32U)) ++ptsBackward;
                    else {
                        maxPtsDelta90k = std::max(maxPtsDelta90k, forward);
                        if (forward > 5400ULL) ++ptsOver60;
                    }
                }
                lastPts90k = pts;
                lastPtsValid = true;
            }
            ++videoFrames;
            if (now - windowStart >= std::chrono::seconds(5)) {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - windowStart).count();
                std::cerr << "NATIVE LIVE CADENCE stream_id=" << config_.streamId
                    << " frames=" << videoFrames
                    << " elapsed_ms=" << elapsedMs
                    << " video_mux_gap_max_ms=" << maxGapMs
                    << " video_mux_gap_over80=" << over80Ms
                    << " video_mux_gap_over200=" << over200Ms
                    << " video_mux_queue_age_max_ms=" << maxMuxAgeMs
                    << " pts_gap_max_ms=" << (maxPtsDelta90k / 90ULL)
                    << " pts_gap_over60=" << ptsOver60
                    << " pts_repeated=" << ptsRepeated
                    << " pts_backward=" << ptsBackward
                    << std::endl;
                videoFrames = maxGapMs = over80Ms = over200Ms = maxMuxAgeMs = 0;
                maxPtsDelta90k = ptsOver60 = ptsRepeated = ptsBackward = 0;
                windowStart = now;
            }
        }
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
                    chooseVideo = !mpegts::ptsAheadOf90k(
                        muxClockOf(muxVideoQueue_.front()),
                        muxClockOf(muxAudioQueue_.front()), 0);
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
                    if (muxDrainRequested_.load(std::memory_order_acquire)) {
                        // At explicit end-of-input there can be no future
                        // peer sample. Drain all remaining video/audio frames
                        // immediately instead of applying live A/V lead holds.
                        // The normal live path below remains unchanged.
                        ready = true;
                    } else if (otherClock == 0) {
                        // Before the peer stream has produced its first sample we may
                        // wait briefly, but do not use queue depth as permission to run
                        // one stream far ahead of the other.
                        ready = waited >= kMuxStartupPeerWait;
                    } else {
                        // Once both streams are established, normally enforce the
                        // 150 ms A/V lead bound. The old loop recomputed
                        // now+kMuxSingleStreamWait on every wake-up, so if one encoder
                        // paused the healthy peer could be held forever. In CBR mode
                        // that produced only NULL packets: CBR Out stayed alive while
                        // Payload Out fell to zero and viewers saw a freeze.
                        const bool withinLead =
                            !mpegts::ptsAheadOf90k(clock, otherClock, kMaxAvLead90k);
                        // V10.8.263 experimental: software AVC decoders use
                        // the same 150-ms envelope. GPU zero-copy retains
                        // the established 500-ms escape hatch.
                        const std::uint64_t hardLead90k =
                            ffmpegAvcMuxPolicy_.load(std::memory_order_acquire)
                                ? kMaxAvLead90k : kMaxAvHardLead90k;
                        const bool withinHardLead =
                            !mpegts::ptsAheadOf90k(clock, otherClock, hardLead90k);
                        // V306 A/B: shorten only the single-video-stream hold.
                        // Never override A/V hard-lead protection or drop frames.
                        const char* videoHoldAb = std::getenv("DVBSTREAMER5_INTEL_MUX_VIDEO_HOLD_AB");
                        const bool shortVideoHold =
                            haveVideo && !ffmpegAvcMuxPolicy_.load(std::memory_order_acquire) &&
                            config_.videoEncoder == "vaapi" &&
                            videoHoldAb && videoHoldAb[0] == '1' && videoHoldAb[1] == '\0';
                        const auto maxHold = shortVideoHold
                            ? std::chrono::milliseconds(200)
                            : std::chrono::duration_cast<std::chrono::milliseconds>(
                                kMuxSingleStreamMaxHold);
                        const bool holdExpired = waited >= maxHold;
                        // Never let bounded-hold recovery accumulate seconds
                        // (or minutes) of A/V skew. The 350 ms escape hatch may
                        // advance the healthy stream only within the selected
                        // hard-lead envelope; the lagging stream catches up.
                        ready = withinLead || (holdExpired && withinHardLead);
                    }
                    chooseVideo = haveVideo;
                    if (!ready) {
                        // V10.8.230: never spin on an expired timed wait.
                        // When the single-stream clock is >500 ms ahead,
                        // waiting longer cannot make the sample eligible:
                        // only arrival of the lagging peer can restore the
                        // A/V hard-lead bound.  Sleep until producer notification
                        // (or stop) rather than issuing thousands of already-
                        // expired pthread_cond_clockwait calls per second.
                        // For an eligible bounded hold, use its exact deadline
                        // instead of periodically waking every 120 ms.
                        const bool peerCanCatchUpOnly =
                            otherClock != 0 &&
                            mpegts::ptsAheadOf90k(
                                clock, otherClock,
                                ffmpegAvcMuxPolicy_.load(std::memory_order_acquire)
                                    ? kMaxAvLead90k : kMaxAvHardLead90k);
                        if (peerCanCatchUpOnly) {
                            muxQueueCv_.wait(lock);
                        } else {
                            const auto deadline = otherClock == 0
                                ? only.queuedAt + kMuxStartupPeerWait
                                : only.queuedAt + (
                                    haveVideo &&
                                    config_.videoEncoder == "vaapi" &&
                                    std::getenv("DVBSTREAMER5_INTEL_MUX_VIDEO_HOLD_AB") &&
                                    std::string(std::getenv("DVBSTREAMER5_INTEL_MUX_VIDEO_HOLD_AB")) == "1"
                                        ? std::chrono::milliseconds(200)
                                        : std::chrono::duration_cast<std::chrono::milliseconds>(
                                            kMuxSingleStreamMaxHold));
                            muxQueueCv_.wait_until(lock, deadline);
                        }
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
            if (audioClock != 0 &&
                mpegts::ptsAheadOf90k(audioClock, clock, kMaxAvLead90k))
                muxLateVideo_.fetch_add(1, std::memory_order_relaxed);
            lastMuxVideoClock90k_.store(clock, std::memory_order_release);
            muxedVideoSamples_.fetch_add(1, std::memory_order_relaxed);
        } else {
            const std::uint64_t videoClock = lastMuxVideoClock90k_.load(std::memory_order_acquire);
            if (videoClock != 0 &&
                mpegts::ptsAheadOf90k(videoClock, clock, kMaxAvLead90k))
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
                avDeltaMs = mpegts::ptsDelta90k(
                    lastVideoClock, lastAudioClock) / 90LL;
            }
            std::cerr << "NATIVE AV MUX"
                      << " stream_id=" << config_.streamId
                      << " vq=" << vqAfter
                      << " aq=" << aqAfter
                      << " video=" << muxedVideo
                      << " audio=" << muxedAudio
                      << " av_delta_ms=" << avDeltaMs
                      << " hard_lead_ms=" <<
                          (ffmpegAvcMuxPolicy_.load(std::memory_order_acquire) ? 150 : 500)
                      << " late_video=" << muxLateVideo_.load(std::memory_order_acquire)
                      << " late_audio=" << muxLateAudio_.load(std::memory_order_acquire)
                      << " drop_video=" << muxDroppedVideo_.load(std::memory_order_acquire)
                      << " drop_audio=" << muxDroppedAudio_.load(std::memory_order_acquire)
                      << std::endl;

            // Source-vs-mux A/V clock telemetry is deliberately limited to
            // streams explicitly using the optional FFmpeg H.264 decoder.
            // The latest-source timestamps are producer watermarks, NOT a
            // measured playback offset; compare repeated stable samples.
            if (ffmpegAvcMuxPolicy_.load(std::memory_order_acquire)) {
                constexpr std::uint64_t mask = (1ULL << 33U) - 1ULL;
                constexpr std::uint64_t half = 1ULL << 32U;
                const auto wrapDeltaMs = [](std::uint64_t a, std::uint64_t b) {
                    if (a == 0 || b == 0) return 999999LL;
                    const auto delta = (a - b) & mask;
                    const auto signedDelta = delta < half
                        ? static_cast<long long>(delta)
                        : static_cast<long long>(delta) -
                              static_cast<long long>(mask + 1ULL);
                    return signedDelta / 90LL;
                };
                const auto inputVideo = lastInputVideoPts90k_.load(std::memory_order_relaxed);
                const auto inputAudio = lastInputAudioPts90k_.load(std::memory_order_relaxed);
                const auto decodedVideo = lastDecodedVideoPts90k_.load(std::memory_order_relaxed);
                const auto encodedAudio = lastEncodedAudioPts90k_.load(std::memory_order_relaxed);
                std::cerr << "NATIVE AV CLOCK TRACE"
                          << " stream_id=" << config_.streamId
                          << " input_av_ms=" << wrapDeltaMs(inputVideo, inputAudio)
                          << " decoded_video_vs_input_ms=" << wrapDeltaMs(decodedVideo, inputVideo)
                          << " encoded_audio_vs_input_ms=" << wrapDeltaMs(encodedAudio, inputAudio)
                          << " mux_av_ms=" << wrapDeltaMs(lastVideoClock, lastAudioClock)
                          << " mux_video_vs_input_ms=" << wrapDeltaMs(lastVideoClock, inputVideo)
                          << " mux_audio_vs_input_ms=" << wrapDeltaMs(lastAudioClock, inputAudio)
                          // FPS-gate and PAL correction counters are owned by
                          // the video worker, so do not read them on the mux
                          // thread without an atomic snapshot.
                          << " mux_video_queue=" << vqAfter
                          << " mux_audio_queue=" << aqAfter
                          << std::endl;
            }
        }

        muxWorkerActive_.store(false, std::memory_order_release);
        idleCv_.notify_all();
        if (failed_.load(std::memory_order_acquire)) break;
    }
}

bool NativeTranscoderPipeline::waitForIdle() {
    for (int i = 0; i < 500; ++i) {
        bool videoEmpty = false, audioEmpty = false, muxVideoEmpty = false, muxAudioEmpty = false;
        {
            std::lock_guard<std::mutex> lock(videoQueueMutex_);
            videoEmpty = videoQueue_.empty() && externalVideoQueue_.empty();
        }
        { std::lock_guard<std::mutex> lock(audioQueueMutex_); audioEmpty = audioQueue_.empty(); }
        { std::lock_guard<std::mutex> lock(muxQueueMutex_); muxVideoEmpty = muxVideoQueue_.empty(); muxAudioEmpty = muxAudioQueue_.empty(); }
        if (videoEmpty && audioEmpty && muxVideoEmpty && muxAudioEmpty &&
            !videoWorkerActive_.load(std::memory_order_acquire) &&
            !videoEncodeWorkerActive_.load(std::memory_order_acquire) &&
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
    if (codecType == mpegts::ElementaryCodec::H264 &&
        selectedAvcDecoder_ == "ittiam") {
        videoDecoder_ = codec::createIttiamAvcDecoder(error);
        if (videoDecoder_)
            std::cerr << "NATIVE AVC DECODER preference selected=ittiam"
                      << " stream_id=" << config_.streamId << std::endl;
    } else if (codecType == mpegts::ElementaryCodec::H264 &&
               selectedAvcDecoder_ == "ffmpeg") {
        videoDecoder_ = codec::createFfmpegAvcDecoder(error);
        if (videoDecoder_)
            std::cerr << "NATIVE AVC DECODER preference selected=ffmpeg"
                      << " stream_id=" << config_.streamId
                      << " encoder=unchanged" << std::endl;
    } else if (codecType == mpegts::ElementaryCodec::H264 &&
               selectedAvcDecoder_ == "intel_hw") {
        videoDecoder_ = codec::createIntelHardwareVideoDecoder(codecType, error);
        if (videoDecoder_)
            std::cerr << "NATIVE AVC DECODER preference selected=intel_hw"
                      << " stream_id=" << config_.streamId
                      << " output=system-memory-i420 encoder_independent=1"
                      << std::endl;
    } else if (codecType == mpegts::ElementaryCodec::H264 &&
               selectedAvcDecoder_ == "nvidia_hw") {
        error = "NVIDIA HW AVC decoder must use the NVDEC/CUDA/NVENC zero-copy path";
        return false;
    } else if (codecType == mpegts::ElementaryCodec::H264 &&
               selectedAvcDecoder_ == "auto") {
        // NVIDIA and Intel zero-copy are probed earlier in processVideo().
        // If no compatible zero-copy path started, try an independent Intel
        // decoder first, then known software backends in a defined order.
        std::string candidateError;
        videoDecoder_ = codec::createIntelHardwareVideoDecoder(codecType, candidateError);
        if (videoDecoder_) {
            selectedAvcDecoder_ = "intel_hw";
            std::cerr << "NATIVE AVC DECODER auto selected=intel_hw"
                      << " stream_id=" << config_.streamId
                      << " reason=hardware-available" << std::endl;
        } else {
            videoDecoder_ = codec::createFfmpegAvcDecoder(candidateError);
            if (videoDecoder_) {
                selectedAvcDecoder_ = "ffmpeg";
                ffmpegAvcMuxPolicy_.store(true, std::memory_order_release);
            } else {
                videoDecoder_ = codec::createIttiamAvcDecoder(candidateError);
                if (videoDecoder_) selectedAvcDecoder_ = "ittiam";
                // No OpenH264 video decoder: FFmpeg and Ittiam are the CPU choices.
            }
            if (videoDecoder_)
                ffmpegAvcMuxPolicy_.store(true, std::memory_order_release);
            if (videoDecoder_)
                std::cerr << "NATIVE AVC DECODER auto selected=" << selectedAvcDecoder_
                          << " stream_id=" << config_.streamId
                          << " reason=hardware-unavailable" << std::endl;
        }
        if (!videoDecoder_) error = "Auto AVC decoder selection failed: " + candidateError;
    } else {
        videoDecoder_ = codec::createVideoDecoder(codecType, error);
    }
    return static_cast<bool>(videoDecoder_);
}

bool NativeTranscoderPipeline::ensureVideoEncoder(std::string& error) {
    if (videoEncoderConfigured_ && videoEncoder_) return true;

    const auto outputCodec = videoCodecFromName(config_.videoCodec);
    const auto configureBackend = [&](const std::string& backend,
                                      std::string& backendError) -> bool {
        backendError.clear();
        auto candidate = codec::createVideoEncoder(
            outputCodec, backend, backendError);
        if (!candidate) return false;
        codec::VideoEncoderOptions options;
        options.preset = config_.preset;
        options.profile = config_.profile;
        options.level = config_.level;
        options.gopFrames = config_.gopFrames;
        options.bFrames = config_.bFrames;
        options.refFrames = config_.refFrames;
        options.openGop = config_.openGop;
        options.aqStrength = config_.aqStrength;
        candidate->setOptions(options);
        if (!candidate->configure(
                config_.width, config_.height, config_.fps,
                config_.videoBitrate, backendError)) {
            return false;
        }
        videoEncoder_ = std::move(candidate);
        videoEncoderConfigured_ = true;
        return true;
    };

    if (config_.videoEncoder == "auto") {
        std::string nvencError;
        if (configureBackend("nvenc", nvencError)) {
            std::cerr << "NATIVE VIDEO ENCODER auto selected=nvenc"
                      << std::endl;
            error.clear();
            return true;
        }
        if (!nvencError.empty()) {
            std::cerr << "NATIVE VIDEO ENCODER auto fallback"
                      << " backend=nvenc error=" << nvencError
                      << std::endl;
        }

        std::string intelError;
        if (configureBackend("qsv", intelError)) {
            std::cerr << "NATIVE VIDEO ENCODER auto selected=qsv-vaapi"
                      << std::endl;
            error.clear();
            return true;
        }
        if (!intelError.empty()) {
            std::cerr << "NATIVE VIDEO ENCODER auto fallback"
                      << " backend=qsv-vaapi error=" << intelError
                      << std::endl;
        }

        std::string cpuError;
        if (configureBackend("cpu", cpuError)) {
            std::cerr << "NATIVE VIDEO ENCODER auto selected=cpu"
                      << std::endl;
            error.clear();
            return true;
        }

        error = cpuError.empty()
            ? "no usable native video encoder backend"
            : cpuError;
        return false;
    }

    return configureBackend(config_.videoEncoder, error);
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
    } else if (config_.audioCodec == "mp3") {
        mp3Encoder_ = std::make_unique<Mp3Encoder>();
        if (!mp3Encoder_->initialize(48000, 2,
              static_cast<std::uint32_t>(config_.audioBitrate), error)) return false;
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
    if (sample.stream.pid == inputVideoPid_)
        dumpH264ElementarySample(sample, config_.streamId);
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

    // OpenH264 is not a dependable decoder for broadcast field-coded AVC.
    // NativeTsDemux already parsed SPS/PPS and confirmed field_pic_flag;
    // route only the affected software tile to the validated FFmpeg path.
    // Never change video encoder, audio timing, or GPU zero-copy selection.
    if (sample.stream.codec == mpegts::ElementaryCodec::H264 &&
        sample.h264FieldCoded &&
        (selectedAvcDecoder_ == "ittiam") &&
        config_.videoDecoder == "auto" &&
        !softwareAvcFallbackUnavailable_) {
        std::string probeError;
        auto recovered = codec::createFfmpegAvcDecoder(probeError);
        if (recovered) {
            const std::string original = selectedAvcDecoder_;
            videoDecoder_ = std::move(recovered);
            selectedAvcDecoder_ = "ffmpeg";
            ffmpegAvcMuxPolicy_.store(true, std::memory_order_release);
            softwareAvcSilentSamples_ = 0;
            ffmpegPalPtsClock_.reset();
            videoRateClockValid_ = false;
            std::cerr << "NATIVE AVC SOFTWARE FALLBACK"
                      << " stream_id=" << config_.streamId
                      << " from=" << original << " to=ffmpeg"
                      << " reason=verified_h264_field_coding"
                      << " encoder=unchanged av_sync=unchanged" << std::endl;
        } else {
            softwareAvcFallbackUnavailable_ = true;
            std::cerr << "NATIVE AVC SOFTWARE FALLBACK unavailable"
                      << " stream_id=" << config_.streamId
                      << " reason=" << probeError << std::endl;
        }
    }

    bool abrRawFanout = false;
    {
        std::lock_guard<std::mutex> observerLock(decodedVideoObserverMutex_);
        abrRawFanout = static_cast<bool>(decodedVideoObserver_);
    }
    if (abrRawFanout && nvidiaZeroCopy_) {
        nvidiaZeroCopy_.reset();
        nvidiaZeroCopyActive_.store(false, std::memory_order_release);
        nvidiaZeroCopyAttempted_ = true;
        std::cerr << "NATIVE NVIDIA ZERO-COPY disabled reason=abr_shared_raw "
                     "fallback=shared_cpu_decode"
                  << std::endl;
    }
    if (abrRawFanout && intelZeroCopy_) {
        intelZeroCopy_.reset();
        intelZeroCopyActive_.store(false, std::memory_order_release);
        intelZeroCopyAttempted_ = true;
        std::cerr << "NATIVE INTEL ZERO-COPY disabled reason=abr_shared_raw "
                     "fallback=shared_cpu_decode"
                  << std::endl;
    }

    // Explicit frame-producing decoders bypass whole-pipeline zero-copy.
    // Intel HW is now an encoder-independent oneVPL decoder that returns I420.
    const bool explicitFrameDecoder =
        sample.stream.codec == mpegts::ElementaryCodec::H264 &&
        (selectedAvcDecoder_ == "ittiam" ||
         selectedAvcDecoder_ == "ffmpeg" ||
         selectedAvcDecoder_ == "intel_hw");
    const bool forceNvidiaHardwareAvc =
        sample.stream.codec == mpegts::ElementaryCodec::H264 &&
        selectedAvcDecoder_ == "nvidia_hw";
    const bool zeroCopyCodec =
        sample.stream.codec == mpegts::ElementaryCodec::Mpeg2Video ||
        sample.stream.codec == mpegts::ElementaryCodec::H264 ||
        sample.stream.codec == mpegts::ElementaryCodec::H265;
    const bool zeroCopyBackend =
        config_.videoEncoder == "nvenc" || config_.videoEncoder == "auto";
    if (forceNvidiaHardwareAvc && !zeroCopyBackend) {
        error = "NVIDIA HW decoder requires video encoder=nvenc or auto because "
                "the current NVIDIA decoder is the NVDEC/CUDA/NVENC zero-copy path";
        return false;
    }
    if (!explicitFrameDecoder && !abrRawFanout &&
        zeroCopyCodec && zeroCopyBackend) {
        if (!nvidiaZeroCopyAttempted_) {
            nvidiaZeroCopyAttempted_ = true;
            std::string zeroCopyError;
            if (codec::nvidiaZeroCopyRuntimeAvailable()) {
                codec::NvidiaZeroCopyConfig zeroCopyConfig;
                zeroCopyConfig.inputCodec = sample.stream.codec;
                zeroCopyConfig.outputCodec = videoCodecFromName(config_.videoCodec);
                zeroCopyConfig.width = config_.width;
                zeroCopyConfig.height = config_.height;
                zeroCopyConfig.fps = config_.fps;
                zeroCopyConfig.bitrate = config_.videoBitrate;
                zeroCopyConfig.deinterlace = config_.deinterlace;
                zeroCopyConfig.preset = config_.preset;
                zeroCopyConfig.profile = config_.profile;
                zeroCopyConfig.level = config_.level;
                zeroCopyConfig.gopFrames = config_.gopFrames;
                zeroCopyConfig.bFrames = config_.bFrames;
                zeroCopyConfig.refFrames = config_.refFrames;
                zeroCopyConfig.openGop = config_.openGop;
                zeroCopyConfig.aqStrength = config_.aqStrength;
                zeroCopyConfig.lockOutputGeometry = config_.lockOutputGeometry;
                nvidiaZeroCopy_ = codec::createNvidiaZeroCopyTranscoder(
                    zeroCopyConfig, zeroCopyError);
            } else {
                const char* disabled = std::getenv("DVBSTREAMER5_DISABLE_NVIDIA_ZERO_COPY");
                zeroCopyError = disabled && std::string(disabled) == "1"
                    ? "NVIDIA zero-copy disabled by DVBSTREAMER5_DISABLE_NVIDIA_ZERO_COPY=1"
                    : "NVDEC/CUDA/NVENC runtime not available";
            }
            if (nvidiaZeroCopy_) {
                nvidiaZeroCopyActive_.store(true, std::memory_order_release);
                const char* inputName =
                    sample.stream.codec == mpegts::ElementaryCodec::Mpeg2Video ? "mpeg2" :
                    sample.stream.codec == mpegts::ElementaryCodec::H264 ? "h264" : "hevc";
                std::cerr << "NATIVE NVIDIA ZERO-COPY selected input="
                          << inputName
                          << " output=" << config_.videoCodec
                          << " backend=" << config_.videoEncoder
                          << std::endl;
            } else if (!nvidiaZeroCopyFallbackLogged_) {
                nvidiaZeroCopyFallbackLogged_ = true;
                std::cerr << "NATIVE NVIDIA ZERO-COPY unavailable reason="
                          << zeroCopyError << " fallback="
                          << (config_.videoEncoder == "nvenc"
                                  ? "cpu-decode+nvenc" : "standard-auto-path")
                          << std::endl;
            }
        }

        if (nvidiaZeroCopy_) {
    std::vector<codec::EncodedVideoFrame> encoded;
    std::string nvidiaProcessError;
    if (!nvidiaZeroCopy_->process(
            sample.data.data(), sample.data.size(),
            sample.pts90k, sample.hasPts, encoded, nvidiaProcessError)) {
        std::cerr << "NATIVE NVIDIA ZERO-COPY runtime fallback reason="
                  << nvidiaProcessError << " fallback="
                  << (config_.videoEncoder == "nvenc"
                          ? "cpu-decode+nvenc" : "standard-auto-path")
                  << std::endl;
        nvidiaZeroCopy_.reset();
        nvidiaZeroCopyActive_.store(false, std::memory_order_release);
        nvidiaZeroCopyFallbackLogged_ = true;
        error.clear();
    } else {
        decodedVideoFrames_ += encoded.size();
        if (!sourceGeometryResolved_ &&
            nvidiaZeroCopy_->sourceWidth() > 0 &&
            nvidiaZeroCopy_->sourceHeight() > 0) {
            sourceGeometryResolved_ = true;
            sourceWidth_ = nvidiaZeroCopy_->sourceWidth();
            sourceHeight_ = nvidiaZeroCopy_->sourceHeight();
            const int outWidth = nvidiaZeroCopy_->outputWidth();
            const int outHeight = nvidiaZeroCopy_->outputHeight();
            if (outWidth > 0 && outHeight > 0 &&
                (outWidth != config_.width || outHeight != config_.height))
                (void)setOutputGeometryIfUnconfigured(outWidth, outHeight);
            std::cerr << "NATIVE VIDEO SOURCE GEOMETRY "
                      << sourceWidth_ << "x" << sourceHeight_
                      << " output=" << config_.width << "x" << config_.height
                      << " path=nvidia-zero-copy" << std::endl;
        }
        for (const auto& frame : encoded)
            if (!emitVideo(frame, error)) return false;
        return true;
    }
}
}

    const bool intelZeroCopyCodec =
        sample.stream.codec == mpegts::ElementaryCodec::H264 ||
        sample.stream.codec == mpegts::ElementaryCodec::H265 ||
        sample.stream.codec == mpegts::ElementaryCodec::Mpeg2Video;
    if (forceNvidiaHardwareAvc && !nvidiaZeroCopy_) {
        error = "NVIDIA HW decoder requested but NVDEC/CUDA/NVENC zero-copy is unavailable";
        return false;
    }

    const bool intelZeroCopyBackend =
        config_.videoEncoder == "qsv" || config_.videoEncoder == "auto";
    if (!explicitFrameDecoder && !forceNvidiaHardwareAvc && !abrRawFanout && !nvidiaZeroCopy_ &&
        intelZeroCopyCodec && intelZeroCopyBackend) {
        if (!intelZeroCopyAttempted_) {
            intelZeroCopyAttempted_ = true;
            std::string zeroCopyError;
            if (codec::intelZeroCopyRuntimeAvailable()) {
                codec::IntelZeroCopyConfig zeroCopyConfig;
                zeroCopyConfig.inputCodec = sample.stream.codec;
                zeroCopyConfig.outputCodec = videoCodecFromName(config_.videoCodec);
                zeroCopyConfig.width = config_.width;
                zeroCopyConfig.height = config_.height;
                zeroCopyConfig.fps = config_.fps;
                zeroCopyConfig.bitrate = config_.videoBitrate;
                zeroCopyConfig.deinterlace = config_.deinterlace;
                zeroCopyConfig.preset = config_.preset;
                zeroCopyConfig.profile = config_.profile;
                zeroCopyConfig.level = config_.level;
                zeroCopyConfig.gopFrames = config_.gopFrames;
                zeroCopyConfig.bFrames = config_.bFrames;
                zeroCopyConfig.refFrames = config_.refFrames;
                zeroCopyConfig.openGop = config_.openGop;
                zeroCopyConfig.aqStrength = config_.aqStrength;
                zeroCopyConfig.lockOutputGeometry = config_.lockOutputGeometry;
                intelZeroCopy_ = codec::createIntelZeroCopyTranscoder(
                    zeroCopyConfig, zeroCopyError);
            } else {
                zeroCopyError = "oneVPL hardware runtime API 2.1+ not available";
            }
            if (intelZeroCopy_) {
                intelZeroCopyActive_.store(true, std::memory_order_release);
                const char* inputName =
                    sample.stream.codec == mpegts::ElementaryCodec::H264 ? "h264" :
                    sample.stream.codec == mpegts::ElementaryCodec::H265 ? "hevc" : "mpeg2";
                std::cerr << "NATIVE INTEL ZERO-COPY selected input="
                          << inputName
                          << " output=" << config_.videoCodec
                          << " backend=" << config_.videoEncoder
                          << std::endl;
            } else if (!intelZeroCopyFallbackLogged_) {
                intelZeroCopyFallbackLogged_ = true;
                std::cerr << "NATIVE INTEL ZERO-COPY unavailable reason="
                          << zeroCopyError << " fallback="
                          << (config_.videoEncoder == "qsv"
                                  ? "cpu-decode+qsv" : "standard-auto-path")
                          << std::endl;
            }
        }

        if (intelZeroCopy_) {
            std::vector<codec::EncodedVideoFrame> encoded;
            std::string intelProcessError;
            if (!intelZeroCopy_->process(
                    sample.data.data(), sample.data.size(),
                    sample.pts90k, sample.hasPts, encoded, intelProcessError)) {
                std::cerr << "NATIVE INTEL ZERO-COPY runtime fallback reason="
                          << intelProcessError << " fallback="
                          << (config_.videoEncoder == "qsv"
                                  ? "cpu-decode+qsv" : "standard-auto-path")
                          << std::endl;
                intelZeroCopy_.reset();
                intelZeroCopyActive_.store(false, std::memory_order_release);
                intelZeroCopyFallbackLogged_ = true;
                error.clear();
            } else {
                decodedVideoFrames_ += encoded.size();
                if (!sourceGeometryResolved_ &&
                    intelZeroCopy_->sourceWidth() > 0 &&
                    intelZeroCopy_->sourceHeight() > 0) {
                    sourceGeometryResolved_ = true;
                    sourceWidth_ = intelZeroCopy_->sourceWidth();
                    sourceHeight_ = intelZeroCopy_->sourceHeight();
                    const int outWidth = intelZeroCopy_->outputWidth();
                    const int outHeight = intelZeroCopy_->outputHeight();
                    if (outWidth > 0 && outHeight > 0 &&
                        (outWidth != config_.width || outHeight != config_.height))
                        (void)setOutputGeometryIfUnconfigured(outWidth, outHeight);
                    std::cerr << "NATIVE VIDEO SOURCE GEOMETRY "
                              << sourceWidth_ << "x" << sourceHeight_
                              << " output=" << config_.width << "x" << config_.height
                              << " path=intel-zero-copy" << std::endl;
                }
                for (const auto& frame : encoded)
                    if (!emitVideo(frame, error)) return false;
                return true;
            }
        }
    }

    if (forceNvidiaHardwareAvc) {
        error = "NVIDIA HW decoder requested but no NVIDIA hardware decode path was selected";
        return false;
    }
    if (!ensureVideoDecoder(sample.stream.codec, error)) return false;

    std::vector<codec::RawVideoFrame> decoded;
    bool decodedOk = videoDecoder_->decode(
        sample.data.data(), sample.data.size(),
        sample.pts90k, sample.hasPts, decoded, error);

    // A decoder can report success while continuously returning zero frames
    // for an unsupported AVC bitstream. Try FFmpeg once after sustained
    // silence; for a fatal native error try it immediately. This is a
    // per-tile recovery only, and never touches an already-working FFmpeg
    // decoder, HEVC, MPEG-2 or hardware zero-copy.
    const bool nativeSoftwareAvc =
        sample.stream.codec == mpegts::ElementaryCodec::H264 &&
        (selectedAvcDecoder_ == "ittiam" ||
         selectedAvcDecoder_ == "auto");
    if (nativeSoftwareAvc) {
        if (decodedOk && !decoded.empty()) {
            softwareAvcSilentSamples_ = 0;
        } else if (decodedOk) {
            ++softwareAvcSilentSamples_;
        }
        if ((!decodedOk || softwareAvcSilentSamples_ >= 150U) &&
            config_.videoDecoder == "auto" &&
            !softwareAvcFallbackUnavailable_) {
            const std::string original = selectedAvcDecoder_;
            const std::string nativeError = error;
            std::string fallbackError;
            auto recovered = codec::createFfmpegAvcDecoder(fallbackError);
            if (recovered) {
                videoDecoder_ = std::move(recovered);
                selectedAvcDecoder_ = "ffmpeg";
                ffmpegAvcMuxPolicy_.store(true, std::memory_order_release);
                softwareAvcSilentSamples_ = 0;
                ffmpegPalPtsClock_.reset();
                videoRateClockValid_ = false;
                decoded.clear();
                error.clear();
                decodedOk = videoDecoder_->decode(
                    sample.data.data(), sample.data.size(),
                    sample.pts90k, sample.hasPts, decoded, error);
                std::cerr << "NATIVE AVC SOFTWARE FALLBACK"
                          << " stream_id=" << config_.streamId
                          << " from=" << original << " to=ffmpeg"
                          << " reason=" << (nativeError.empty()
                                ? "no_decoded_frames" : "decoder_error")
                          << " encoder=unchanged av_sync=unchanged"
                          << std::endl;
            } else {
                softwareAvcFallbackUnavailable_ = true;
                if (!nativeError.empty()) error = nativeError;
                std::cerr << "NATIVE AVC SOFTWARE FALLBACK unavailable"
                          << " stream_id=" << config_.streamId
                          << " reason=" << fallbackError << std::endl;
            }
        }
    }
    if (!decodedOk) return false;

    decodedVideoFrames_ += decoded.size();
    for (auto& raw : decoded) {
        if (!sourceGeometryResolved_ && raw.width > 0 && raw.height > 0) {
            sourceGeometryResolved_ = true;
            sourceWidth_ = raw.width;
            sourceHeight_ = raw.height;
            const std::uint64_t sourcePixels =
                static_cast<std::uint64_t>(raw.width) *
                static_cast<std::uint64_t>(raw.height);
            const std::uint64_t requestedPixels =
                static_cast<std::uint64_t>(std::max(0, config_.width)) *
                static_cast<std::uint64_t>(std::max(0, config_.height));
            if (!config_.lockOutputGeometry && requestedPixels > sourcePixels) {
                (void)setOutputGeometryIfUnconfigured(raw.width, raw.height);
            }
            std::cerr << "NATIVE VIDEO SOURCE GEOMETRY "
                      << raw.width << "x" << raw.height
                      << " output=" << config_.width << "x" << config_.height
                      << std::endl;
        }

        // FFmpeg delivers decoded H.264 in display order, but interlaced
        // broadcast PTS may wobble through the field-pair reorder delay.
        // For a VERIFIED 25-fps PAL cadence only, reconstruct its steady
        // display clock before the generic FPS gate. No change for Ittiam,
        // OpenH264, NVDEC, progressive or 30/50-fps sources.
        if (selectedAvcDecoder_ == "ffmpeg" && raw.interlaced &&
            raw.hasPts && config_.fps >= 24.99 && config_.fps <= 25.01) {
            const auto pal = ffmpegPalPtsClock_.apply(raw.pts90k);
            if (pal.discontinuity || pal.locked) {
                // The accepted PTS lock now inherits the *current* source
                // timestamp. Reset the gate's old first-frame anchor so the
                // new source-aligned cadence cannot cause a burst of drops.
                videoRateClockValid_ = false;
            }
            if (pal.discontinuity) {
                std::cerr << "NATIVE AVC FFMPEG PTS discontinuity"
                          << " stream_id=" << config_.streamId
                          << " source_pts=" << raw.pts90k
                          << " action=reanchor" << std::endl;
            }
            if (pal.corrected) {
                raw.pts90k = pal.pts90k;
                raw.dts90k = pal.pts90k;
                raw.hasDts = true;
                ++ffmpegPalPtsCorrectedFrames_;
                if (pal.locked || ffmpegPalPtsCorrectedFrames_ % 500U == 0U) {
                    std::cerr << "NATIVE AVC FFMPEG PTS cadence"
                              << " stream_id=" << config_.streamId
                              << " fps=25 locked=1 corrected="
                              << ffmpegPalPtsCorrectedFrames_
                              << " source_delta90k=" << pal.sourceDelta90k
                              << " output_pts=" << raw.pts90k
                              << std::endl;
                }
            }
        }

        // All ABR renditions share this post-decode frame. Gate once here so
        // a 29.97/30 fps source does not feed ~30 frames/s into encoders that
        // are configured for 25 fps. Dropping before deinterlace/VPP/queues
        // keeps every rendition frame-aligned and avoids queue-overflow drops.
        // A 25-fps interlaced FFmpeg decode can carry reordered/field-pair PTS
        // that are not a reliable 50-fps display clock. At >=50-fps output
        // the gate must not remove any 25-fps decoded frames: it cannot
        // synthesize new motion samples, and rejecting originals causes
        // visible ticker judder (observed ~16/25 fps on TV3Bel).
        // Keep existing gate behavior for 25-fps conversion and all other
        // decoders / progressive sources.
        const bool preserveInterlacedFfmpegFrames =
            selectedAvcDecoder_ == "ffmpeg" && raw.interlaced &&
            config_.fps >= 49.5;
        if (raw.hasPts && config_.fps > 0.0 &&
            !preserveInterlacedFfmpegFrames) {
            constexpr std::uint64_t kPtsMask = (1ULL << 33U) - 1ULL;
            constexpr std::uint64_t kPtsHalf = 1ULL << 32U;
            const std::uint64_t interval =
                std::max<std::uint64_t>(1ULL, videoDuration90k(config_.fps));
            const std::uint64_t pts = raw.pts90k & kPtsMask;
            if (!videoRateClockValid_) {
                videoRateClockValid_ = true;
                videoRateNextPts90k_ = pts;
            }
            const auto atOrAfter = [&](std::uint64_t a, std::uint64_t b) {
                return ((a - b) & kPtsMask) < kPtsHalf;
            };
            if (!atOrAfter(pts, videoRateNextPts90k_)) {
                ++videoRateDroppedFrames_;
                if (videoRateDroppedFrames_ <= 4 ||
                    (videoRateDroppedFrames_ % 250U) == 0U) {
                    std::cerr << "NATIVE VIDEO FPS GATE dropped="
                              << videoRateDroppedFrames_
                              << " target_fps=" << config_.fps
                              << " pts=" << pts << std::endl;
                }
                continue;
            }
            do {
                videoRateNextPts90k_ =
                    (videoRateNextPts90k_ + interval) & kPtsMask;
            } while (atOrAfter(pts, videoRateNextPts90k_));
        }

        if (raw.hasPts)
            lastDecodedVideoPts90k_.store(raw.pts90k & ((1ULL << 33U) - 1ULL),
                                         std::memory_order_relaxed);

        if (config_.deinterlace) codec::deinterlaceBlendI420(raw);

        // Replace ONLY the decoded pixel buffer on the named diagnostic tile.
        // Keep original input decode, timing, geometry, and all downstream
        // scalers/encoders/muxers to localize the cause of colored stripes.
        if (diagnosticColorI420Enabled_) {
            if (codec::fillDiagnosticColorI420(raw, diagnosticColorI420Frames_++) &&
                (diagnosticColorI420Frames_ == 1 ||
                 (diagnosticColorI420Frames_ % 500U) == 0U))
                std::cerr << "NATIVE VIDEO DIAG color-i420 frame="
                          << diagnosticColorI420Frames_
                          << " stream_id=" << config_.streamId
                          << " size=" << raw.width << "x" << raw.height
                          << std::endl;
        } else if (diagnosticI420Enabled_ &&
                   codec::fillDiagnosticNeutralI420(raw, diagnosticI420Frames_++)) {
            if (diagnosticI420Frames_ == 1 ||
                (diagnosticI420Frames_ % 500U) == 0U)
                std::cerr << "NATIVE VIDEO DIAG neutral-i420 frame="
                          << diagnosticI420Frames_
                          << " stream_id=" << config_.streamId
                          << " size=" << raw.width << "x" << raw.height
                          << std::endl;
        }

        auto sharedRaw =
            std::make_shared<codec::RawVideoFrame>(std::move(raw));

        DecodedVideoObserver observer;
        {
            std::lock_guard<std::mutex> lock(decodedVideoObserverMutex_);
            observer = decodedVideoObserver_;
        }
        if (observer) observer(sharedRaw);

        {
            std::lock_guard<std::mutex> qlock(videoQueueMutex_);
            if (externalVideoQueue_.size() >= kMaxExternalVideoQueue) {
                externalVideoQueue_.pop_front();
                ++decodedVideoQueueDrops_;
                if (decodedVideoQueueDrops_ <= 4 ||
                    (decodedVideoQueueDrops_ % 100U) == 0U) {
                    std::cerr
                        << "NATIVE PRIMARY RAW DROP stream_id="
                        << config_.streamId << " dropped="
                        << decodedVideoQueueDrops_
                        << " queue=" << externalVideoQueue_.size()
                        << std::endl;
                }
            }
            externalVideoQueue_.push_back(std::move(sharedRaw));
        }
        ++videoRawPublishedTotal_;
        videoEncodeQueueCv_.notify_one();
    }
    return true;
}

bool NativeTranscoderPipeline::handleDecodedVideoFrame(
    const codec::RawVideoFrame& raw, std::string& error,
    std::uint64_t* scaleTimeUs, std::uint64_t* encodeTimeUs) {
    if (scaleTimeUs) *scaleTimeUs = 0;
    if (encodeTimeUs) *encodeTimeUs = 0;
    if (!ensureVideoEncoder(error)) return false;

    const auto scaleBegin = std::chrono::steady_clock::now();
    codec::RawVideoFrame scaled;
    const codec::RawVideoFrame* source = &raw;
    if ((raw.width != config_.width || raw.height != config_.height) &&
        !videoEncoder_->supportsNativeScaling()) {
        if (!codec::scaleI420(raw, config_.width, config_.height, scaled, error))
            return false;
        source = &scaled;
    }
    const auto scaleEnd = std::chrono::steady_clock::now();
    if (scaleTimeUs) {
        *scaleTimeUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                scaleEnd - scaleBegin).count());
    }

    std::vector<codec::EncodedVideoFrame> encoded;
    if (!videoEncoder_->encode(*source, encoded, error)) return false;
    for (const auto& frame : encoded)
        if (!emitVideo(frame, error)) return false;
    if (encodeTimeUs) {
        *encodeTimeUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - scaleEnd).count());
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

    // E-AC-3 is not yet decoded by the current portable native codec core.
    // Preserve it instead of failing the whole relay. AC-3 is handled above
    // by the vendored native codec-ac3 decoder and can be transcoded to AAC/MP2.
    if (sample.stream.codec == mpegts::ElementaryCodec::Eac3) {
        if (!unsupportedAudioPassthroughLogged_) {
            unsupportedAudioPassthroughLogged_ = true;
            std::cerr << "NATIVE AUDIO TRANSCODE FALLBACK"
                      << " input=eac3"
                      << " requested=" << config_.audioCodec
                      << " fallback=copy"
                      << " reason=native_eac3_decoder_unavailable"
                      << std::endl;
        }
        error.clear();
        return emitCopy(std::move(sample), error);
    }

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
        } else if (config_.audioCodec == "mp3") {
            if (!mp3PtsValid_ && normalized.hasPts) {
                mp3NextPts90k_ = normalized.pts90k;
                mp3PtsValid_ = true;
            }
            const std::size_t frames = normalized.samples.size() / 2U;
            std::vector<std::uint8_t> bytes;
            if (!mp3Encoder_->encodeInterleaved(normalized.samples.data(), frames, bytes, error)) return false;
            if (!bytes.empty()) {
                const std::size_t frameBytes = mp3Encoder_->frameSizeBytes();
                if (frameBytes == 0 || bytes.size() % frameBytes != 0) {
                    error = "native MP3 encoder returned incomplete MPEG audio frames";
                    return false;
                }
                const std::uint64_t duration = (bytes.size() / frameBytes) * 2160ULL;
                codec::EncodedAudioFrame frame;
                frame.data = std::move(bytes);
                frame.hasPts = mp3PtsValid_;
                frame.pts90k = mp3NextPts90k_;
                if (!emitAudio(frame, duration, error)) return false;
                mp3NextPts90k_ += duration;
            }
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
        if (outputCodec == mpegts::ElementaryCodec::H264) {
            videoStartupH264Sps_ = videoStartupH264Sps_ || sets.h264Sps;
            videoStartupH264Pps_ = videoStartupH264Pps_ || sets.h264Pps;
        } else {
            videoStartupH265Vps_ = videoStartupH265Vps_ || sets.h265Vps;
            videoStartupH265Sps_ = videoStartupH265Sps_ || sets.h265Sps;
            videoStartupH265Pps_ = videoStartupH265Pps_ || sets.h265Pps;
        }
        const bool ready = outputCodec == mpegts::ElementaryCodec::H264
            ? (sets.h264Idr && videoStartupH264Sps_ && videoStartupH264Pps_)
            : (sets.h265Irap && videoStartupH265Vps_ &&
               videoStartupH265Sps_ && videoStartupH265Pps_);
        if (!ready) {
            ++videoStartupDropped_;
            if (videoStartupDropped_ <= 4U ||
                (videoStartupDropped_ % 100U) == 0U) {
                std::cerr << "NATIVE VIDEO STARTUP waiting"
                          << " codec=" << (outputCodec == mpegts::ElementaryCodec::H264 ? "h264" : "hevc")
                          << " dropped=" << videoStartupDropped_
                          << " key_flag=" << (frame.keyFrame ? 1 : 0)
                          << " h264_sps=" << (videoStartupH264Sps_ ? 1 : 0)
                          << " h264_pps=" << (videoStartupH264Pps_ ? 1 : 0)
                          << " h264_idr=" << (sets.h264Idr ? 1 : 0)
                          << " h265_vps=" << (videoStartupH265Vps_ ? 1 : 0)
                          << " h265_sps=" << (videoStartupH265Sps_ ? 1 : 0)
                          << " h265_pps=" << (videoStartupH265Pps_ ? 1 : 0)
                          << " h265_irap=" << (sets.h265Irap ? 1 : 0)
                          << std::endl;
            }
            return true;
        }
        videoStartupReady_ = true;
        std::cerr << "NATIVE VIDEO STARTUP ready"
                  << " codec=" << (outputCodec == mpegts::ElementaryCodec::H264 ? "h264" : "hevc")
                  << " dropped=" << videoStartupDropped_
                  << " key_flag=" << (frame.keyFrame ? 1 : 0)
                  << " nal_random_access=1"
                  << std::endl;
    }
    // V301: capture the exact H.264 access units accepted for TS muxing.
    // One file per pipeline instance, 64 MiB maximum. Disabled by default.
    if (outputCodec == mpegts::ElementaryCodec::H264) {
        std::lock_guard<std::mutex> diagLock(h264PremuxDiagnosticMutex_);
        if (!h264PremuxDiagnosticInitialized_) {
            h264PremuxDiagnosticInitialized_ = true;
            const char* enabled = std::getenv("DVBSTREAMER5_DIAG_H264_PREMUX");
            if (enabled && std::string(enabled) == "1") {
                const std::string path =
                    "/tmp/dvbstreamer5-h264-premux-" +
                    std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".h264";
                h264PremuxDiagnosticFile_.open(path, std::ios::binary | std::ios::trunc);
                if (h264PremuxDiagnosticFile_) {
                    std::cerr << "NATIVE H264 PREMUX DIAG path=" << path << std::endl;
                } else {
                    std::cerr << "NATIVE H264 PREMUX DIAG open failed path=" << path << std::endl;
                }
            }
        }
        constexpr std::uint64_t kDiagnosticLimit = 64ULL * 1024ULL * 1024ULL;
        if (h264PremuxDiagnosticFile_ && !frame.data.empty() &&
            h264PremuxDiagnosticBytes_ + frame.data.size() <= kDiagnosticLimit) {
            h264PremuxDiagnosticFile_.write(
                reinterpret_cast<const char*>(frame.data.data()),
                static_cast<std::streamsize>(frame.data.size()));
            h264PremuxDiagnosticFile_.flush();
            h264PremuxDiagnosticBytes_ += frame.data.size();
        }
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
    EncodedAudioObserver observer;
    {
        std::lock_guard<std::mutex> lock(encodedAudioObserverMutex_);
        observer = encodedAudioObserver_;
    }
    if (observer) {
        observer(audioCodecFromName(config_.audioCodec), frame, duration90k);
    }

    if (frame.hasPts)
        lastEncodedAudioPts90k_.store(frame.pts90k & ((1ULL << 33U) - 1ULL),
                                     std::memory_order_relaxed);
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

    // V10.8.101: emitAudio() used to be the only source for the ABR shared
    // audio callback.  Therefore a perfectly valid primary audio=copy path
    // (for example DVB MP2) never delivered any audio to lower renditions.
    // Fan out the already-demuxed compressed audio before moving its payload
    // into the primary mux queue.  This code is dormant when ABR is disabled.
    if (sample.stream.kind == mpegts::ElementaryKind::Audio) {
        EncodedAudioObserver observer;
        {
            std::lock_guard<std::mutex> lock(encodedAudioObserverMutex_);
            observer = encodedAudioObserver_;
        }
        if (observer) {
            codec::EncodedAudioFrame frame;
            frame.data = sample.data;
            frame.pts90k = sample.pts90k;
            frame.hasPts = sample.hasPts;
            observer(sample.stream.codec, frame, 1920);
        }
    }

    if (sample.stream.kind == mpegts::ElementaryKind::Audio && sample.hasPts)
        lastEncodedAudioPts90k_.store(sample.pts90k & ((1ULL << 33U) - 1ULL),
                                     std::memory_order_relaxed);
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
    // Observe actual caller-provided TS chunk cadence before the MPEG-TS
    // framer/demux runs. These counters do not change scheduling or payload.
    const auto tsInputNow = std::chrono::steady_clock::now();
    if (lastTsInputAt_.time_since_epoch().count() != 0) {
        const auto gapMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                tsInputNow - lastTsInputAt_).count());
        tsInputGapMaxMsWindow_ = std::max(tsInputGapMaxMsWindow_, gapMs);
        if (gapMs > 80) ++tsInputGapOver80MsWindow_;
        if (gapMs > 200) ++tsInputGapOver200MsWindow_;
    }
    lastTsInputAt_ = tsInputNow;
    ++tsInputCallsWindow_;
    tsInputBytesWindow_ += size;
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

    // flush() signals a finite, complete input. A video-only source cannot
    // satisfy the normal 2.5-second "wait for first audio" policy, while
    // waitForIdle() times out in approximately 1 second. The same issue can
    // occur on the final few frames of any finite A/V source.
    // Use an RAII guard so every early failure restores live-mode policy.
    struct MuxDrainGuard {
        std::atomic<bool>& requested;
        std::condition_variable& cv;
        ~MuxDrainGuard() {
            requested.store(false, std::memory_order_release);
            cv.notify_all();
        }
    } muxDrainGuard{muxDrainRequested_, muxQueueCv_};
    muxDrainRequested_.store(true, std::memory_order_release);
    muxQueueCv_.notify_all();

    demux_.flush();
    if (!waitForIdle()) {
        if (failed_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> failLock(failureMutex_); error = failure_;
        } else error = "native transcoder async queues did not drain";
        return false;
    }
    {
        std::lock_guard<std::mutex> codecLock(videoDecoderMutex_);
        if (nvidiaZeroCopy_) {
            std::vector<codec::EncodedVideoFrame> encoded;
            if (!nvidiaZeroCopy_->flush(encoded, error)) return false;
            for (const auto& frame : encoded)
                if (!emitVideo(frame, error)) return false;
        }
        if (intelZeroCopy_) {
            std::vector<codec::EncodedVideoFrame> encoded;
            if (!intelZeroCopy_->flush(encoded, error)) return false;
            for (const auto& frame : encoded)
                if (!emitVideo(frame, error)) return false;
        }
    }
    {
        std::lock_guard<std::mutex> codecLock(videoEncoderMutex_);
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
        if (mp3Encoder_) {
            std::vector<std::uint8_t> bytes;
            if (!mp3Encoder_->finish(bytes, error)) return false;
            if (!bytes.empty()) {
                const std::size_t frameBytes = mp3Encoder_->frameSizeBytes();
                if (frameBytes == 0 || bytes.size() % frameBytes != 0) {
                    error = "native MP3 flush returned incomplete audio frames";
                    return false;
                }
                const std::uint64_t duration = (bytes.size() / frameBytes) * 2160ULL;
                codec::EncodedAudioFrame frame; frame.data = std::move(bytes);
                frame.pts90k = mp3NextPts90k_;
                frame.hasPts = mp3PtsValid_;
                if (!emitAudio(frame, duration, error)) return false;
                mp3NextPts90k_ += duration;
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
    std::size_t vq = 0, rawVq = 0, aq = 0, mvq = 0, maq = 0;
    std::uint64_t vdrop = 0, rawVdrop = 0, primaryRawDrop = 0, adrop = 0;
    {
        std::lock_guard<std::mutex> qlock(videoQueueMutex_);
        vq = videoQueue_.size();
        rawVq = externalVideoQueue_.size();
        vdrop = droppedVideoSamples_;
        rawVdrop = droppedExternalVideoFrames_;
        primaryRawDrop = decodedVideoQueueDrops_;
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
           " raw_vq=" + std::to_string(rawVq) +
           " aq=" + std::to_string(aq) +
           " mux_vq=" + std::to_string(mvq) +
           " mux_aq=" + std::to_string(maq) +
           " av_delta_ms=" + std::to_string(avDeltaMs) +
           " late_video=" + std::to_string(muxLateVideo_.load(std::memory_order_acquire)) +
           " late_audio=" + std::to_string(muxLateAudio_.load(std::memory_order_acquire)) +
           " vdrop=" + std::to_string(vdrop) +
           " raw_vdrop=" + std::to_string(rawVdrop) +
           " primary_raw_drop=" + std::to_string(primaryRawDrop) +
           " fps_gate_drop=" + std::to_string(videoRateDroppedFrames_) +
           " nvidia_zero_copy=" + std::to_string(
               nvidiaZeroCopyActive_.load(std::memory_order_acquire) ? 1 : 0) +
           " intel_zero_copy=" + std::to_string(
               intelZeroCopyActive_.load(std::memory_order_acquire) ? 1 : 0) +
           " adrop=" + std::to_string(adrop);
}

} // namespace dvbstreamer5::media::transcode
