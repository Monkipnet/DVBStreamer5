#pragma once

#include <jsoncpp/json/json.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ConfigManager.h"
#include "media/NativePreviewHub.h"
#include "media/NativeUdpRelay.h"
#include "media/SharedDvbInputPool.h"
#include "media/NativeHlsInput.h"
#include "media/NativeHlsSegmenter.h"
#include "media/NativeCmaf.h"
#include "media/NativeRtspTransport.h"
#include "media/NativeRtmpTransport.h"
#include "media/NativeSrtTransport.h"
#include "media/NativeTranscoderPipeline.h"
#include "media/NativeTestPatternSource.h"
#include "TelegramNotifier.h"

class MptsOutputManager;

struct ActiveStreamSession {
    std::string streamId;
    std::string clientIp;
    std::string protocol;
    size_t connections = 0;
};

// Keep the real remote address for diagnostics/session management while carrying
// one bit of access-policy context that only the private admin-preview path may
// set. Ordinary HTTP/HLS/SRT/RTSP sessions always keep this flag false.
struct StreamClientIp : std::string {
    using std::string::string;
    using std::string::operator=;

    StreamClientIp() = default;
    StreamClientIp(const std::string& value) : std::string(value) {}
    StreamClientIp(std::string&& value) : std::string(std::move(value)) {}

    StreamClientIp& operator=(const std::string& value) {
        std::string::operator=(value);
        subscriberFilterExempt = false;
        return *this;
    }

    StreamClientIp& operator=(std::string&& value) {
        std::string::operator=(std::move(value));
        subscriberFilterExempt = false;
        return *this;
    }

    bool subscriberFilterExempt = false;
};

inline std::string operator+(const StreamClientIp& lhs, const char* rhs) {
    return static_cast<const std::string&>(lhs) + rhs;
}

// Browser preview must not keep a complete transcoder worker set alive while
// nobody is watching. StreamManager owns a stable wrapper so its relay callback
// never holds a dangling pointer, while the real native pipeline is initialized
// only by preview traffic. Five seconds without preview media stops all preview
// decoder/encoder/mux workers; a later preview transparently starts them again.
// Production transcoders are not routed through this wrapper.
class LazyPreviewTranscoder {
public:
    LazyPreviewTranscoder() = default;
    ~LazyPreviewTranscoder() { reset(); }

    LazyPreviewTranscoder(const LazyPreviewTranscoder&) = delete;
    LazyPreviewTranscoder& operator=(const LazyPreviewTranscoder&) = delete;

    bool initialize(
        const dvbstreamer5::media::transcode::NativeTranscoderConfig& config,
        std::string& error) {
        reset();
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        configured_ = true;
        error.clear();
        return true;
    }

    bool process(const std::uint8_t* data, std::size_t size,
                 std::vector<std::uint8_t>& output, std::string& error) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!configured_) {
            error = "browser preview transcoder is not configured";
            return false;
        }

        if (!active_) {
            if (monitor_.joinable()) {
                std::thread finished = std::move(monitor_);
                lock.unlock();
                finished.join();
                lock.lock();
            }

            if (!pipeline_.initialize(config_, error)) return false;
            active_ = true;
            stopMonitor_ = false;
            lastActivity_ = std::chrono::steady_clock::now();
            const std::uint64_t generation = ++generation_;
            monitor_ = std::thread([this, generation] {
                monitorLoop(generation);
            });
            std::cerr << "NATIVE BROWSER PREVIEW transcoder active"
                      << " idle_shutdown_s=5" << std::endl;
        } else {
            lastActivity_ = std::chrono::steady_clock::now();
        }
        activityCv_.notify_all();

        // Keep the wrapper lock while process() owns the native pipeline. The
        // idle monitor therefore cannot reset codecs/workers concurrently.
        return pipeline_.process(data, size, output, error);
    }

    bool flush(std::vector<std::uint8_t>& output, std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            output.clear();
            error.clear();
            return true;
        }
        return pipeline_.flush(output, error);
    }

    std::string status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) return "not configured";
        if (!active_) return "preview idle";
        return pipeline_.status();
    }

    void reset() {
        std::thread monitor;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopMonitor_ = true;
            ++generation_;
            activityCv_.notify_all();
            if (monitor_.joinable()) monitor = std::move(monitor_);
        }
        if (monitor.joinable()) monitor.join();

        std::lock_guard<std::mutex> lock(mutex_);
        if (active_) pipeline_.reset();
        active_ = false;
        configured_ = false;
        stopMonitor_ = false;
    }

