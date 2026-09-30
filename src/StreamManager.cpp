#include "StreamManager.h"

#include "CardManager.h"
#include "CaBackend.h"
#include "DvbSatellite.h"
#include "mpts/MptsOutputManager.h"
#include "utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr auto kAdHocSessionTtl = std::chrono::minutes(2);

bool writeAll(int fd, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

std::string cleanInterface(std::string value) {
    if (value == "auto" || value == "Auto" || value == "0.0.0.0") return {};
    return value;
}

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

} // namespace

StreamManager::StreamManager(ConfigManager& cfg, TelegramNotifier& notifier)
    : configManager(cfg), telegramNotifier(notifier),
      mptsOutputManager(std::make_unique<MptsOutputManager>()) {
    configureMptsOutputs();
}

StreamManager::~StreamManager() {
    stopAll();
}

std::string StreamManager::normalizedOutputType(const StreamConfig& cfg,
                                                const StreamOutputConfig* extra) {
    std::string type = toLower(extra ? extra->outputType : cfg.outputType);
    if (type == "udp_cbr" || type == "udpcbr") type = "udp-cbr";
    if (type == "udp_vbr" || type == "udpvbr") type = "udp-vbr";
    if (type == "udp") type = extra ? "udp-vbr" : (cfg.cbr ? "udp-cbr" : "udp-vbr");
    return type;
}

bool StreamManager::isNativeInputSupported(const StreamConfig& cfg, std::string& reason) {
    const std::string input = toLower(normalizeInputUri(cfg.inputUri));
    const std::string mode = toLower(cfg.inputMode);
    if (cfg.testPattern || mode == "test") {
        reason = "test-pattern generation is not yet implemented in the native media engine";
        return false;
    }
    if (startsWith(input, "udp://") || startsWith(input, "rtp://") ||
        startsWith(input, "http://") || startsWith(input, "https://") ||
        startsWith(input, "file://") || DvbSatellite::isDvbUri(cfg.inputUri) ||
        input.find("://") == std::string::npos) {
        if (mode == "hls" || input.find(".m3u8") != std::string::npos) {
            reason = "HLS input is being migrated to the native engine and is disabled in stage 3";
            return false;
        }
        return true;
    }
    if (startsWith(input, "srt://")) reason = "SRT input is not yet implemented in the native engine";
    else if (startsWith(input, "rtsp://")) reason = "RTSP input is not yet implemented in the native engine";
    else if (startsWith(input, "rtmp://") || startsWith(input, "rtmps://")) reason = "RTMP input is not yet implemented in the native engine";
    else reason = "unsupported native input protocol";
    return false;
}

bool StreamManager::isNativeOutputSupported(const std::string& type, std::string& reason) {
    if (type == "udp-cbr" || type == "udp-vbr" || type == "rtp" || type == "http") {
        return true;
    }
    if (type == "hls") reason = "HLS output segmenting is not yet implemented in the native media engine";
    else if (type == "srt") reason = "SRT output is not yet implemented in the native media engine";
    else if (type == "rtsp") reason = "RTSP output is not yet implemented in the native media engine";
    else if (type == "rtmp" || type == "youtube") reason = "RTMP/YouTube output is not yet implemented in the native media engine";
    else reason = "unsupported native output protocol";
    return false;
}

