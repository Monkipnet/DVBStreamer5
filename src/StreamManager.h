#pragma once

#include <jsoncpp/json/json.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ConfigManager.h"
#include "media/NativePreviewHub.h"
#include "media/NativeUdpRelay.h"
#include "media/NativeHlsInput.h"
#include "media/NativeHlsSegmenter.h"
#include "TelegramNotifier.h"

class MptsOutputManager;

struct ActiveStreamSession {
    std::string streamId;
    std::string clientIp;
    std::string protocol;
    size_t connections = 0;
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
    std::atomic<uint64_t> inputBytes{0};
    std::atomic<uint64_t> outputBytes{0};
    std::atomic<uint64_t> inputCcErrors{0};
    std::atomic<uint64_t> inputCcErrorsDelta{0};
    std::atomic<uint64_t> outputCcErrors{0};
    std::atomic<uint64_t> outputCcErrorsDelta{0};
    std::atomic<uint64_t> outputTsPayloadPacketsDelta{0};
    std::atomic<uint64_t> outputTsScrambledPacketsDelta{0};
    std::atomic<uint64_t> outputTsClearPesStartsDelta{0};

    std::unique_ptr<dvbstreamer5::media::network::NativeUdpRelay> nativeRelay;
    std::unique_ptr<dvbstreamer5::media::hls::NativeHlsInput> nativeHlsInput;
    std::unique_ptr<dvbstreamer5::media::hls::NativeHlsSegmenter> nativeHlsSegmenter;
    std::shared_ptr<dvbstreamer5::media::network::NativePreviewHub> nativePreviewHub;
    std::atomic<bool> monitorStop{false};
    std::thread monitorThread;
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
    struct HttpClientSession {
        std::string streamId;
        std::string clientIp;
        std::string protocol;
        std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
        int upstreamFd = -1;
        std::string previewSession;
    };

    bool isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const;
    void pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::time_point now);
    static std::string normalizedOutputType(const StreamConfig& cfg, const StreamOutputConfig* extra = nullptr);
    static bool isNativeInputSupported(const StreamConfig& cfg, std::string& reason);
    static bool isNativeOutputSupported(const std::string& type, std::string& reason);
    void monitorNativeStream(StreamState* state);

    ConfigManager& configManager;
    TelegramNotifier& telegramNotifier;
    std::map<std::string, std::unique_ptr<StreamState>> streams;
    std::unique_ptr<MptsOutputManager> mptsOutputManager;
    std::map<int, HttpClientSession> httpClients;
    std::map<std::string, HttpClientSession> adHocSessions;
    mutable std::mutex managerMutex;
    std::atomic<uint64_t> nextSessionId{0};
};