private:
    void monitorLoop(std::uint64_t generation) {
        std::unique_lock<std::mutex> lock(mutex_);
        constexpr auto kIdle = std::chrono::seconds(5);
        for (;;) {
            if (stopMonitor_ || generation != generation_) return;
            const auto observedActivity = lastActivity_;
            activityCv_.wait_for(lock, kIdle, [&] {
                return stopMonitor_ || generation != generation_ ||
                       lastActivity_ != observedActivity;
            });
            if (stopMonitor_ || generation != generation_) return;
            if (lastActivity_ != observedActivity) continue;

            if (active_) {
                pipeline_.reset();
                active_ = false;
                std::cerr << "NATIVE BROWSER PREVIEW transcoder stopped"
                          << " reason=idle" << std::endl;
            }
            return;
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable activityCv_;
    dvbstreamer5::media::transcode::NativeTranscoderConfig config_;
    dvbstreamer5::media::transcode::NativeTranscoderPipeline pipeline_;
    std::thread monitor_;
    std::chrono::steady_clock::time_point lastActivity_{};
    bool configured_ = false;
    bool active_ = false;
    bool stopMonitor_ = false;
    std::uint64_t generation_ = 0;
};

// Adapter used so StreamManager.cpp can keep its unique_ptr-like syntax while
// the real preview pipeline remains lazy. V10.8.152 preserves the preview
// configuration before discarding the short-lived eager probe pipeline; older
// code created a fresh LazyPreviewTranscoder without initialize(), so the first
// fallback-transcode packet failed with "browser preview transcoder is not configured".
class LazyPreviewTranscoderHandle {
public:
    LazyPreviewTranscoderHandle() = default;
    LazyPreviewTranscoderHandle(const LazyPreviewTranscoderHandle&) = delete;
    LazyPreviewTranscoderHandle& operator=(const LazyPreviewTranscoderHandle&) = delete;

    LazyPreviewTranscoderHandle& operator=(
        std::unique_ptr<dvbstreamer5::media::transcode::NativeTranscoderPipeline>&& eager) {
        if (!eager) {
            value_.reset();
            return *this;
        }

        const auto geometry = eager->configuredOutputGeometry();
        dvbstreamer5::media::transcode::NativeTranscoderConfig config;
        config.videoCodec = "h264";
        config.videoEncoder = "cpu";
        config.audioCodec = "aac";
        config.width = geometry.first > 0 ? geometry.first : 1280;
        config.height = geometry.second > 0 ? geometry.second : 720;
        config.fps = 25.0;
        config.videoBitrate = 1800000ULL;
        config.audioBitrate = 128000ULL;
        config.deinterlace = true;
        config.lockOutputGeometry = true;
        config.muxBitrate = 0;
        config.serviceName = "DVBStreamer5 Preview";
        config.serviceProvider = "DVBStreamer5";

        eager.reset();
        auto lazy = std::make_unique<LazyPreviewTranscoder>();
        std::string ignored;
        (void)lazy->initialize(config, ignored);
        value_ = std::move(lazy);
        return *this;
    }

    LazyPreviewTranscoder* get() const noexcept { return value_.get(); }
    LazyPreviewTranscoder* operator->() const noexcept { return value_.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(value_); }
    void reset() noexcept { value_.reset(); }

private:
    std::unique_ptr<LazyPreviewTranscoder> value_;
};

struct StreamState {
    std::atomic<bool> active{false};
    std::atomic<bool> running{false};
    bool usingBackup = false;
    std::string statusMessage = "stopped";
    std::string activeInputUri;
    StreamConfig config;

    std::atomic<uint64_t> inputBitrate{0};
    std::atomic<uint64_t> outputBitrate{0};
    std::atomic<uint64_t> outputPayloadBitrate{0};
    std::atomic<uint64_t> inputBytes{0};
    std::atomic<uint64_t> outputBytes{0};
    std::atomic<uint64_t> inputCcErrors{0};
    std::atomic<uint64_t> inputCcErrorsDelta{0};
    std::atomic<uint64_t> outputCcErrors{0};
    std::atomic<uint64_t> outputCcErrorsDelta{0};
    // V10.8.70: cumulative counters sampled from the finished production TS.
    // /api/state consumes the per-second Delta fields below for the dashboard
    // CA indicator, so collect them after remap/CA/transcode, not from input TS.
    std::atomic<uint64_t> outputTsPayloadPackets{0};
    std::atomic<uint64_t> outputTsScrambledPackets{0};
    std::atomic<uint64_t> outputTsClearPesStarts{0};
    std::atomic<uint64_t> outputTsPayloadPacketsDelta{0};
    std::atomic<uint64_t> outputTsScrambledPacketsDelta{0};
    std::atomic<uint64_t> outputTsClearPesStartsDelta{0};

    std::unique_ptr<dvbstreamer5::media::network::NativeUdpRelay> nativeRelay;
    std::unique_ptr<dvbstreamer5::media::transcode::NativeTranscoderPipeline> nativeTranscoder;
    // Browser preview owns a lazy, preview-only transcoder. It is completely
    // separate from nativeTranscoder (the production path) and tears its worker
    // set down after the preview client stops delivering activity.
    LazyPreviewTranscoderHandle nativePreviewTranscoder;
    std::atomic<bool> previewTranscodeFailed{false};
    struct HlsAbrVariantRuntime {
        std::string name;
        int width = 0;
        int height = 0;
        std::uint64_t videoBitrate = 0;
        std::uint64_t muxBitrate = 0;
        bool enabled = true;
        bool failed = false;
        std::string lastError;
        std::unique_ptr<dvbstreamer5::media::transcode::NativeTranscoderPipeline> transcoder;
        std::unique_ptr<dvbstreamer5::media::hls::NativeHlsSegmenter> segmenter;
    };
    std::vector<std::unique_ptr<HlsAbrVariantRuntime>> hlsAbrVariants;
    mutable std::mutex hlsAbrMutex;
    bool hlsAbrSourceResolved = false;
    int hlsAbrPrimaryWidth = 0;
    int hlsAbrPrimaryHeight = 0;
    std::unique_ptr<dvbstreamer5::media::hls::NativeHlsInput> nativeHlsInput;
    std::unique_ptr<dvbstreamer5::media::hls::NativeHlsSegmenter> nativeHlsSegmenter;
    std::unique_ptr<dvbstreamer5::media::cmaf::NativeCmafSegmenter> nativeCmafSegmenter;
    std::unique_ptr<dvbstreamer5::media::rtsp::NativeRtspInput> nativeRtspInput;
    std::vector<std::unique_ptr<dvbstreamer5::media::rtsp::NativeRtspOutput>> nativeRtspOutputs;
    std::unique_ptr<dvbstreamer5::media::rtmp::NativeRtmpInput> nativeRtmpInput;
    std::vector<std::unique_ptr<dvbstreamer5::media::rtmp::NativeRtmpOutput>> nativeRtmpOutputs;
    std::unique_ptr<dvbstreamer5::media::srt::NativeSrtInput> nativeSrtInput;
    std::vector<std::unique_ptr<dvbstreamer5::media::srt::NativeSrtOutput>> nativeSrtOutputs;
    // Production HTTP MPEG-TS and private browser preview are deliberately separate.
    // HTTP clients receive post-remap/post-CA/post-transcode TS without waking preview codecs.
    std::shared_ptr<dvbstreamer5::media::network::NativePreviewHub> nativeHttpHub;
    std::shared_ptr<dvbstreamer5::media::network::NativePreviewHub> nativePreviewHub;
    std::atomic<bool> monitorStop{false};
    std::thread monitorThread;
    std::atomic<bool> testPatternStop{false};
    std::thread testPatternThread;
};

class StreamManager {
public:
    explicit StreamManager(ConfigManager& cfg, TelegramNotifier& notifier);
    ~StreamManager();

    bool startStream(const StreamConfig& streamConfig, std::string* error = nullptr);
    bool restartStream(const StreamConfig& streamConfig, std::string* error = nullptr);
    bool stopStream(const std::string& id);
    bool stopStreamAsync(const std::string& id);
    void stopAll();
    bool isStreamActive(const std::string& id);
    bool ensureOnDemandStream(const std::string& id, const std::string& source,
                              std::string* error = nullptr);

    // V10.8.77: the private browser-preview endpoint is a legitimate OnDemand
    // consumer.  HttpServer redirects its existing activity probes here so an
    // idle satellite service is tuned before the preview manifest/TS is tested.
    // Online streams keep the previous behavior because ensureOnDemandStream()
    // is a no-op for activation modes other than "ondemand".
    bool isStreamActiveForHttpPreview(const std::string& id) {
        std::string demandError;
        if (!ensureOnDemandStream(id, "preview", &demandError)) return false;
        return isStreamActive(id);
    }

    std::vector<std::string> activeStreams();
    std::map<std::string, StreamState*> snapshot();

    bool addHttpClient(const std::string& id, int fd, const std::string& clientIp,
                       const std::string& previewSession = {});
    bool closePreviewSession(const std::string& id, const std::string& previewSession);
    bool addStreamSession(const std::string& streamId, const std::string& clientIp,
                          const std::string& protocol);
    bool removeStreamSession(const std::string& streamId, const std::string& clientIp,
                             const std::string& protocol);
    size_t activeHttpSessions(const std::string& clientIp) const;
    size_t activeSubscriberSessions(const SubscriberConfig& subscriber);
    std::vector<ActiveStreamSession> activeStreamSessions();
    size_t resetHttpSessions(const std::string& clientIp);
    size_t enforceSubscriberAccess();
    size_t restartSrtOutputsForStreams(const std::vector<std::string>& streamIds);
    size_t restartAllSrtOutputs();

    void configureMptsOutputs();
    bool startMptsOutput(const std::string& id, std::string* error = nullptr);
    bool stopMptsOutput(const std::string& id);
    Json::Value mptsSnapshot() const;
    Json::Value queueMemorySnapshot() const;

private:
    void monitorNativeStream(StreamState* state);
    void monitorOnDemandStreams();
    bool isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const;
    bool isClientAllowedForStream(const std::string& streamId, const StreamClientIp& clientIp) const {
        if (clientIp.subscriberFilterExempt) return true;
        return isClientAllowedForStream(
            streamId, static_cast<const std::string&>(clientIp));
    }
    static std::string normalizedOutputType(const StreamConfig& cfg,
                                            const StreamOutputConfig* extra = nullptr);
    static bool isNativeInputSupported(const StreamConfig& cfg, std::string& reason);
    static bool isNativeOutputSupported(const std::string& type, std::string& reason);
    void pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::time_point now);

    ConfigManager& configManager;
    TelegramNotifier& telegramNotifier;
    std::unique_ptr<MptsOutputManager> mptsOutputManager;
    mutable std::mutex managerMutex;
    std::map<std::string, std::unique_ptr<StreamState>> streams;
    struct HttpClientSession {
        HttpClientSession() = default;
        HttpClientSession(std::string streamIdValue,
                          std::string clientIpValue,
                          std::string protocolValue,
                          std::chrono::steady_clock::time_point lastActivityValue,
                          int upstreamFdValue,
                          std::string previewSessionValue)
            : streamId(std::move(streamIdValue)),
              clientIp(std::move(clientIpValue)),
              protocol(std::move(protocolValue)),
              lastActivity(lastActivityValue),
              upstreamFd(upstreamFdValue),
              previewSession(std::move(previewSessionValue)) {
            // previewSession is minted only by the authenticated private
            // /api/streams/<id>/preview.ts endpoint. Subscriber filtering is a
            // delivery policy for stream clients, not an admin-panel ACL.
            clientIp.subscriberFilterExempt = !previewSession.empty();
        }

        std::string streamId;
        StreamClientIp clientIp;
        std::string protocol;
        std::chrono::steady_clock::time_point lastActivity;
        int upstreamFd = -1;
        std::string previewSession;
    };
    std::map<int, HttpClientSession> httpClients;
    std::map<std::string, HttpClientSession> adHocSessions;
    mutable std::mutex onDemandMutex;
    std::map<std::string, std::chrono::steady_clock::time_point> onDemandLastActivity;
    std::set<std::string> onDemandStartedStreams;
    std::atomic<bool> onDemandMonitorStop{false};
    std::thread onDemandMonitorThread;
    dvbstreamer5::media::SharedDvbInputPool sharedDvbInputs;
};