bool StreamManager::startStream(const StreamConfig& streamConfig, std::string* error) {
    if (error) error->clear();
    if (streamConfig.id.empty()) {
        if (error) *error = "stream id is empty";
        return false;
    }
    if (streamConfig.transcodeEnabled) {
        if (error) *error = "transcoding is disabled in native stage 3 until native video/audio codecs are integrated";
        return false;
    }
    std::string reason;
    if (!isNativeInputSupported(streamConfig, reason)) {
        if (error) *error = reason;
        return false;
    }

    std::vector<dvbstreamer5::media::network::NativeUdpRelayOutputConfig> nativeOutputs;
    bool hasHttpOutput = false;
    auto appendOutput = [&](const std::string& type, const std::string& host, int port,
                            const std::string& iface) -> bool {
        std::string outputReason;
        if (!isNativeOutputSupported(type, outputReason)) {
            if (error) *error = outputReason;
            return false;
        }
        if (type == "http") {
            hasHttpOutput = true;
            return true;
        }
        nativeOutputs.push_back({type, host, port, cleanInterface(iface)});
        return true;
    };

    if (!appendOutput(normalizedOutputType(streamConfig), streamConfig.outputHost,
                      streamConfig.outputPort, streamConfig.interfaceAddress)) {
        return false;
    }
    for (const auto& extra : streamConfig.additionalOutputs) {
        if (!appendOutput(normalizedOutputType(streamConfig, &extra), extra.outputHost,
                          extra.outputPort, extra.interfaceAddress)) {
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(managerMutex);
        if (streams.count(streamConfig.id)) {
            if (error) *error = "stream is already active";
            return false;
        }
    }

    std::string caError;
    if (!CardManager::instance().reserveService(streamConfig, &caError)) {
        if (error) *error = caError.empty() ? "CAM reservation failed" : caError;
        return false;
    }

    auto state = std::make_unique<StreamState>();
    state->config = streamConfig;
    state->activeInputUri = streamConfig.inputUri;
    state->nativePreviewHub = std::make_shared<dvbstreamer5::media::network::NativePreviewHub>();
    state->nativeRelay = std::make_unique<dvbstreamer5::media::network::NativeUdpRelay>();

    dvbstreamer5::media::network::NativeUdpRelayConfig relay;
    relay.inputUri = normalizeInputUri(streamConfig.inputUri);
    relay.outputs = nativeOutputs;
    relay.allowNoNetworkOutput = hasHttpOutput && nativeOutputs.empty();
    relay.inputInterfaceAddress = cleanInterface(streamConfig.inputInterfaceAddress);
    relay.inputInterfaceAddressConfigured = streamConfig.inputInterfaceAddressConfigured;
    relay.interfaceAddress = cleanInterface(streamConfig.interfaceAddress);
    relay.accessKeyMode = streamConfig.hlsAccessKeyMode;
    relay.accessKeyName = streamConfig.hlsAccessKeyName;
    relay.accessKeyValue = streamConfig.hlsAccessKeyValue;
    relay.userAgent = streamConfig.hlsUserAgent;
    relay.targetBitrate = streamConfig.targetBitrate;

    DvbSatelliteParams dvbParams;
    std::string dvbError;
    if (DvbSatellite::isDvbUri(streamConfig.inputUri)) {
        if (!DvbSatellite::parseUri(streamConfig.inputUri, dvbParams, dvbError)) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = dvbError.empty() ? "invalid DVB input" : dvbError;
            return false;
        }
        relay.dvbInputSource = true;
        relay.dvbTuneConfig.adapter = dvbParams.adapter;
        relay.dvbTuneConfig.frontend = dvbParams.frontend;
        relay.dvbTuneConfig.frequencyKHz = dvbParams.frequencyKHz;
        relay.dvbTuneConfig.symbolRateK = dvbParams.symbolRateK;
        relay.dvbTuneConfig.polarity = dvbParams.polarity;
        relay.dvbTuneConfig.deliverySystem = dvbParams.deliverySystem;
        relay.dvbTuneConfig.modulation = dvbParams.modulation;
        relay.dvbTuneConfig.fec = dvbParams.fec;
        relay.dvbTuneConfig.diseqcSource = dvbParams.diseqcSource;
        relay.dvbTuneConfig.lnbLof1KHz = dvbParams.lnbLof1KHz;
        relay.dvbTuneConfig.lnbLof2KHz = dvbParams.lnbLof2KHz;
        relay.dvbTuneConfig.lnbSlofKHz = dvbParams.lnbSlofKHz;
        relay.dvbTuneConfig.streamId = dvbParams.streamId;
        relay.dvbTuneConfig.pids = dvbParams.pids;
        if (streamConfig.inputServiceId > 0 && streamConfig.conditionalAccessClient.empty()) {
            std::string selectedPids;
            bool scrambled = false;
            if (DvbSatellite::resolveServicePids(dvbParams, streamConfig.inputServiceId,
                                                 selectedPids, scrambled, dvbError)) {
                relay.dvbTuneConfig.pids = selectedPids;
            }
        }
        if (!streamConfig.conditionalAccessClient.empty()) relay.dvbTuneConfig.pids = "8192";
    }

    relay.remapEnabled = streamConfig.remapEnabled ||
        (relay.dvbInputSource && streamConfig.inputServiceId > 0);
    if (relay.remapEnabled) {
        const uint32_t outSid = streamConfig.remapEnabled ? streamConfig.serviceId : streamConfig.inputServiceId;
        if (streamConfig.inputServiceId > 0xffff || outSid == 0 || outSid > 0xffff ||
            streamConfig.videoPid > 0xffff || streamConfig.audioPid > 0xffff) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = "service/PID value is outside MPEG-TS range";
            return false;
        }
        relay.remapConfig.inputServiceId = static_cast<uint16_t>(streamConfig.inputServiceId);
        relay.remapConfig.outputServiceId = static_cast<uint16_t>(outSid);
        relay.remapConfig.outputVideoPid = streamConfig.remapEnabled ? static_cast<uint16_t>(streamConfig.videoPid) : 0;
        relay.remapConfig.outputAudioPid = streamConfig.remapEnabled ? static_cast<uint16_t>(streamConfig.audioPid) : 0;
        relay.remapConfig.serviceName = streamConfig.serviceName.empty() ? streamConfig.name : streamConfig.serviceName;
        relay.remapConfig.serviceProvider = streamConfig.serviceProvider;
    }

    if (!streamConfig.conditionalAccessClient.empty()) {
        const std::string streamId = streamConfig.id;
        relay.processTransport = [streamId](uint8_t* data, std::size_t size) {
            return CaBackendManager::instance().processTransport(streamId, data, size);
        };
    }

    auto previewHub = state->nativePreviewHub;
    auto* mpts = mptsOutputManager.get();
    const std::string streamId = streamConfig.id;
    relay.observeTransport = [previewHub, mpts, streamId](const uint8_t* data, std::size_t size) {
        previewHub->publish(data, size);
        if (mpts) mpts->pushBytes(streamId, data, size);
    };

    std::string relayError;
    {
        auto guard = relay.dvbInputSource ? DvbSatellite::acquireFrontendTuneGuard(dvbParams)
                                         : std::unique_lock<std::mutex>();
        if (!state->nativeRelay->start(relay, relayError)) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = relayError.empty() ? "native relay failed" : relayError;
            return false;
        }
    }

    state->active.store(true);
    state->running.store(true);
    state->statusMessage = "running (native media engine)";
    StreamState* rawState = state.get();
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        streams.emplace(streamConfig.id, std::move(state));
    }
    CardManager::instance().activateService(streamConfig.id);
    try {
        rawState->monitorThread = std::thread(&StreamManager::monitorNativeStream, this, rawState);
    } catch (const std::exception& ex) {
        stopStream(streamConfig.id);
        if (error) *error = std::string("native monitor thread failed: ") + ex.what();
        return false;
    }
    return true;
}

