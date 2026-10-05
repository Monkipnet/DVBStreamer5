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

// V10.8.53: browser preview must not keep a complete transcoder worker set
// alive while nobody is watching. StreamManager still owns a stable object so
// its relay callback never holds a dangling pointer, but the real native
// pipeline is initialized only by the first preview transport packet. When no
// packet reaches the preview path for five seconds (longer than the normal
// ~3-second HLS source segment cadence), all decoder/encoder/mux workers are
// stopped. A later preview transparently initializes the saved configuration
// again. Production transcoders are not routed through this wrapper.
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

// Adapter used so existing StreamManager.cpp code can keep its unique_ptr-like
// syntax. Its assignment intentionally discards the eagerly allocated base
// pipeline and replaces it with the lazy preview-only implementation above.
class LazyPreviewTranscoderHandle {
public:
    LazyPreviewTranscoderHandle() = default;
    LazyPreviewTranscoderHandle(const LazyPreviewTranscoderHandle&) = delete;
    LazyPreviewTranscoderHandle& operator=(const LazyPreviewTranscoderHandle&) = delete;

    LazyPreviewTranscoderHandle& operator=(
        std::unique_ptr<dvbstreamer5::media::transcode::NativeTranscoderPipeline>&& eager) {
        eager.reset();
        value_ = std::make_unique<LazyPreviewTranscoder>();
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
    // V10.8.69: browser preview uses the direct V10.8.41 pipeline again.
    // It stays initialized for the stream lifetime and is isolated from the
    // production transcoder. Output is fixed H.264/AAC 1280x720 square-pixel 16:9.
    std::unique_ptr<dvbstreamer5::media::transcode::NativeTranscoderPipeline> nativePreviewTranscoder;
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
    // V10.8.77: monitorOnDemandStreams() historically used
    // previewSession.empty() as a proxy for a persistent HTTP viewer and thus
    // ignored private browser-preview sockets.  The token itself is still kept
    // for /preview/close matching, while empty() intentionally reports true so
    // both production HTTP and browser preview sockets keep OnDemand alive.
    struct PreviewSessionToken {
        std::string value;

        PreviewSessionToken() = default;
        PreviewSessionToken(const std::string& token) : value(token) {}
        PreviewSessionToken& operator=(const std::string& token) {
            value = token;
            return *this;
        }
        void clear() noexcept { value.clear(); }
        bool empty() const noexcept { return true; }
        bool operator==(const std::string& token) const noexcept { return value == token; }
    };

    struct HttpClientSession {
        std::string streamId;
        std::string clientIp;
        std::string protocol;
        std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
        int upstreamFd = -1;
        PreviewSessionToken previewSession;
    };

    // V10.8.54 build wrapper keeps the previous implementation available under
    // this private name while addHttpClient() cleanly separates public HTTP TS
    // subscribers from browser-preview subscribers.
    bool addHttpClientOriginal(const std::string& id, int fd, const std::string& clientIp,
                               const std::string& previewSession);

    bool isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const;
    void pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::time_point now);
    static std::string normalizedOutputType(const StreamConfig& cfg, const StreamOutputConfig* extra = nullptr);
    static bool isNativeInputSupported(const StreamConfig& cfg, std::string& reason);
    static bool isNativeOutputSupported(const std::string& type, std::string& reason);
    void monitorNativeStream(StreamState* state);
    void monitorOnDemandStreams();

    ConfigManager& configManager;
    TelegramNotifier& telegramNotifier;
    std::map<std::string, std::unique_ptr<StreamState>> streams;
    std::unique_ptr<MptsOutputManager> mptsOutputManager;
    dvbstreamer5::media::network::SharedDvbInputPool sharedDvbInputs;
    std::map<int, HttpClientSession> httpClients;
    std::map<std::string, HttpClientSession> adHocSessions;
    mutable std::mutex managerMutex;
    std::mutex onDemandMutex;
    std::set<std::string> onDemandStartedStreams;
    std::map<std::string, std::chrono::steady_clock::time_point> onDemandLastActivity;
    std::atomic<bool> onDemandMonitorStop{false};
    std::thread onDemandMonitorThread;
    std::atomic<uint64_t> nextSessionId{0};
};