void StreamManager::monitorNativeStream(StreamState* state) {
    uint64_t lastIn = 0, lastOut = 0, lastCc = 0;
    while (!state->monitorStop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (state->monitorStop.load()) break;
        auto* relay = state->nativeRelay.get();
        if (!relay) break;
        const uint64_t in = relay->inputBytes();
        const uint64_t out = relay->outputBytes();
        const uint64_t cc = relay->continuityErrors();
        state->inputBytes.store(in);
        state->outputBytes.store(out);
        state->inputBitrate.store((in - lastIn) * 8);
        const uint64_t outRate = (out - lastOut) * 8;
        state->outputBitrate.store(outRate ? outRate : state->inputBitrate.load());
        state->inputCcErrors.store(cc);
        state->inputCcErrorsDelta.store(cc - lastCc);
        lastIn = in;
        lastOut = out;
        lastCc = cc;
        if (!relay->isRunning()) {
            state->running.store(false);
            state->active.store(false);
            const std::string relayError = relay->lastError();
            state->statusMessage = relayError.empty() ? "native input stopped" : relayError;
            break;
        }
    }
}

bool StreamManager::restartStream(const StreamConfig& cfg, std::string* error) {
    stopStream(cfg.id);
    return startStream(cfg, error);
}

bool StreamManager::stopStream(const std::string& id) {
    std::unique_ptr<StreamState> state;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto it = streams.find(id);
        if (it == streams.end()) return false;
        state = std::move(it->second);
        streams.erase(it);
        for (const auto& [fd, session] : httpClients) {
            if (session.streamId == id) {
                ::shutdown(fd, SHUT_RDWR);
                if (session.upstreamFd >= 0) ::shutdown(session.upstreamFd, SHUT_RDWR);
            }
        }
    }
    state->monitorStop.store(true);
    if (state->nativePreviewHub) state->nativePreviewHub->close();
    if (state->nativeRelay) state->nativeRelay->stop();
    if (state->monitorThread.joinable()) state->monitorThread.join();
    state->running.store(false);
    state->active.store(false);
    CardManager::instance().releaseService(id);
    return true;
}

bool StreamManager::stopStreamAsync(const std::string& id) { return stopStream(id); }

void StreamManager::stopAll() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        for (const auto& item : streams) ids.push_back(item.first);
    }
    for (const auto& id : ids) stopStream(id);
    if (mptsOutputManager) mptsOutputManager->stopAll();
    CardManager::instance().releaseAll();
}

bool StreamManager::isStreamActive(const std::string& id) {
    std::lock_guard<std::mutex> lock(managerMutex);
    const auto it = streams.find(id);
    return it != streams.end() && it->second->active.load();
}

std::vector<std::string> StreamManager::activeStreams() {
    std::vector<std::string> result;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [id, state] : streams) if (state->active.load()) result.push_back(id);
    return result;
}

std::map<std::string, StreamState*> StreamManager::snapshot() {
    std::map<std::string, StreamState*> result;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (auto& [id, state] : streams) result[id] = state.get();
    return result;
}

bool StreamManager::addHttpClient(const std::string& id, int fd, const std::string& clientIp,
                                  const std::string& previewSession) {
    std::shared_ptr<dvbstreamer5::media::network::NativePreviewHub> hub;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto it = streams.find(id);
        if (it == streams.end() || !it->second->active.load() || !it->second->nativePreviewHub) {
            ::close(fd);
            return false;
        }
        hub = it->second->nativePreviewHub;
    }
    std::string subscribeError;
    const int upstreamFd = hub->subscribe(subscribeError);
    if (upstreamFd < 0) {
        ::close(fd);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        httpClients[fd] = {id, normalizeIpAddress(clientIp), "mpegts",
                           std::chrono::steady_clock::now(), upstreamFd, previewSession};
    }
    try {
        std::thread([this, hub, fd, upstreamFd]() {
            std::array<char, 64 * 1024> buffer{};
            for (;;) {
                const ssize_t n = ::read(upstreamFd, buffer.data(), buffer.size());
                if (n > 0) {
                    if (!writeAll(fd, buffer.data(), static_cast<std::size_t>(n))) break;
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            std::lock_guard<std::mutex> lock(managerMutex);
            httpClients.erase(fd);
            hub->unsubscribe(upstreamFd);
            ::close(upstreamFd);
            ::close(fd);
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(managerMutex);
        httpClients.erase(fd);
        hub->unsubscribe(upstreamFd);
        ::close(upstreamFd);
        ::close(fd);
        return false;
    }
    return true;
}

bool StreamManager::closePreviewSession(const std::string& id, const std::string& previewSession) {
    bool closed = false;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [fd, session] : httpClients) {
        if (session.streamId == id && session.previewSession == previewSession) {
            ::shutdown(fd, SHUT_RDWR);
            if (session.upstreamFd >= 0) ::shutdown(session.upstreamFd, SHUT_RDWR);
            closed = true;
        }
    }
    return closed;
}

bool StreamManager::addStreamSession(const std::string& streamId, const std::string& clientIp,
                                     const std::string& protocol) {
    if (streamId.empty() || clientIp.empty()) return false;
    std::lock_guard<std::mutex> lock(managerMutex);
    const std::string ip = normalizeIpAddress(clientIp);
    const std::string key = protocol + ":" + streamId + ":" + ip + ":" +
        std::to_string(nextSessionId.fetch_add(1));
    adHocSessions[key] = {streamId, ip, protocol, std::chrono::steady_clock::now(), -1, {}};
    return true;
}

bool StreamManager::removeStreamSession(const std::string& streamId, const std::string& clientIp,
                                        const std::string& protocol) {
    const std::string ip = normalizeIpAddress(clientIp);
    bool removed = false;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        if (it->second.streamId == streamId && it->second.clientIp == ip && it->second.protocol == protocol) {
            it = adHocSessions.erase(it);
            removed = true;
        } else ++it;
    }
    return removed;
}

void StreamManager::pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        if (now - it->second.lastActivity > kAdHocSessionTtl) it = adHocSessions.erase(it);
        else ++it;
    }
}

size_t StreamManager::activeHttpSessions(const std::string& clientIp) const {
    const std::string ip = normalizeIpAddress(clientIp);
    size_t count = 0;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [fd, session] : httpClients) { (void)fd; if (session.clientIp == ip) ++count; }
    for (const auto& [key, session] : adHocSessions) { (void)key; if (session.clientIp == ip) ++count; }
    return count;
}

size_t StreamManager::activeSubscriberSessions(const SubscriberConfig& subscriber) {
    if (!subscriber.enabled) return 0;
    const std::string primary = normalizeIpAddress(subscriber.primaryIp);
    const std::string backup = normalizeIpAddress(subscriber.backupIp);
    auto matches = [&](const HttpClientSession& session) {
        const bool ip = session.clientIp == primary || (!backup.empty() && session.clientIp == backup);
        const bool stream = std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), session.streamId) != subscriber.streamIds.end();
        return ip && stream;
    };
    size_t count = 0;
    std::lock_guard<std::mutex> lock(managerMutex);
    pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::now());
    for (const auto& [fd, session] : httpClients) { (void)fd; if (matches(session)) ++count; }
    for (const auto& [key, session] : adHocSessions) { (void)key; if (matches(session)) ++count; }
    return count;
}

std::vector<ActiveStreamSession> StreamManager::activeStreamSessions() {
    std::map<std::string, ActiveStreamSession> grouped;
    std::lock_guard<std::mutex> lock(managerMutex);
    pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::now());
    auto add = [&](const HttpClientSession& session) {
        const std::string key = session.clientIp + "\n" + session.streamId + "\n" + session.protocol;
        auto& out = grouped[key];
        out.streamId = session.streamId;
        out.clientIp = session.clientIp;
        out.protocol = session.protocol;
        ++out.connections;
    };
    for (const auto& [fd, session] : httpClients) { (void)fd; add(session); }
    for (const auto& [key, session] : adHocSessions) { (void)key; add(session); }
    std::vector<ActiveStreamSession> result;
    for (auto& [key, session] : grouped) { (void)key; result.push_back(std::move(session)); }
    return result;
}

size_t StreamManager::resetHttpSessions(const std::string& clientIp) {
    const std::string ip = normalizeIpAddress(clientIp);
    size_t count = 0;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [fd, session] : httpClients) {
        if (session.clientIp == ip) {
            ::shutdown(fd, SHUT_RDWR);
            if (session.upstreamFd >= 0) ::shutdown(session.upstreamFd, SHUT_RDWR);
            ++count;
        }
    }
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        if (it->second.clientIp == ip) { it = adHocSessions.erase(it); ++count; }
        else ++it;
    }
    return count;
}

bool StreamManager::isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const {
    if (!configManager.subscribers.filteringEnabled) return true;
    const std::string ip = normalizeIpAddress(clientIp);
    if (std::find(configManager.subscribers.blockedIps.begin(), configManager.subscribers.blockedIps.end(), ip) != configManager.subscribers.blockedIps.end()) return false;
    for (const auto& subscriber : configManager.subscribers.subscribers) {
        if (!subscriber.enabled) continue;
        const bool ipMatch = normalizeIpAddress(subscriber.primaryIp) == ip ||
            (!subscriber.backupIp.empty() && normalizeIpAddress(subscriber.backupIp) == ip);
        const bool streamMatch = std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), streamId) != subscriber.streamIds.end();
        if (ipMatch && streamMatch) return true;
    }
    return false;
}

size_t StreamManager::enforceSubscriberAccess() {
    size_t count = 0;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [fd, session] : httpClients) {
        if (!isClientAllowedForStream(session.streamId, session.clientIp)) {
            ::shutdown(fd, SHUT_RDWR);
            if (session.upstreamFd >= 0) ::shutdown(session.upstreamFd, SHUT_RDWR);
            ++count;
        }
    }
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        if (!isClientAllowedForStream(it->second.streamId, it->second.clientIp)) {
            it = adHocSessions.erase(it); ++count;
        } else ++it;
    }
    return count;
}

size_t StreamManager::restartSrtOutputsForStreams(const std::vector<std::string>&) { return 0; }
size_t StreamManager::restartAllSrtOutputs() { return 0; }

void StreamManager::configureMptsOutputs() {
    if (mptsOutputManager) mptsOutputManager->configure(configManager.config.mptsOutputs, configManager.config.streams);
}

bool StreamManager::startMptsOutput(const std::string& id, std::string* error) {
    return mptsOutputManager && mptsOutputManager->start(id, error);
}

bool StreamManager::stopMptsOutput(const std::string& id) {
    return mptsOutputManager && mptsOutputManager->stop(id);
}

Json::Value StreamManager::mptsSnapshot() const {
    return mptsOutputManager ? mptsOutputManager->snapshot() : Json::Value(Json::arrayValue);
}

Json::Value StreamManager::queueMemorySnapshot() const {
    Json::Value root;
    root["engine"] = "native";
    root["stream_count"] = Json::UInt64(streams.size());
    return root;
}
