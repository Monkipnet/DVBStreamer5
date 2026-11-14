#include "media/NativeUdpRelay.h"

#include "NativeHttpClient.h"
#include "media/CbrTsPacer.h"
#include "media/IpCbrSourceClockPolicy.h"
#include "media/WisiCbrOutputWorker.h"
#include "media/PcrVbrUdpOutputWorker.h"
#include "media/RtpMpegTs.h"
#include "media/TransportStream.h"
#include "media/NativeFileLoopClock.h"
#include "media/NativeMp4Replacement.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <regex>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

// Native transport relay: acquire UDP/RTP/HTTP/file/DVB/external input, apply the
// configured remap/CA/transcode callbacks once, then fan out the processed MPEG-TS to
// UDP/RTP sockets and observers used by SRT/HTTP/HLS/RTSP/RTMP.

namespace dvbstreamer5::media::network {
namespace {

struct UdpInputEndpoint {
    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
};

bool startsWithInsensitive(const std::string& value, const char* prefix) {
    const std::size_t prefixSize = std::char_traits<char>::length(prefix);
    return value.size() >= prefixSize &&
        std::equal(prefix, prefix + prefixSize, value.begin(),
            [](unsigned char left, unsigned char right) {
                return std::tolower(left) == std::tolower(right);
            });
}

bool resolveFileInputPath(const std::string& uri, std::string& path) {
    path = uri;
    if (startsWithInsensitive(path, "file://")) {
        path.erase(0, 7);
        if (startsWithInsensitive(path, "localhost/")) {
            path.erase(0, 9);
        }
    } else if (path.find("://") != std::string::npos) {
        return false;
    }
    return !path.empty();
}

bool isHttpInput(const std::string& uri) {
    return startsWithInsensitive(uri, "http://") ||
        startsWithInsensitive(uri, "https://");
}

bool parseInputEndpoint(const std::string& uri, UdpInputEndpoint& endpoint) {
    static const std::regex pattern(
        R"(^(udp|rtp)://@?([^:/]*):([0-9]+)(?:[/?#].*)?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(uri, match, pattern)) {
        return false;
    }

    int port = 0;
    try {
        port = std::stoi(match[3].str());
    } catch (const std::exception&) {
        return false;
    }
    if (port <= 0 || port > 65535) {
        return false;
    }

    endpoint.scheme = match[1].str();
    std::transform(endpoint.scheme.begin(), endpoint.scheme.end(), endpoint.scheme.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    endpoint.host = match[2].str();
    endpoint.port = static_cast<std::uint16_t>(port);
    return true;
}

bool isMulticastIpv4(const std::string& address) {
    std::size_t dot = address.find('.');
    if (dot == std::string::npos) return false;
    try {
        const int firstOctet = std::stoi(address.substr(0, dot));
        return firstOctet >= 224 && firstOctet <= 239;
    } catch (const std::exception&) {
        return false;
    }
}

std::uint32_t currentRtpTimestamp() {
    const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 11111;
    return static_cast<std::uint32_t>(ticks);
}

bool parseOutputEndpoint(
    const std::string& configuredHost,
    int configuredPort,
    std::string& host,
    std::uint16_t& port) {
    static const std::regex uriPattern(
        R"(^(?:udp://)?@?([^:/]+):([0-9]+)(?:[/?#].*)?$)",
        std::regex::icase);
    std::smatch match;
    if (std::regex_match(configuredHost, match, uriPattern)) {
        int parsedPort = 0;
        try {
            parsedPort = std::stoi(match[2].str());
        } catch (const std::exception&) {
            return false;
        }
        if (parsedPort <= 0 || parsedPort > 65535) return false;
        host = match[1].str();
        port = static_cast<std::uint16_t>(parsedPort);
        return true;
    }

    if (configuredPort <= 0 || configuredPort > 65535 || configuredHost.empty()) {
        return false;
    }
    host = configuredHost;
    port = static_cast<std::uint16_t>(configuredPort);
    return true;
}

bool flushPackets(
    const std::vector<dvbstreamer5::media::mpegts::Packet>& packets,
    bool rtpOutput,
    dvbstreamer5::media::rtp::MpegTsPacketizer* packetizer,
    UdpSocket& output,
    std::atomic<std::uint64_t>& outputBytes,
    std::string& error) {
    if (packets.empty()) return true;

    if (rtpOutput) {
        if (!packetizer) {
            error = "RTP output packetizer is not initialized";
            return false;
        }
        std::vector<std::vector<std::uint8_t>> datagrams;
        if (!packetizer->packetize(packets, currentRtpTimestamp(), datagrams)) {
            error = "failed to packetize MPEG-TS RTP output";
            return false;
        }
        for (const auto& datagram : datagrams) {
            if (!output.send(datagram.data(), datagram.size(), error)) return false;
        }
    } else {
        // Seven 188-byte MPEG-TS packets per UDP datagram (1316-byte payload)
        // is the conventional MPEG-over-UDP layout and matches WISI IPTV
        // equipment that exposes a TS-packets-per-datagram setting.
        constexpr std::size_t kPacketsPerDatagram = 7;
        for (std::size_t first = 0; first < packets.size(); first += kPacketsPerDatagram) {
            const std::size_t count =
                (std::min)(kPacketsPerDatagram, packets.size() - first);
            std::array<std::uint8_t,
                kPacketsPerDatagram * dvbstreamer5::media::mpegts::kPacketSize> bytes {};
            for (std::size_t index = 0; index < count; ++index) {
                std::copy(
                    packets[first + index].begin(),
                    packets[first + index].end(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(
                        index * dvbstreamer5::media::mpegts::kPacketSize));
            }
            const std::size_t size =
                count * dvbstreamer5::media::mpegts::kPacketSize;
            if (!output.send(bytes.data(), size, error)) return false;
        }
    }

    outputBytes.fetch_add(
        packets.size() * dvbstreamer5::media::mpegts::kPacketSize, std::memory_order_relaxed);
    return true;
}

std::string inputInterfaceFor(const NativeUdpRelayConfig& config, bool multicastOrWildcard) {
    if (config.inputInterfaceAddressConfigured) {
        return config.inputInterfaceAddress;
    }
    return multicastOrWildcard ? config.interfaceAddress : std::string();
}

} // namespace

// V345: the constructor must be compiled here where NativeMp4Replacement is
// complete; an inline defaulted constructor triggers unique_ptr's sizeof(T)
// static assertion in StreamManager.cpp with GCC 13.
NativeUdpRelay::NativeUdpRelay() = default;

NativeUdpRelay::~NativeUdpRelay() {
    stop();
}

bool NativeUdpRelay::start(const NativeUdpRelayConfig& config, std::string& error) {
    stop();
    config_ = config;
    inputBackpressureEvents_.store(0, std::memory_order_relaxed);
    httpJitterEnabled_ = config_.httpTsInputJitterEnabled &&
                         !config_.externallyFedInput &&
                         isHttpInput(config_.inputUri);
    if (httpJitterEnabled_) {
        std::cerr << "NATIVE HTTP JITTER enabled"
                  << " stream_id=" << config_.streamId
                  << " prebuffer_ms=1200"
                  << " queue_limit_kib=2048"
                  << " pacing_chunk_bytes=8192"
                  << " media_pts=preserved"
                  << " fallback=disabled_other_streams"
                  << std::endl;
    }
    if (config_.outputs.empty() && !config_.allowNoNetworkOutput) {
        config_.outputs.push_back({
            config.outputType,
            config.outputHost,
            config.outputPort,
            config.interfaceAddress});
    }
    // V386: one PCR timeline for VBR physical UDP and the shared
    // SRT/HTTP/HLS/RTSP/RTMP observer. Do not first pace the HTTP input by
    // its averaged byte rate: the two independent clocks accumulate jitter.
    const bool hasCbrPhysical=std::any_of(config_.outputs.begin(),config_.outputs.end(),
        [](const auto& out){return out.outputType=="udp-cbr";});
    const bool vbrSourcePacing=!config_.paceObservedTransport && !hasCbrPhysical &&
        (config_.observeTransport || std::any_of(
            config_.outputs.begin(),config_.outputs.end(),
            [](const auto& out){return out.outputType=="udp-vbr";}));
    if(vbrSourcePacing && httpJitterEnabled_){
        httpJitterEnabled_=false;
        std::cerr<<"NATIVE HTTP JITTER disabled stream_id="<<config_.streamId
                 <<" reason=vbr_pcr_shared_clock"<<std::endl;
    }
    fileInput_.close();
    fileInput_.clear();
    mp4Input_.reset();
    fileInputSource_ = false;
    httpInputSource_ = false;
    externalInputSource_ = false;
    dvbInputSource_ = false;
    {
        std::lock_guard<std::mutex> lock(httpQueueMutex_);
        httpQueue_.clear();
        httpQueueBufferPool_.clear();
        httpQueuedBytes_ = 0;
        httpQueueProducerWaiting_ = false;
        httpFinished_ = false;
    }
    inputBytes_.store(0, std::memory_order_relaxed);
    httpReceivedBytes_.store(0, std::memory_order_relaxed);
    selectedInputBytes_.store(0, std::memory_order_relaxed);
    outputBytes_.store(0, std::memory_order_relaxed);
    perOutputTransportBytes_.clear();
    perOutputTransportBytes_.reserve(config_.outputs.size());
    for (std::size_t i = 0; i < config_.outputs.size(); ++i) {
        perOutputTransportBytes_.push_back(
            std::make_shared<std::atomic<std::uint64_t>>(0ULL));
    }
    payloadOutputBytes_.store(0, std::memory_order_relaxed);
    continuityErrors_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_.clear();
    }

    UdpInputEndpoint input;
    const bool networkInput = parseInputEndpoint(config.inputUri, input);
    httpInputSource_ = !networkInput && isHttpInput(config.inputUri);
    externalInputSource_ = config.externallyFedInput;
    dvbInputSource_ = config.dvbInputSource;
    std::string filePath;
    fileInputSource_ = !networkInput && !httpInputSource_ && !externalInputSource_ &&
        resolveFileInputPath(config.inputUri, filePath);
    if (!networkInput && !httpInputSource_ && !externalInputSource_ &&
        !fileInputSource_ && !dvbInputSource_) {
        error = "native relay requires a valid UDP/RTP, HTTP(S), externally-fed, DVB, or local file input";
        return false;
    }
    // MP4 is ISO-BMFF, not MPEG-TS. Remux only local MP4/MOV files;
    // all network inputs keep the existing transport and media pipelines.
    bool mp4Replacement = false;
    if (fileInputSource_) {
        auto lower = filePath;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        mp4Replacement = (lower.size() >= 4 &&
            (lower.compare(lower.size()-4,4,".mp4") == 0 ||
             lower.compare(lower.size()-4,4,".m4v") == 0 ||
             lower.compare(lower.size()-4,4,".mov") == 0));
    }
    if (mp4Replacement && config_.remapEnabled) {
        // A primary MPEG-TS audio PID is unrelated to an MP4 media track.
        config_.remapConfig.inputAudioPid = 0;
    }
    if (config_.remapEnabled &&
        !remapper_.initialize(config_.remapConfig, error)) {
        return false;
    }
    for (const auto& output : config_.outputs) {
        if (output.outputType != "udp-vbr" && output.outputType != "udp-cbr" &&
            output.outputType != "rtp") {
            error = "native UDP relay supports only UDP VBR, UDP CBR, and RTP outputs";
            return false;
        }
        if (output.outputType == "udp-cbr" && config.targetBitrate == 0) {
            error = "native UDP CBR target bitrate must be greater than zero";
            return false;
        }
    }

    if (fileInputSource_) {
        if (mp4Replacement) {
            mp4Input_ = std::make_unique<dvbstreamer5::media::mp4::NativeMp4Replacement>();
            const auto service = config_.remapConfig.inputServiceId != 0
                ? config_.remapConfig.inputServiceId : std::uint16_t{1};
            if (!mp4Input_->open(filePath, config_.fileInputLoop, service,
                config_.remapConfig.serviceName, config_.remapConfig.serviceProvider,
                error)) {
                mp4Input_.reset();
                fileInputSource_ = false;
                return false;
            }
            std::cerr << "NATIVE BACKUP MP4 REMUX stream_id=" << config_.streamId
                      << " video_samples=" << mp4Input_->videoSamples()
                      << " audio_samples=" << mp4Input_->audioSamples()
                      << " mode=remux_no_ffmpeg"
                      << " loop=" << (config_.fileInputLoop ? 1 : 0)
                      << std::endl;
        } else {
            fileInput_.open(filePath, std::ios::binary);
            if (!fileInput_.is_open()) {
                fileInputSource_ = false;
                error = "native file input could not be opened: " + filePath;
                return false;
            }
        }
    } else if (!httpInputSource_ && !externalInputSource_ && !dvbInputSource_) {
        const bool multicastInput = isMulticastIpv4(input.host);
        const bool wildcardInput = input.host.empty() || input.host == "0.0.0.0";
        const std::string inputInterface =
            inputInterfaceFor(config, multicastInput || wildcardInput);
        if (!inputSocket_.openReceiver(
                "0.0.0.0",
                input.port,
                multicastInput ? input.host : std::string(),
                inputInterface,
                16 * 1024 * 1024,
                error,
                config_.inputInterfaceDeviceName)) {
            error = "native UDP input setup failed: " + error;
            return false;
        }
    }
    if (dvbInputSource_ && !dvbInput_.open(config_.dvbTuneConfig, error)) {
        error = "native DVB input setup failed: " + error;
        return false;
    }

    outputSockets_.clear();
    for (const auto& output : config_.outputs) {
        std::string outputHost;
        std::uint16_t parsedOutputPort = 0;
        if (!parseOutputEndpoint(
                output.outputHost, output.outputPort, outputHost, parsedOutputPort)) {
            inputSocket_.close();
            dvbInput_.close();
            fileInput_.close();
            outputSockets_.clear();
            error = "native UDP relay output endpoint is invalid";
            return false;
        }
        auto outputSocket = std::make_unique<UdpSocket>();
        if (!outputSocket->openSender(
                outputHost, parsedOutputPort, output.interfaceAddress, error)) {
            inputSocket_.close();
            dvbInput_.close();
            fileInput_.close();
            outputSockets_.clear();
            error = "native UDP output setup failed: " + error;
            return false;
        }
        std::cerr << "NATIVE UDP OUTPUT open index=" << outputSockets_.size()
                  << " type=" << output.outputType
                  << " destination=" << outputHost << ":" << parsedOutputPort
                  << " interface="
                  << (output.interfaceAddress.empty() ? std::string("auto")
                                                      : output.interfaceAddress)
                  << " target_kbps=" << (config_.targetBitrate / 1000ULL)
                  << std::endl;
        outputSockets_.push_back(std::move(outputSocket));
    }

    running_.store(true, std::memory_order_release);
    httpStopRequested_.store(false, std::memory_order_release);
    try {
        worker_ = std::thread(&NativeUdpRelay::run, this);
    } catch (const std::exception& exception) {
        running_.store(false, std::memory_order_release);
        inputSocket_.close();
        fileInput_.close();
        outputSockets_.clear();
        error = std::string("native UDP worker thread could not start: ") + exception.what();
        return false;
    }
    if (httpInputSource_) {
        try {
            httpWorker_ = std::thread(&NativeUdpRelay::runHttpInput, this);
        } catch (const std::exception& exception) {
            running_.store(false, std::memory_order_release);
            httpQueueCondition_.notify_all();
            if (worker_.joinable()) worker_.join();
            inputSocket_.close();
            outputSockets_.clear();
            error = std::string("native HTTP worker thread could not start: ") +
                exception.what();
            return false;
        }
    }

    error.clear();
    return true;
}

void NativeUdpRelay::requestStop() noexcept {
    httpStopRequested_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    httpQueueCondition_.notify_all();
}

void NativeUdpRelay::stop() noexcept {
    requestStop();
    if (httpWorker_.joinable() && httpWorker_.get_id() != std::this_thread::get_id()) {
        httpWorker_.join();
    }
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
    inputSocket_.close();
    fileInput_.close();
    mp4Input_.reset();
    fileInputSource_ = false;
    httpInputSource_ = false;
    externalInputSource_ = false;
    for (auto& outputSocket : outputSockets_) {
        outputSocket->close();
    }
    outputSockets_.clear();
    {
        std::lock_guard<std::mutex> lock(httpQueueMutex_);
        httpQueue_.clear();
        httpQueueBufferPool_.clear();
        httpQueuedBytes_ = 0;
        httpQueueProducerWaiting_ = false;
        httpFinished_ = false;
    }
}

bool NativeUdpRelay::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

std::uint64_t NativeUdpRelay::inputBytes() const noexcept {
    return inputBytes_.load(std::memory_order_relaxed);
}

std::uint64_t NativeUdpRelay::sourceInputBytes() const noexcept {
    if (httpInputSource_) {
        return httpReceivedBytes_.load(std::memory_order_relaxed);
    }
    return inputBytes_.load(std::memory_order_relaxed);
}

std::uint64_t NativeUdpRelay::selectedInputBytes() const noexcept {
    return selectedInputBytes_.load(std::memory_order_relaxed);
}

std::uint64_t NativeUdpRelay::outputBytes() const noexcept {
    return outputBytes_.load(std::memory_order_relaxed);
}

std::vector<std::uint64_t> NativeUdpRelay::outputTransportBytesByIndex() const {
    // The vector is fixed for the entire running stream; only atomic values
    // change on output worker threads.
    std::vector<std::uint64_t> bytes;
    bytes.reserve(perOutputTransportBytes_.size());
    for (const auto& counter : perOutputTransportBytes_) {
        bytes.push_back(counter ? counter->load(std::memory_order_relaxed) : 0ULL);
    }
    return bytes;
}

std::uint64_t NativeUdpRelay::payloadOutputBytes() const noexcept {
    return payloadOutputBytes_.load(std::memory_order_relaxed);
}

std::uint64_t NativeUdpRelay::continuityErrors() const noexcept {
    return continuityErrors_.load(std::memory_order_relaxed);
}

std::string NativeUdpRelay::lastError() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

bool NativeUdpRelay::enqueueHttpData(const std::uint8_t* data, std::size_t size) {
    constexpr std::size_t kMaximumHttpQueueBytes = 2 * 1024 * 1024;
    constexpr std::size_t kMaximumChunkBytes = 65536;
    if (!data || size == 0) {
        return false;
    }
    for (std::size_t offset = 0; offset < size;) {
        // Small chunks allow near-20ms dequeue pacing for the opt-in
        // HTTP jitter smoother. Other feeds retain the 64KiB fast path.
        const std::size_t chunkSize =
            (std::min)(httpJitterEnabled_ ? std::size_t{8192}
                                          : kMaximumChunkBytes, size - offset);
        if (chunkSize > kMaximumHttpQueueBytes) {
            return false;
        }
        std::unique_lock<std::mutex> lock(httpQueueMutex_);
        std::uint64_t backpressureMs = 0;
        std::size_t backpressureQueuedBytes = 0;
        const auto hasQueueSpace = [this, chunkSize, kMaximumHttpQueueBytes] {
            return !running_.load(std::memory_order_acquire) ||
                httpQueuedBytes_ + chunkSize <= kMaximumHttpQueueBytes;
        };
        if (!hasQueueSpace()) {
            backpressureQueuedBytes = httpQueuedBytes_;
            const auto waitStarted = std::chrono::steady_clock::now();
            httpQueueProducerWaiting_ = true;
            httpQueueCondition_.wait(lock, hasQueueSpace);
            httpQueueProducerWaiting_ = false;
            backpressureMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - waitStarted).count());
        }
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }

        std::vector<std::uint8_t> chunk;
        if (!httpQueueBufferPool_.empty()) {
            chunk = std::move(httpQueueBufferPool_.back());
            httpQueueBufferPool_.pop_back();
        } else {
            chunk.reserve(kMaximumChunkBytes);
        }
        chunk.resize(chunkSize);
        std::memcpy(chunk.data(), data + offset, chunkSize);
        // V10.8.123: there is one producer and one relay consumer for this
        // bounded queue. Only wake the consumer when the queue transitions from
        // empty to non-empty; repeated broadcasts while it is already draining
        // create avoidable futex/scheduler churn on multi-service DVB inputs.
        const bool notifyConsumer = httpQueue_.empty();
        httpQueuedBytes_ += chunk.size();
        httpQueue_.push_back(std::move(chunk));
        lock.unlock();
        if (notifyConsumer) httpQueueCondition_.notify_one();
        if (config_.trustedAlignedExternalInput && backpressureMs > 25) {
            const auto n = inputBackpressureEvents_.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (n <= 3 || (n & (n - 1)) == 0) {
                std::cerr << "NATIVE DVB RELAY BACKPRESSURE stream="
                          << config_.streamId
                          << " wait_ms=" << backpressureMs
                          << " queued_before_bytes=" << backpressureQueuedBytes
                          << " queue_limit_bytes=" << kMaximumHttpQueueBytes
                          << " occurrences=" << n << std::endl;
            }
        }
        offset += chunkSize;
    }
    return true;
}

bool NativeUdpRelay::pushInput(const std::uint8_t* data, std::size_t size) {
    if (!externalInputSource_) return false;
    return enqueueHttpData(data, size);
}

void NativeUdpRelay::finishInput(const std::string& error) {
    if (!externalInputSource_) return;
    finishHttpInput(error);
}

void NativeUdpRelay::finishHttpInput(const std::string& error) {
    if (!error.empty()) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = error;
    }
    {
        std::lock_guard<std::mutex> lock(httpQueueMutex_);
        httpFinished_ = true;
    }
    httpQueueCondition_.notify_all();
}

void NativeUdpRelay::runHttpInput() {
    std::string location = config_.inputUri;
    dvbstreamer5::http::RequestOptions options;
    options.connectTimeoutMs = 10000;
    options.readTimeoutMs = 15000;
    options.writeTimeoutMs = 10000;
    options.userAgent = config_.userAgent.empty()
        ? "Mozilla/5.0 DVBStreamer5"
        : config_.userAgent;
    options.stopping = &httpStopRequested_;
    options.maxBodyBytes = (std::numeric_limits<std::size_t>::max)();
    // This is a long-running live input. Keep the request connection alive
    // while the origin is streaming; if it still closes, the loop below
    // reconnects without stopping the relay.
    options.keepAlive = true;

    if (config_.accessKeyMode == "header" &&
        !config_.accessKeyName.empty() && !config_.accessKeyValue.empty()) {
        if (config_.accessKeyName.find_first_of("\r\n") != std::string::npos ||
            config_.accessKeyValue.find_first_of("\r\n") != std::string::npos) {
            finishHttpInput("native HTTP input: access header contains a line break");
            return;
        }
        options.headers.emplace_back(config_.accessKeyName, config_.accessKeyValue);
    } else if (config_.accessKeyMode == "query" &&
        !config_.accessKeyName.empty() && !config_.accessKeyValue.empty()) {
        const std::string name = dvbstreamer5::http::encodeQueryComponent(config_.accessKeyName);
        const std::string value = dvbstreamer5::http::encodeQueryComponent(config_.accessKeyValue);
        const std::string key = name + "=";
        const auto fragmentPosition = location.find('#');
        const std::string fragment = fragmentPosition == std::string::npos
            ? std::string()
            : location.substr(fragmentPosition);
        if (fragmentPosition != std::string::npos) {
            location.erase(fragmentPosition);
        }
        const auto queryPosition = location.find('?');
        const std::string query = queryPosition == std::string::npos
            ? std::string()
            : location.substr(queryPosition + 1);
        if (query.rfind(key, 0) != 0 &&
            query.find("&" + key) == std::string::npos) {
            location += queryPosition == std::string::npos ? "?" : "&";
            location += name;
            location += "=";
            location += value;
        }
        location += fragment;
    }

    const bool hasAccessKey =
        !config_.accessKeyName.empty() && !config_.accessKeyValue.empty() &&
        (config_.accessKeyMode == "header" ||
            config_.accessKeyMode == "query");
    options.maxRedirects = hasAccessKey ? 0 : 8;

    const char* inputDiagnosticId =
        std::getenv("DVBSTREAMER5_DIAG_INPUT_STREAM_ID");
    const bool diagnosticHttp =
        inputDiagnosticId && *inputDiagnosticId &&
        config_.streamId == inputDiagnosticId;
    std::uint64_t diagHttpChunks = 0;
    std::uint64_t diagHttpBytes = 0;
    std::uint64_t diagHttpGapMaxMs = 0;
    std::uint64_t diagHttpGapsOver200Ms = 0;
    std::uint64_t diagHttpBurstsUnder5Ms = 0;
    std::uint64_t diagHttpEnqueueMaxMs = 0;
    std::uint64_t diagHttpEnqueueOver50Ms = 0;
    auto diagHttpLastChunkAt = std::chrono::steady_clock::time_point{};
    int reconnectDelaySeconds = 1;
    std::uint64_t attempt = 0;
    while (running_.load(std::memory_order_acquire) &&
           !httpStopRequested_.load(std::memory_order_acquire)) {
        ++attempt;
        std::cerr << "NATIVE HTTP INPUT attempt=" << attempt
                  << " url=" << location << std::endl;
        dvbstreamer5::http::Response response;
        std::string requestError;
        bool receivedAny = false;
        std::uint64_t receivedBytes = 0;

        const bool ok = dvbstreamer5::http::get(
            location, options, response, requestError,
            [this, &receivedAny, &receivedBytes, attempt, diagnosticHttp,
             &diagHttpChunks, &diagHttpBytes, &diagHttpGapMaxMs,
             &diagHttpGapsOver200Ms, &diagHttpBurstsUnder5Ms,
             &diagHttpEnqueueMaxMs, &diagHttpEnqueueOver50Ms,
             &diagHttpLastChunkAt](const std::uint8_t* data, std::size_t size) {
                if (size != 0 && !receivedAny) {
                    receivedAny = true;
                    std::cerr << "NATIVE HTTP INPUT connected attempt=" << attempt
                              << " first_chunk=" << size << std::endl;
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_.clear();
                }
                receivedBytes += size;
                httpReceivedBytes_.fetch_add(size, std::memory_order_relaxed);
                const auto arrivedAt = std::chrono::steady_clock::now();
                if (diagnosticHttp && size != 0 &&
                    diagHttpLastChunkAt.time_since_epoch().count() != 0) {
                    const auto gapMs = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            arrivedAt - diagHttpLastChunkAt).count());
                    diagHttpGapMaxMs = std::max(diagHttpGapMaxMs, gapMs);
                    if (gapMs > 200) ++diagHttpGapsOver200Ms;
                    if (gapMs < 5) ++diagHttpBurstsUnder5Ms;
                }
                if (diagnosticHttp && size != 0)
                    diagHttpLastChunkAt = arrivedAt;
                // The HTTP library invokes this callback synchronously on
                // the reader thread. Time spent enqueuing includes waiting
                // for the bounded 2 MiB queue, not network receive time.
                const bool accepted = enqueueHttpData(data, size);
                if (diagnosticHttp && size != 0) {
                    const auto queuedAt = std::chrono::steady_clock::now();
                    const auto enqueueMs = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            queuedAt - arrivedAt).count());
                    diagHttpEnqueueMaxMs =
                        std::max(diagHttpEnqueueMaxMs, enqueueMs);
                    if (enqueueMs > 50) ++diagHttpEnqueueOver50Ms;
                    diagHttpBytes += size;
                    if (++diagHttpChunks >= 125) {
                        std::cerr << "NATIVE HTTP SOURCE CADENCE"
                                  << " stream_id=" << config_.streamId
                                  << " chunks=" << diagHttpChunks
                                  << " bytes=" << diagHttpBytes
                                  << " network_gap_max_ms=" << diagHttpGapMaxMs
                                  << " network_gaps_over_200ms="
                                  << diagHttpGapsOver200Ms
                                  << " network_burst_under_5ms="
                                  << diagHttpBurstsUnder5Ms
                                  << " enqueue_max_ms=" << diagHttpEnqueueMaxMs
                                  << " enqueue_over_50ms="
                                  << diagHttpEnqueueOver50Ms
                                  << std::endl;
                        diagHttpChunks = 0;
                        diagHttpBytes = 0;
                        diagHttpGapMaxMs = 0;
                        diagHttpGapsOver200Ms = 0;
                        diagHttpBurstsUnder5Ms = 0;
                        diagHttpEnqueueMaxMs = 0;
                        diagHttpEnqueueOver50Ms = 0;
                    }
                }
                return accepted;
            });

        if (!running_.load(std::memory_order_acquire) ||
            httpStopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        // A live HTTP response ending normally is still a disconnect from our
        // point of view. Treat both EOF and transport errors as reconnectable.
        std::string reconnectError;
        if (!ok) {
            reconnectError = "native HTTP input reconnecting: " + requestError;
            if (response.status != 0) {
                reconnectError += " (HTTP " + std::to_string(response.status) + ")";
            }
        } else {
            reconnectError = "native HTTP input ended; reconnecting";
        }
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            lastError_ = reconnectError;
        }
        std::cerr << "NATIVE HTTP INPUT disconnected attempt=" << attempt
                  << " bytes=" << receivedBytes
                  << " status=" << response.status
                  << " ok=" << (ok ? 1 : 0)
                  << " error=" << (requestError.empty() ? "-" : requestError)
                  << std::endl;

        // Preserve the byte boundary between two independent HTTP responses.
        // The consumer uses an empty queue item as a discontinuity marker and
        // resets PacketFramer before accepting bytes from the next connection.
        // Without this marker a partial 188-byte packet from the old socket can
        // be concatenated with the first bytes of the new socket and create a
        // syntactically broken MPEG-TS packet.
        {
            std::lock_guard<std::mutex> lock(httpQueueMutex_);
            httpQueue_.emplace_back();
        }
        httpQueueCondition_.notify_all();

        // If we actually received transport data before the disconnect, start
        // the next retry again at 1 second. Repeated immediate failures back
        // off to a maximum of 5 seconds.
        const int retryDelaySeconds = reconnectDelaySeconds;
        reconnectDelaySeconds = receivedAny
            ? 1
            : (std::min)(5, reconnectDelaySeconds * 2);

        std::unique_lock<std::mutex> lock(httpQueueMutex_);
        httpQueueCondition_.wait_for(
            lock,
            std::chrono::seconds(retryDelaySeconds),
            [this] {
                return !running_.load(std::memory_order_acquire) ||
                    httpStopRequested_.load(std::memory_order_acquire);
            });
    }

    // Only stopping the relay, or a permanent validation error above, marks
    // the HTTP source finished. Transient network failures never do.
    finishHttpInput({});
}

void NativeUdpRelay::run() {
    struct OutputWorker {
        UdpSocket* socket = nullptr;
        std::size_t index = 0;
        std::string type;
        bool rtp = false;
        bool firstSendLogged = false;
        std::uint64_t datagramsSent = 0;
        std::shared_ptr<std::atomic<std::uint64_t>> sentTsBytes;
        std::unique_ptr<dvbstreamer5::media::rtp::MpegTsPacketizer> packetizer;
        std::unique_ptr<dvbstreamer5::media::network::WisiCbrOutputWorker> cbrWorker;
        std::unique_ptr<dvbstreamer5::media::network::PcrVbrUdpOutputWorker> vbrWorker;
    };

    // V386: observer may ride the first UDP-VBR worker, or use an
    // observer-only worker when the stream has HTTP/SRT/HLS outputs alone.
    const bool hasPhysicalCbr=std::any_of(config_.outputs.begin(),config_.outputs.end(),
        [](const auto& out){return out.outputType=="udp-cbr";});
    const bool vbrObserverEnabled=static_cast<bool>(config_.observeTransport) &&
        !config_.paceObservedTransport && !hasPhysicalCbr;
    bool vbrObserverAttached=false;
    std::unique_ptr<PcrVbrUdpOutputWorker> vbrObserverWorker;
    UdpInputEndpoint inputEndpoint;
    const bool networkInput = parseInputEndpoint(config_.inputUri, inputEndpoint);
    const bool httpInput = httpInputSource_ || externalInputSource_;
    const bool dvbInput = dvbInputSource_;
    if (!networkInput && !httpInput && !fileInputSource_ && !dvbInput) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = "native input endpoint became invalid";
        running_.store(false, std::memory_order_release);
        httpQueueCondition_.notify_all();
        return;
    }

    const bool rtpInput = networkInput && inputEndpoint.scheme == "rtp";
    bool fileInputEof = false;
    bool fileHadTsPackets = false;
    dvbstreamer5::media::mpegts::NativeFileLoopClock fileLoopClock;
    auto fileNextRead = std::chrono::steady_clock::time_point{};
    auto mp4WallOrigin = std::chrono::steady_clock::time_point{};
    std::uint64_t mp4MediaOrigin90k = 0;
    std::uint64_t mp4PreviousLoops = 0;
    std::uint64_t fileReadBps = config_.targetBitrate > 0
        ? dvbstreamer5::media::mpegts::CbrTsPacer::decodeTargetBitrate(config_.targetBitrate)
        : 4000000ULL;
    if (fileInputSource_ && !mp4Input_ && config_.fileInputLoop) {
        // Estimate the real playback rate from the file's own PCR and byte
        // offsets. A user-selected network CBR rate is not its media clock.
        // Bound scanning to 4 MiB and never read the whole replacement file.
        const auto fileStart = fileInput_.tellg();
        std::array<char, 188 * 7> block{};
        std::uint64_t firstPcr = 0, lastPcr = 0;
        std::uint64_t firstOffset = 0, lastOffset = 0;
        bool haveFirst = false;
        std::uint64_t scanned = 0;
        while (scanned < 4ULL * 1024ULL * 1024ULL && fileInput_) {
            fileInput_.read(block.data(), block.size());
            const auto bytes = fileInput_.gcount();
            if (bytes <= 0) break;
            for (std::streamsize pos = 0;
                 pos + 188 <= bytes; pos += 188) {
                dvbstreamer5::media::mpegts::PacketInfo info;
                if (!dvbstreamer5::media::mpegts::inspectPacket(
                        reinterpret_cast<const std::uint8_t*>(block.data() + pos),
                        188, info) || !info.hasPcr) continue;
                if (!haveFirst) {
                    haveFirst = true;
                    firstPcr = info.pcrBase90k;
                    firstOffset = scanned + static_cast<std::uint64_t>(pos);
                }
                lastPcr = info.pcrBase90k;
                lastOffset = scanned + static_cast<std::uint64_t>(pos);
            }
            scanned += static_cast<std::uint64_t>(bytes);
        }
        if (haveFirst && lastOffset > firstOffset) {
            const auto pcrDelta = (lastPcr - firstPcr) &
                dvbstreamer5::media::mpegts::NativeFileLoopClock::kMask;
            if (pcrDelta >= 90000 && pcrDelta <= 90000ULL * 60ULL) {
                const auto measured = (lastOffset - firstOffset) * 8ULL * 90000ULL /
                    pcrDelta;
                if (measured >= 100000ULL && measured <= 60000000ULL) {
                    fileReadBps = measured;
                }
            }
        }
        fileInput_.clear();
        fileInput_.seekg(fileStart);
        std::cerr << "NATIVE BACKUP FILE LOOP enabled"
                  << " stream_id=" << config_.streamId
                  << " media_kbps=" << fileReadBps / 1000ULL
                  << " pts_pcr_cc=continuous"
                  << std::endl;
    }
    // V10.3: keep the source/input MPEG-TS framer completely separate from
    // the post-transform/output framer. HTTP reads are not guaranteed to end
    // on 188-byte packet boundaries, so inputFramer can legitimately retain a
    // partial source TS packet between iterations. Reusing that same framer
    // for transcoder output contaminates the pending source bytes with muxed
    // output bytes and corrupts the next live input packet. Test-pattern input
    // often hid this because its chunks were already packet-aligned.
    dvbstreamer5::media::mpegts::PacketFramer inputFramer;
    dvbstreamer5::media::mpegts::PacketFramer transformedFramer;
    dvbstreamer5::media::mpegts::ContinuityTracker continuity;
    std::vector<dvbstreamer5::media::mpegts::Packet> packets;
    std::vector<dvbstreamer5::media::mpegts::Packet> remappedPackets;
    // V430: reuse the container; the chunks themselves are moved out of the
    // bounded queue without copying under its lock. Only selected DVB+CA
    // feeds coalesce already-buffered chunks, never wait for future input.
    std::vector<std::vector<std::uint8_t>> dvbCoalescedChunks;
    std::vector<std::uint8_t> sourceObservedTransport;
    std::vector<std::uint8_t> observedTransport;
    std::vector<OutputWorker> outputs;
    outputs.reserve(config_.outputs.size());
    std::size_t observedCbrOutputIndex = config_.outputs.size();
    if (config_.paceObservedTransport && config_.observeTransport) {
        for (std::size_t index = 0; index < config_.outputs.size(); ++index) {
            if (config_.outputs[index].outputType == "udp-cbr") {
                observedCbrOutputIndex = index;
                break;
            }
        }
    }
    auto seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (std::size_t index = 0; index < config_.outputs.size(); ++index) {
        OutputWorker output;
        output.socket = outputSockets_[index].get();
        output.index = index;
        output.type = config_.outputs[index].outputType;
        if (index < perOutputTransportBytes_.size())
            output.sentTsBytes = perOutputTransportBytes_[index];
        output.rtp = output.type == "rtp";
        if (output.rtp) {
            const auto outputSeed = seed + index;
            const std::uint32_t sourceId =
                static_cast<std::uint32_t>(outputSeed ^ (outputSeed >> 32));
            output.packetizer = std::make_unique<dvbstreamer5::media::rtp::MpegTsPacketizer>(
                sourceId,
                static_cast<std::uint16_t>(outputSeed >> 16),
                std::uint8_t{33},
                std::size_t{7});
        }
        if (config_.outputs[index].outputType == "udp-vbr") {
            try {
                output.vbrWorker =
                    std::make_unique<dvbstreamer5::media::network::PcrVbrUdpOutputWorker>(
                        output.socket,index,&outputBytes_,
                        output.sentTsBytes ? output.sentTsBytes.get() : nullptr,
                        [this](const std::string& message){
                            {std::lock_guard<std::mutex> lock(errorMutex_);lastError_=message;}
                            running_.store(false,std::memory_order_release);
                            httpQueueCondition_.notify_all();
                        },config_.streamId,
                        vbrObserverEnabled && !vbrObserverAttached
                            ? config_.observeTransport
                            : PcrVbrUdpOutputWorker::ObserverCallback{});
                if(vbrObserverEnabled && !vbrObserverAttached)
                    vbrObserverAttached=true;
            }catch(const std::exception& exception){
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastError_=exception.what();
                running_.store(false,std::memory_order_release);
                httpQueueCondition_.notify_all();
                return;
            }
        }
        if (config_.outputs[index].outputType == "udp-cbr") {
            try {
                dvbstreamer5::media::network::WisiCbrOutputWorker::Observer observer;
                if (index == observedCbrOutputIndex) observer = config_.observeTransport;
                // V513: transcoded source-PCR TS must pace *useful packets* against
                // its own queued PCR timeline, not an averaged useful bitrate.
                // Otherwise 40-ms PCRs can leave the CBR socket 200-400 ms apart
                // while NULL stuffing keeps the physical 8-Mbit sender regular.
                // Only the already selected source-PCR mode with a nonzero
                // startup reservoir opts in. Fixed PCR, copy and 0-ms profiles
                // retain their previous behavior and timestamp semantics.
                const std::uint64_t encodedCbrBitrate =
                    config_.wisiSourcePcrStrict
                        ? sourcePcrCbrTargetKeepingStartupBuffer(config_.targetBitrate)
                        : (config_.wisiPreserveSourcePcr &&
                           config_.targetBitrate % 1000ULL == 0ULL
                           ? config_.targetBitrate + 809ULL
                           : config_.targetBitrate);
                const bool transcodeSourcePcrPacketPacing =
                    config_.transcodedTransport &&
                    mpegts::CbrTsPacer::sourcePllEncodedTargetBitrate(encodedCbrBitrate) &&
                    mpegts::CbrTsPacer::startupBufferMilliseconds(encodedCbrBitrate) != 0;
                if (transcodeSourcePcrPacketPacing) {
                    std::cerr << "NATIVE TRANSCODE SOURCE PCR PACING stream_id="
                              << config_.streamId << " output_index=" << index
                              << " pcr=preserved packets=source_timeline"
                              << " fixed_cbr=unchanged" << std::endl;
                }
                output.cbrWorker =
                    std::make_unique<dvbstreamer5::media::network::WisiCbrOutputWorker>(
                        output.socket,
                        index,
                        encodedCbrBitrate,
                        &outputBytes_,
                        &running_,
                        std::move(observer),
                        [this](const std::string& message) {
                            {
                                std::lock_guard<std::mutex> lock(errorMutex_);
                                lastError_ = message;
                            }
                            running_.store(false, std::memory_order_release);
                            httpQueueCondition_.notify_all();
                        },
                        output.sentTsBytes ? output.sentTsBytes.get() : nullptr,
                        config_.streamId,
                        config_.sourcePcrPacketPacing || transcodeSourcePcrPacketPacing);
            } catch (const std::exception& exception) {
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastError_ = exception.what();
                running_.store(false, std::memory_order_release);
                httpQueueCondition_.notify_all();
                return;
            }
        }
        outputs.push_back(std::move(output));
        ++seed;
    }

    if(vbrObserverEnabled && !vbrObserverAttached){
        try{
            vbrObserverWorker=std::make_unique<PcrVbrUdpOutputWorker>(
                nullptr,static_cast<std::size_t>(-1),nullptr,nullptr,
                [this](const std::string& message){
                    {std::lock_guard<std::mutex> lock(errorMutex_);lastError_=message;}
                    running_.store(false,std::memory_order_release);
                    httpQueueCondition_.notify_all();
                },config_.streamId,config_.observeTransport);
            vbrObserverAttached=true;
        }catch(const std::exception& e){
            {std::lock_guard<std::mutex> lock(errorMutex_);lastError_=e.what();}
            running_.store(false,std::memory_order_release);
            httpQueueCondition_.notify_all();
            return;
        }
    }
    if(vbrObserverAttached)
        std::cerr<<"NATIVE OBSERVED VBR PCR START stream_id="<<config_.streamId
                 <<" shared_media=1 protocols=http,srt,hls,rtsp,rtmp"
                 <<" buffer_ms=1200"<<std::endl;

    // WISI CBR: the first UDP-CBR worker publishes its already-shaped
    // datagrams to observeTransport asynchronously. Observer work can never
    // block the physical WISI UDP sender clock.
    const bool shareObservedCbrWithUdpOutput =
        observedCbrOutputIndex < outputs.size();
    if (shareObservedCbrWithUdpOutput) {
        std::cerr << "NATIVE OBSERVED CBR share output_index="
                  << observedCbrOutputIndex
                  << " target_kbps=" << (config_.targetBitrate / 1000ULL)
                  << " profile=wisi-fixed-cbr async_observer=1"
                  << std::endl;
    }

    // V10.8.105: observeTransport feeds SRT/HTTP/HLS/RTSP/RTMP.  The old
    // implementation only slept between already-existing TS packets, so when
    // payload bitrate was below target it remained VBR and CBR Out simply
    // followed the source.  Give these consumers their own CbrTsPacer: it emits
    // the configured number of TS packets per second and fills missing capacity
    // with PID 0x1fff NULL packets, exactly like native UDP-CBR.
    std::unique_ptr<dvbstreamer5::media::mpegts::CbrTsPacer> observedCbrPacer;
    std::mutex observedCbrMutex;
    std::condition_variable observedCbrCondition;
    bool observedCbrStop = false;
    std::thread observedCbrWorker;

    if (config_.paceObservedTransport && config_.targetBitrate > 0 &&
        !shareObservedCbrWithUdpOutput) {
        try {
            // HTTP/SRT/HLS observers must keep PCR in the same clock domain
                // as unchanged video PTS/DTS. The legacy fixed-CBR pacer
                // rewrites PCR to output packet slots and can stall VLC after
                // an HLS/VAAPI transcode even while TCP delivers full rate.
                // The +799 marker selects the existing source-PCR timeline
                // mode; it is decoded internally and does not change CBR bps.
            observedCbrPacer =
                std::make_unique<dvbstreamer5::media::mpegts::CbrTsPacer>(
                    config_.targetBitrate % 1000ULL == 0ULL
                        ? config_.targetBitrate + 799ULL
                        : config_.targetBitrate);
            // V336: an observer-only SRT/HTTP CBR output has no WISI sender
            // startup reservoir to absorb the async encoder's first IDR and
            // audio bursts. Honour the existing per-tile CBR buffer control
            // for *transcoded* media only. Cap its size to 70% of the same
            // 8 MiB pacer queue, leaving room for live input jitter.
            const auto requestedObserverBufferMs = config_.transcodedTransport
                ? dvbstreamer5::media::mpegts::CbrTsPacer::startupBufferMilliseconds(
                    config_.targetBitrate)
                : 0ULL;
            const auto observerPhysicalBps =
                dvbstreamer5::media::mpegts::CbrTsPacer::decodeTargetBitrate(
                    config_.targetBitrate);
            const auto observerSafeBufferMs = observerPhysicalBps == 0
                ? 0ULL
                : static_cast<std::uint64_t>(
                    dvbstreamer5::media::mpegts::CbrTsPacer::maximumQueuedBytes())
                    * 8ULL * 1000ULL * 70ULL / (observerPhysicalBps * 100ULL);
            const auto observerStartupMs =
                (std::min)(requestedObserverBufferMs, observerSafeBufferMs);
            const auto observerStartupBuffer =
                std::chrono::milliseconds(observerStartupMs);
            // Capture the delay by value: the setup block ends before the
            // observer thread's lifetime. All other referenced objects live
            // until this thread is joined by run().
            observedCbrWorker = std::thread([&, observerStartupBuffer] {
                bool startupBuffered = observerStartupBuffer.count() == 0;
                auto firstReadyAt = std::chrono::steady_clock::time_point{};
                while (true) {
                    dvbstreamer5::media::mpegts::CbrDatagram cbrDatagram;
                    bool ready = false;
                    {
                        std::unique_lock<std::mutex> lock(observedCbrMutex);
                        observedCbrCondition.wait(lock, [&] {
                            return observedCbrStop ||
                                (observedCbrPacer && observedCbrPacer->started());
                        });
                        if (observedCbrStop) break;

                        // Delay only the initial packet clock; do not move
                        // PCR/PTS/DTS or throttle the media producer. A fresh
                        // transport anchor prevents a catch-up burst at start.
                        if (!startupBuffered) {
                            if (firstReadyAt ==
                                std::chrono::steady_clock::time_point{}) {
                                firstReadyAt = std::chrono::steady_clock::now();
                            }
                            if (observedCbrCondition.wait_until(
                                    lock, firstReadyAt + observerStartupBuffer,
                                    [&] { return observedCbrStop; })) {
                                break;
                            }
                            if (observedCbrStop) break;
                            observedCbrPacer->reanchorDeadline(
                                std::chrono::steady_clock::now());
                            startupBuffered = true;
                        }

                        const auto deadline = observedCbrPacer->nextDeadline();
                        if (observedCbrCondition.wait_until(
                                lock, deadline, [&] { return observedCbrStop; })) {
                            break;
                        }
                        if (observedCbrStop) break;

                        ready = observedCbrPacer->nextDatagram(
                            std::chrono::steady_clock::now(), cbrDatagram);
                    }
                    if (!ready) continue;

                    constexpr std::size_t kObservedCbrBytes =
                        dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram *
                        dvbstreamer5::media::mpegts::kPacketSize;
                    static_assert(
                        sizeof(dvbstreamer5::media::mpegts::CbrDatagram) ==
                            kObservedCbrBytes,
                        "observed CBR datagram storage must be contiguous");
                    // V10.8.140: nextDatagram() fills every one of the seven
                    // packets before returning true, so the observer can use
                    // CbrDatagram storage directly with no second memset/copy.
                    config_.observeTransport(
                        reinterpret_cast<const std::uint8_t*>(cbrDatagram.data()),
                        kObservedCbrBytes);
                }
            });
            std::cerr << "NATIVE OBSERVED CBR start target_kbps="
                      << (observerPhysicalBps / 1000ULL)
                      << " transcoded=" << (config_.transcodedTransport ? 1 : 0)
                      << " requested_startup_ms=" << requestedObserverBufferMs
                      << " effective_startup_ms=" << observerStartupMs
                      << " source_pcr="
                      << (observedCbrPacer->sourcePllMode() ? 1 : 0)
                      << std::endl;
        } catch (const std::exception& exception) {
            {
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastError_ = std::string("native observed CBR setup failed: ") +
                    exception.what();
            }
            running_.store(false, std::memory_order_release);
            httpQueueCondition_.notify_all();
            return;
        }
    }

    auto observePackets = [&](
        const std::vector<dvbstreamer5::media::mpegts::Packet>& observedPackets) -> bool {
        if (!config_.observeTransport || observedPackets.empty()) return true;

        // The first UDP-CBR pacer will publish its already-shaped 1316-byte
        // datagrams below. Do not enqueue the same TS into a second pacer.
        if (shareObservedCbrWithUdpOutput) return true;

        // One VBR worker publishes time-aligned TS to every observer exactly
        // once. It is either the first UDP-VBR worker (queued below), or a
        // standalone shared observer when no physical VBR output exists.
        if(vbrObserverAttached){
            if(vbrObserverWorker && !vbrObserverWorker->enqueue(observedPackets)){
                {std::lock_guard<std::mutex> lock(errorMutex_);
                 lastError_="shared VBR PCR observer stopped";}
                running_.store(false,std::memory_order_release);
                return false;
            }
            return true;
        }

        if (observedCbrPacer) {
            bool wakeForStart = false;
            {
                std::lock_guard<std::mutex> lock(observedCbrMutex);
                const bool wasStarted = observedCbrPacer->started();
                for (const auto& packet : observedPackets) {
                    if (!observedCbrPacer->enqueue(packet)) {
                        std::lock_guard<std::mutex> errorLock(errorMutex_);
                        // V341: the old error string said 2 MiB even though
                        // the current CbrTsPacer limit is 8 MiB. Record the
                        // exact queue and physical target so a persistent
                        // encoded bitrate > transport CBR is distinguishable
                        // from a short encoder/decoder burst.
                        lastError_ =
                            "observed CBR pacing queue overflow (queued_bytes=" +
                            std::to_string(observedCbrPacer->queuedBytes()) +
                            ", limit_bytes=" +
                            std::to_string(
                                dvbstreamer5::media::mpegts::CbrTsPacer::maximumQueuedBytes()) +
                            ", target_kbps=" +
                            std::to_string(observedCbrPacer->targetBitrate() / 1000ULL) +
                            ", measured_useful_kbps=" +
                            std::to_string(observedCbrPacer->sourcePayloadBitrate() / 1000ULL) +
                            "); increase network CBR headroom or lower encoder bitrate";
                        std::cerr << "NATIVE OBSERVED CBR OVERFLOW"
                                  << " stream_id=" << config_.streamId
                                  << " queued_bytes=" << observedCbrPacer->queuedBytes()
                                  << " limit_bytes="
                                  << dvbstreamer5::media::mpegts::CbrTsPacer::maximumQueuedBytes()
                                  << " target_kbps="
                                  << observedCbrPacer->targetBitrate() / 1000ULL
                                  << " measured_useful_kbps="
                                  << observedCbrPacer->sourcePayloadBitrate() / 1000ULL
                                  << " paced_useful_kbps="
                                  << observedCbrPacer->usefulPaceBitrate() / 1000ULL
                                  << " buffered_media_ms="
                                  << observedCbrPacer->bufferedMediaMilliseconds()
                                  << " timing_locked="
                                  << (observedCbrPacer->timingLocked() ? 1 : 0)
                                  << std::endl;
                        running_.store(false, std::memory_order_release);
                        httpQueueCondition_.notify_all();
                        return false;
                    }
                }
                // V10.8.126: once the observed pacer has started, its worker
                // sleeps on nextDeadline() and new input does not move that
                // deadline. Repeated notify_one() calls only wake wait_until()
                // early, creating avoidable futex/scheduler churn. The first
                // successful enqueue still wakes the worker immediately.
                wakeForStart = !wasStarted && observedCbrPacer->started();
            }
            if (wakeForStart) observedCbrCondition.notify_one();
            return true;
        }

        constexpr std::size_t kObservedPacketsPerBatch = 7;
        for (std::size_t first = 0; first < observedPackets.size();
             first += kObservedPacketsPerBatch) {
            if (!running_.load(std::memory_order_acquire)) return false;
            const std::size_t count = (std::min)(
                kObservedPacketsPerBatch, observedPackets.size() - first);
            std::array<std::uint8_t,
                kObservedPacketsPerBatch * dvbstreamer5::media::mpegts::kPacketSize> bytes {};
            for (std::size_t index = 0; index < count; ++index) {
                std::memcpy(
                    bytes.data() + index * dvbstreamer5::media::mpegts::kPacketSize,
                    observedPackets[first + index].data(),
                    dvbstreamer5::media::mpegts::kPacketSize);
            }
            config_.observeTransport(
                bytes.data(), count * dvbstreamer5::media::mpegts::kPacketSize);
        }
        return true;
    };
    std::array<std::uint8_t, 65536> datagram {};
    std::string error;

    const char* relayDiagnosticId =
        std::getenv("DVBSTREAMER5_DIAG_INPUT_STREAM_ID");
    const bool diagnosticHttpRelay =
        httpInputSource_ && relayDiagnosticId && *relayDiagnosticId &&
        config_.streamId == relayDiagnosticId;
    auto diagRelayLastChunkAt = std::chrono::steady_clock::time_point{};
    std::uint64_t diagRelayChunks = 0;
    std::uint64_t diagRelayBytes = 0;
    std::uint64_t diagRelayGapMaxMs = 0;
    std::uint64_t diagRelayGapsOver200Ms = 0;
    std::uint64_t diagRelayTransformMaxMs = 0;
    std::uint64_t diagRelayLoopMaxMs = 0;
    std::uint64_t diagRelayLoopsOver100Ms = 0;
    std::size_t diagRelayQueuePeakBytes = 0;
    std::uint64_t slowCaBatchEvents = 0;
    std::uint64_t slowPipelineEvents = 0;

    // Only for a single explicitly opted-in HTTP TS service. The HTTP reader
    // continues receiving immediately into the existing bounded 2MiB queue.
    // This consumer pre-buffers source bursts, then releases small TS chunks
    // using an adaptive byte-rate clock; it NEVER edits PCR, PTS or DTS.
    constexpr auto kHttpJitterPrebuffer = std::chrono::milliseconds(1200);
    auto jitterFirstChunkAt = std::chrono::steady_clock::time_point{};
    auto jitterNextChunkAt = std::chrono::steady_clock::time_point{};
    auto jitterRateMeasuredAt = std::chrono::steady_clock::time_point{};
    bool jitterReady = false;
    std::uint64_t jitterRateBps = 0;
    std::uint64_t jitterRateBytesAt = 0;
    std::uint64_t jitterOutputChunks = 0;
    std::uint64_t jitterOutputBytes = 0;

    while (running_.load(std::memory_order_acquire)) {
        bool diagRelayDidChunk = false;
        auto diagRelayChunkStart = std::chrono::steady_clock::time_point{};
        std::size_t received = 0;
        packets.clear();
        dvbCoalescedChunks.clear();
        if (fileInputSource_) {
            if (mp4Input_) {
                const bool cbrSenderReady = std::all_of(outputs.begin(),outputs.end(),
                    [](const OutputWorker& o) {
                        return !o.cbrWorker || o.cbrWorker->queuedPackets() <
                            dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram;
                    });
                if (!cbrSenderReady) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                std::uint64_t mediaClock90k = 0;
                if (!mp4Input_->next(packets, mediaClock90k, error)) {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = "native MP4 replacement: " + error;
                    break;
                }
                if (mp4WallOrigin == std::chrono::steady_clock::time_point{}) {
                    mp4WallOrigin = std::chrono::steady_clock::now();
                    mp4MediaOrigin90k = mediaClock90k;
                }
                const auto elapsed90k = mediaClock90k >= mp4MediaOrigin90k
                    ? mediaClock90k - mp4MediaOrigin90k : 0ULL;
                const auto deadline = mp4WallOrigin +
                    std::chrono::nanoseconds(elapsed90k * 1000000000ULL / 90000ULL);
                while (running_.load(std::memory_order_acquire)) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) break;
                    std::this_thread::sleep_for(std::min(
                        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                        std::chrono::milliseconds(20)));
                }
                if (mp4Input_->loops() != mp4PreviousLoops) {
                    mp4PreviousLoops = mp4Input_->loops();
                    std::cerr << "NATIVE BACKUP MP4 REWIND stream_id="
                              << config_.streamId << " loops=" << mp4PreviousLoops
                              << " pts_pcr_cc=continuous" << std::endl;
                }
                inputBytes_.fetch_add(packets.size() *
                    dvbstreamer5::media::mpegts::kPacketSize,
                    std::memory_order_relaxed);
            } else {
            if (fileInputEof && config_.fileInputLoop && fileHadTsPackets) {
                fileInput_.clear();
                fileInput_.seekg(0, std::ios::beg);
                if (!fileInput_) {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = "replacement TS file seek to start failed";
                    break;
                }
                inputFramer.reset();
                fileLoopClock.nextLoop();
                fileInputEof = false;
                std::cerr << "NATIVE BACKUP FILE REWIND"
                          << " stream_id=" << config_.streamId
                          << " loops=" << fileLoopClock.loops()
                          << std::endl;
            }
            const bool dedicatedCbrSender = std::any_of(
                outputs.begin(), outputs.end(), [](const OutputWorker& output) {
                    return static_cast<bool>(output.cbrWorker);
                });
            const bool needsFileData = !dedicatedCbrSender || std::any_of(
                outputs.begin(), outputs.end(), [](const OutputWorker& output) {
                    return output.cbrWorker &&
                        output.cbrWorker->queuedPackets() <
                            dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram;
                });
            if (!fileInputEof && needsFileData) {
                if (!dedicatedCbrSender) {
                    const auto now = std::chrono::steady_clock::now();
                    if (fileNextRead > now) {
                        std::this_thread::sleep_until(fileNextRead);
                    } else if (fileNextRead == std::chrono::steady_clock::time_point{} ||
                               now - fileNextRead > std::chrono::seconds(1)) {
                        fileNextRead = now;
                    }
                }
                constexpr std::size_t kFileReadSize =
                    dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram *
                    dvbstreamer5::media::mpegts::kPacketSize;
                fileInput_.read(
                    reinterpret_cast<char*>(datagram.data()),
                    static_cast<std::streamsize>(kFileReadSize));
                received = static_cast<std::size_t>(fileInput_.gcount());
                if (fileInput_.bad()) {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = "native file input read failed";
                    break;
                }
                fileInputEof = fileInput_.eof();
                if (received > 0) {
                    inputBytes_.fetch_add(received, std::memory_order_relaxed);
                    inputFramer.push(datagram.data(), received, packets);
                    fileHadTsPackets = fileHadTsPackets || !packets.empty();
                    if (config_.fileInputLoop) {
                        for (auto& packet : packets) fileLoopClock.apply(packet);
                    }
                    if (!dedicatedCbrSender && fileReadBps > 0) {
                        const auto intervalNs = static_cast<std::uint64_t>(received) *
                            8ULL * 1000000000ULL / fileReadBps;
                        fileNextRead += std::chrono::nanoseconds(intervalNs);
                    }
                }
            }
            if (fileInputEof && packets.empty() &&
                !(config_.fileInputLoop && fileHadTsPackets) && std::all_of(
                    outputs.begin(), outputs.end(), [](const OutputWorker& output) {
                        return !output.cbrWorker || output.cbrWorker->queuedPackets() == 0;
                    })) {
                if (!fileHadTsPackets) {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ =
                        "native file input contains no complete MPEG-TS packets";
                }
                break;
            }
            if (received == 0) {
                // The dedicated WISI sender owns all CBR deadlines. The input
                // relay must not wake once per UDP slot merely to watch them.
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            }
        } else if (httpInput) {
            std::unique_lock<std::mutex> lock(httpQueueMutex_);
            if (httpQueue_.empty()) {
                const bool cbrDrained = std::all_of(
                    outputs.begin(), outputs.end(), [](const OutputWorker& output) {
                        return !output.cbrWorker ||
                            output.cbrWorker->queuedPackets() == 0;
                    });
                if (httpFinished_ && cbrDrained) {
                    break;
                }
                if (httpFinished_) {
                    // Source is done but the dedicated WISI queue may still be
                    // draining. Avoid a busy loop without coupling to its CBR clock.
                    httpQueueCondition_.wait_for(
                        lock, std::chrono::milliseconds(20), [this] {
                            return !httpQueue_.empty() ||
                                !running_.load(std::memory_order_acquire);
                        });
                } else {
                    httpQueueCondition_.wait_for(
                        lock, std::chrono::milliseconds(250), [this] {
                            return !httpQueue_.empty() || httpFinished_ ||
                                !running_.load(std::memory_order_acquire);
                        });
                }
            }
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            if (!httpQueue_.empty()) {
                // Do not pace a discontinuity marker (empty vector). On
                // reconnect, start a fresh latency budget and bitrate clock.
                if (httpJitterEnabled_ && !httpQueue_.front().empty()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (!jitterReady) {
                        if (jitterFirstChunkAt ==
                            std::chrono::steady_clock::time_point{}) {
                            jitterFirstChunkAt = now;
                        }
                        const auto readyAt =
                            jitterFirstChunkAt + kHttpJitterPrebuffer;
                        if (now < readyAt && !httpFinished_) {
                            httpQueueCondition_.wait_until(lock, readyAt,
                                [this] {
                                    return !running_.load(
                                        std::memory_order_acquire);
                                });
                            continue;
                        }
                        const auto elapsedUs = static_cast<std::uint64_t>(
                            std::max<std::int64_t>(1,
                                std::chrono::duration_cast<
                                    std::chrono::microseconds>(
                                    now - jitterFirstChunkAt).count()));
                        const auto receivedBytes =
                            httpReceivedBytes_.load(std::memory_order_relaxed);
                        // Estimate an initial transport rate from actual
                        // HTTP bytes received during the prebuffer interval.
                        const auto measured =
                            (receivedBytes >= jitterRateBytesAt
                                ? receivedBytes - jitterRateBytesAt
                                : 0ULL) * 8000000ULL / elapsedUs;
                        jitterRateBps = std::clamp<std::uint64_t>(
                            measured, 128000ULL, 80000000ULL);
                        jitterRateMeasuredAt = now;
                        jitterRateBytesAt = receivedBytes;
                        jitterNextChunkAt = now;
                        jitterReady = true;
                        std::cerr << "NATIVE HTTP JITTER ready"
                                  << " stream_id=" << config_.streamId
                                  << " prebuffer_ms=" << elapsedUs / 1000ULL
                                  << " bitrate_kbps=" << jitterRateBps / 1000ULL
                                  << " backlog_bytes=" << httpQueuedBytes_
                                  << std::endl;
                    }
                    // Re-estimate over several upstream HTTP bursts rather
                    // than reacting to individual 460ms source gaps.
                    if (now - jitterRateMeasuredAt >=
                        std::chrono::seconds(3)) {
                        const auto currentBytes =
                            httpReceivedBytes_.load(std::memory_order_relaxed);
                        const auto elapsedUs = static_cast<std::uint64_t>(
                            std::max<std::int64_t>(1,
                                std::chrono::duration_cast<
                                    std::chrono::microseconds>(
                                    now - jitterRateMeasuredAt).count()));
                        if (currentBytes > jitterRateBytesAt) {
                            const auto measured =
                                (currentBytes - jitterRateBytesAt) *
                                8000000ULL / elapsedUs;
                            const auto bounded = std::clamp<std::uint64_t>(
                                measured, 128000ULL, 80000000ULL);
                            jitterRateBps = (jitterRateBps * 3ULL +
                                             bounded) / 4ULL;
                        }
                        jitterRateMeasuredAt = now;
                        jitterRateBytesAt = currentBytes;
                    }
                    if (now < jitterNextChunkAt) {
                        httpQueueCondition_.wait_until(
                            lock, jitterNextChunkAt, [this] {
                                return !running_.load(
                                    std::memory_order_acquire);
                            });
                        continue;
                    }
                }
                auto chunk = std::move(httpQueue_.front());
                httpQueue_.pop_front();
                if (httpJitterEnabled_ && !chunk.empty() && jitterReady) {
                    // V10.8.284: feedback control against the actual
                    // reservoir depth. V283 corrected by only +/-10% after
                    // half/full-target thresholds, allowing a gradual drift
                    // from ~0.4 MiB to >0.8 MiB on a ~3.5 Mbit/s HTTP feed.
                    // Correct continuously, but cap acceleration at +30%
                    // and slowdown at -15% to avoid recreating RAW bursts.
                    // This affects only the opt-in HTTP input byte clock:
                    // media PCR/PTS/DTS, encoder and AV mux remain intact.
                    const auto targetBytes = std::clamp<std::uint64_t>(
                        jitterRateBps * 1200ULL / 8000ULL,
                        16384ULL, 1258291ULL);
                    const auto correctionPct =
                        detail::httpTsJitterCorrectionPercent(
                            httpQueuedBytes_, targetBytes);
                    const auto effectiveBps = std::max<std::uint64_t>(
                        1ULL, jitterRateBps *
                            static_cast<std::uint64_t>(100LL + correctionPct) /
                            100ULL);
                    const auto now = std::chrono::steady_clock::now();
                    const auto advanceUs = std::max<std::uint64_t>(
                        1ULL, static_cast<std::uint64_t>(
                            chunk.size()) * 8000000ULL / effectiveBps);
                    jitterNextChunkAt =
                        std::max(jitterNextChunkAt, now) +
                        std::chrono::microseconds(
                            static_cast<std::int64_t>(advanceUs));
                    jitterOutputBytes += chunk.size();
                    if (++jitterOutputChunks >= 125) {
                        std::cerr << "NATIVE HTTP JITTER STATUS"
                                  << " stream_id=" << config_.streamId
                                  << " chunks=" << jitterOutputChunks
                                  << " bytes=" << jitterOutputBytes
                                  << " bitrate_kbps=" << jitterRateBps / 1000ULL
                                  << " backlog_bytes=" << httpQueuedBytes_
                                  << " target_bytes=" << targetBytes
                                  << " correction_pct=" << correctionPct
                                  << " output_kbps=" << effectiveBps / 1000ULL
                                  << " mode=paced_input"
                                  << std::endl;
                        jitterOutputChunks = 0;
                        jitterOutputBytes = 0;
                    }
                }
                if (diagnosticHttpRelay && !chunk.empty()) {
                    diagRelayQueuePeakBytes = std::max(
                        diagRelayQueuePeakBytes, httpQueuedBytes_);
                    diagRelayChunkStart = std::chrono::steady_clock::now();
                    if (diagRelayLastChunkAt.time_since_epoch().count() != 0) {
                        const auto gapMs = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                diagRelayChunkStart - diagRelayLastChunkAt).count());
                        diagRelayGapMaxMs = std::max(diagRelayGapMaxMs, gapMs);
                        if (gapMs > 200) ++diagRelayGapsOver200Ms;
                    }
                    diagRelayLastChunkAt = diagRelayChunkStart;
                    diagRelayDidChunk = true;
                    diagRelayBytes += chunk.size();
                }
                httpQueuedBytes_ -= chunk.size();

                // V430: A shared-DVB CA relay can accumulate tens of
                // thousands of 188-byte packets in this queue while its
                // bitslice decoder processes tiny input calls. Drain up to
                // four 77-packet CA batches when they are ALREADY queued.
                // Moving buffers requires no TS copying while holding the
                // queue mutex. Do not change HTTP/HLS/SRT or plain DVB paths,
                // and never merge across an empty reconnect marker.
                if (config_.trustedAlignedExternalInput &&
                    config_.processTransport && !chunk.empty()) {
                    constexpr std::size_t kDvbCoalesceBytes =
                        4U * 77U * dvbstreamer5::media::mpegts::kPacketSize;
                    std::size_t coalescedBytes = chunk.size();
                    while (!httpQueue_.empty() && !httpQueue_.front().empty()) {
                        const std::size_t nextBytes = httpQueue_.front().size();
                        if (nextBytes > kDvbCoalesceBytes - (std::min)(
                                coalescedBytes, kDvbCoalesceBytes)) {
                            break;
                        }
                        dvbCoalescedChunks.push_back(
                            std::move(httpQueue_.front()));
                        httpQueue_.pop_front();
                        httpQueuedBytes_ -= nextBytes;
                        coalescedBytes += nextBytes;
                    }
                }

                // V10.8.139: most dequeues happen while the 2 MiB queue has
                // ample space. Avoid a futex wake unless enqueueHttpData()
                // actually had to block on backpressure. The flag and queue
                // state are observed under the same mutex, so no wake is lost.
                const bool notifyProducer = httpQueueProducerWaiting_;
                lock.unlock();
                if (notifyProducer) httpQueueCondition_.notify_one();

                if (chunk.empty()) {
                    // HTTP reconnect boundary.  Do not join an incomplete TS
                    // packet from the previous response with bytes from the
                    // next response.  Continuity counters are also expected to
                    // jump when a live origin reconnects.
                    inputFramer.reset();
                    transformedFramer.reset();
                    continuity.reset();
                    if (httpJitterEnabled_) {
                        jitterReady = false;
                        jitterFirstChunkAt = {};
                        jitterNextChunkAt = {};
                        jitterRateMeasuredAt = {};
                        jitterRateBps = 0;
                        jitterRateBytesAt = httpReceivedBytes_.load(
                            std::memory_order_relaxed);
                        jitterOutputChunks = 0;
                        jitterOutputBytes = 0;
                    }
                    continue;
                }

                received = chunk.size();
                // All extra blocks belong to the same service and are
                // already structurally aligned. Append in original arrival
                // order, then invoke the unchanged 77-packet CA hook on the
                // resulting vector. This improves native bitslice occupancy.
                if (config_.trustedAlignedExternalInput) {
                    inputFramer.pushTrustedAligned(chunk.data(), chunk.size(), packets);
                    for (const auto& more : dvbCoalescedChunks) {
                        received += more.size();
                        inputFramer.pushTrustedAligned(
                            more.data(), more.size(), packets);
                    }
                } else {
                    inputFramer.push(chunk.data(), received, packets);
                }
                inputBytes_.fetch_add(received, std::memory_order_relaxed);

                {
                    std::lock_guard<std::mutex> recycleLock(httpQueueMutex_);
                    constexpr std::size_t kMaximumRecycledQueueBuffers = 2;
                    if (httpQueueBufferPool_.size() < kMaximumRecycledQueueBuffers) {
                        chunk.clear();
                        httpQueueBufferPool_.push_back(std::move(chunk));
                    }
                    for (auto& extra : dvbCoalescedChunks) {
                        if (httpQueueBufferPool_.size() ==
                            kMaximumRecycledQueueBuffers) break;
                        extra.clear();
                        httpQueueBufferPool_.push_back(std::move(extra));
                    }
                }
            } else if (httpFinished_) {
                const bool cbrDrained = std::all_of(
                    outputs.begin(), outputs.end(), [](const OutputWorker& output) {
                        return !output.cbrWorker ||
                            output.cbrWorker->queuedPackets() == 0;
                    });
                if (cbrDrained) {
                    break;
                }
            }
        } else {
            // Input reads no longer share the WISI output clock. A fixed timeout
            // avoids thousands of unnecessary wakeups per second across channels.
            int receiveTimeoutMs = 250;
            const bool receivedOk = dvbInput
                ? dvbInput_.read(
                    datagram.data(), datagram.size(), received, receiveTimeoutMs, error)
                : inputSocket_.receive(
                    datagram.data(), datagram.size(), received, receiveTimeoutMs, error);
            if (!receivedOk) {
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = error.empty()
                        ? (dvbInput ? "DVB input read failed" : "UDP receive failed")
                        : error;
                }
                break;
            }

            if (received > 0) {
                inputBytes_.fetch_add(received, std::memory_order_relaxed);
                if (rtpInput) {
                    dvbstreamer5::media::rtp::PacketView rtp;
                    if (!dvbstreamer5::media::rtp::parsePacket(datagram.data(), received, rtp) ||
                        !dvbstreamer5::media::rtp::decodeMpegTsPayload(rtp, packets)) {
                        continuityErrors_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                } else {
                    inputFramer.push(datagram.data(), received, packets);
                }
            }
        }

        // Time one active DVB relay consumer iteration (not the idle wait).
        const bool measureDvbPipeline =
            config_.trustedAlignedExternalInput && !packets.empty();
        const auto pipelineStarted = measureDvbPipeline
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        if (!packets.empty() && config_.observeSourceTransport) {
            sourceObservedTransport.resize(
                packets.size() * dvbstreamer5::media::mpegts::kPacketSize);
            for (std::size_t index = 0; index < packets.size(); ++index) {
                std::memcpy(
                    sourceObservedTransport.data() +
                        index * dvbstreamer5::media::mpegts::kPacketSize,
                    packets[index].data(),
                    dvbstreamer5::media::mpegts::kPacketSize);
            }
            config_.observeSourceTransport(
                sourceObservedTransport.data(), sourceObservedTransport.size());
        }

        if (!packets.empty() && config_.remapEnabled) {
            remappedPackets.clear();
            for (const auto& packet : packets) {
                // Live network inputs can contain an isolated damaged packet
                // around reconnect/discontinuity.  A malformed TS packet must
                // be dropped and counted, not turn the whole service OFFLINE.
                dvbstreamer5::media::mpegts::PacketInfo packetInfo;
                if (!dvbstreamer5::media::mpegts::inspectPacket(
                        packet.data(), packet.size(), packetInfo)) {
                    continuityErrors_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (!remapper_.process(packet, remappedPackets, error)) {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = error.empty() ? "native MPEG-TS remap failed" : error;
                    running_.store(false, std::memory_order_release);
                    break;
                }
            }
            if (!running_.load(std::memory_order_acquire)) break;
            packets.swap(remappedPackets);
        }

        if (!packets.empty() && config_.processTransport) {
            constexpr std::size_t kCaBatchPackets = 77;
            // V429: Packet is exactly a contiguous 188-byte std::array.
            // The CA hook mutates TS bytes in place. Use 77-packet slices of
            // the existing vector instead of copying every packet to a
            // temporary 14 KiB buffer and copying it back after decrypt.
            // Batch boundaries and ECM/CW processing order stay unchanged.
            static_assert(sizeof(dvbstreamer5::media::mpegts::Packet) ==
                dvbstreamer5::media::mpegts::kPacketSize,
                "CA in-place TS storage must be contiguous");
            for (std::size_t first = 0; first < packets.size(); first += kCaBatchPackets) {
                const std::size_t count = (std::min)(
                    kCaBatchPackets, packets.size() - first);
                const std::size_t bytes =
                    count * dvbstreamer5::media::mpegts::kPacketSize;
                auto* const caBytes = reinterpret_cast<std::uint8_t*>(
                    packets.data() + first);
                const bool traceCa = config_.trustedAlignedExternalInput;
                const auto caStarted = traceCa
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                const bool accepted = config_.processTransport(caBytes, bytes);
                if (traceCa) {
                    const auto caMs = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - caStarted).count();
                    if (caMs > 25 && (++slowCaBatchEvents <= 3 ||
                        (slowCaBatchEvents & (slowCaBatchEvents - 1)) == 0)) {
                        std::cerr << "NATIVE DVB CA BATCH SLOW stream="
                                  << config_.streamId
                                  << " packets=" << count
                                  << " elapsed_ms=" << caMs
                                  << " occurrences=" << slowCaBatchEvents
                                  << std::endl;
                    }
                }
                if (!accepted) {
                    error = "native conditional-access transport processing failed";
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = error;
                    running_.store(false, std::memory_order_release);
                    break;
                }
            }
            if (!running_.load(std::memory_order_acquire)) break;
        }

        if (!packets.empty()) {
            // This counter represents the selected service entering the output
            // pipeline. Shared DVB receives the complete transponder, but after
            // PAT/PMT remap only this service's non-null packets count here.
            // Count before transcoding so "Bitrate In" remains an input metric.
            std::size_t selectedNonNullPackets = 0;
            for (const auto& packet : packets) {
                const std::uint16_t pid = static_cast<std::uint16_t>(
                    ((packet[1] & 0x1fU) << 8) | packet[2]);
                if (pid != 0x1fffU) ++selectedNonNullPackets;
            }
            selectedInputBytes_.fetch_add(
                selectedNonNullPackets * dvbstreamer5::media::mpegts::kPacketSize,
                std::memory_order_relaxed);
        }

        if (!packets.empty() && config_.observeMediaPmt)
            config_.observeMediaPmt(packets);

        if (!packets.empty() && (config_.transformTransport || config_.observeInputTransport || config_.observeDvbSubtitles)) {
            observedTransport.resize(packets.size() * dvbstreamer5::media::mpegts::kPacketSize);
            for (std::size_t index = 0; index < packets.size(); ++index) {
                std::memcpy(observedTransport.data() + index * dvbstreamer5::media::mpegts::kPacketSize,
                            packets[index].data(), dvbstreamer5::media::mpegts::kPacketSize);
            }
            if (config_.observeInputTransport) {
                config_.observeInputTransport(observedTransport.data(), observedTransport.size());
            }
            if (config_.observeDvbSubtitles) {
                config_.observeDvbSubtitles(observedTransport.data(), observedTransport.size());
            }
        }

        if (!packets.empty() && config_.transformTransport) {
            // observedTransport was already materialized above so that ABR taps and
            // the primary transcoder consume byte-identical post-remap/post-CA TS.
            std::vector<std::uint8_t> transformed;
            const auto diagTransformStart =
                diagnosticHttpRelay ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
            const bool transformOk = config_.transformTransport(
                observedTransport.data(), observedTransport.size(), transformed, error);
            if (diagnosticHttpRelay) {
                const auto transformMs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - diagTransformStart).count());
                diagRelayTransformMaxMs =
                    std::max(diagRelayTransformMaxMs, transformMs);
            }
            if (!transformOk) {
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastError_ = error.empty() ? "native transport transform failed" : error;
                running_.store(false, std::memory_order_release);
                break;
            }
            packets.clear();
            if (!transformed.empty()) {
                transformedFramer.push(transformed.data(), transformed.size(), packets);
            }
        }

        if (!packets.empty() && config_.injectDvbSubtitles)
            config_.injectDvbSubtitles(packets);

        if (!packets.empty() && config_.injectDvbEit)
            config_.injectDvbEit(packets);

        if (!packets.empty() && config_.outputSwitchClock) {
            // One timeline and consistent PID CC for UDP, SRT, HTTP and HLS.
            for (auto& packet : packets)
                config_.outputSwitchClock->apply(packet.data(), packet.size());
        }
        if (!packets.empty() && config_.capturePrimaryProgram &&
            config_.primaryProgramIdentity)
            config_.primaryProgramIdentity->observe(packets);
        if (!packets.empty()) {
            // Count the real MPEG-TS payload produced by the pipeline before
            // CBR null-packet stuffing and before duplicating it to outputs.
            std::size_t nonNullPacketCount = 0;
            for (const auto& packet : packets) {
                const std::uint16_t pid = static_cast<std::uint16_t>(
                    ((packet[1] & 0x1fU) << 8) | packet[2]);
                if (pid != 0x1fffU) ++nonNullPacketCount;
            }
            payloadOutputBytes_.fetch_add(
                nonNullPacketCount * dvbstreamer5::media::mpegts::kPacketSize,
                std::memory_order_relaxed);
            for (const auto& packet : packets) {
                const auto continuityStatus =
                    continuity.observe(packet.data(), packet.size());
                if (continuityStatus == dvbstreamer5::media::mpegts::ContinuityStatus::Gap ||
                    continuityStatus == dvbstreamer5::media::mpegts::ContinuityStatus::Duplicate ||
                    continuityStatus == dvbstreamer5::media::mpegts::ContinuityStatus::TransportError ||
                    continuityStatus == dvbstreamer5::media::mpegts::ContinuityStatus::InvalidPacket) {
                    continuityErrors_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (config_.observeTransport && !observePackets(packets)) {
                break;
            }

            for (auto& output : outputs) {
                if (output.cbrWorker) {
                    for (const auto& packet : packets) {
                        if (!output.cbrWorker->enqueue(packet)) {
                            error = "native UDP CBR input exceeded the bounded 2 MiB pacing queue";
                            break;
                        }
                    }
                    if (!error.empty()) {
                        std::lock_guard<std::mutex> lock(errorMutex_);
                        lastError_ = error;
                        break;
                    }
                } else if (output.vbrWorker) {
                    if (!output.vbrWorker->enqueue(packets)) {
                        error = "UDP-VBR source-PCR sender stopped";
                        std::lock_guard<std::mutex> lock(errorMutex_);
                        lastError_ = error;
                        break;
                    }
                    // The dedicated worker updates per-output and total bytes
                    // only after the physical UDP datagram has been sent.
                    continue;
                } else if (!flushPackets(
                        packets, output.rtp, output.packetizer.get(), *output.socket,
                        outputBytes_, error)) {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = error.empty() ? "UDP output send failed" : error;
                    break;
                } else {
                    if (output.sentTsBytes) {
                        output.sentTsBytes->fetch_add(
                            packets.size() * dvbstreamer5::media::mpegts::kPacketSize,
                            std::memory_order_relaxed);
                    }
                    ++output.datagramsSent;
                    if (!output.firstSendLogged) {
                        output.firstSendLogged = true;
                        std::cerr << "NATIVE UDP OUTPUT first_send index="
                                  << output.index
                                  << " type=" << output.type
                                  << " packets=" << packets.size()
                                  << std::endl;
                    }
                }
            }
            if (!error.empty()) {
                break;
            }
        }

        if (measureDvbPipeline) {
            const auto pipelineMs = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - pipelineStarted).count();
            if (pipelineMs > 50 && (++slowPipelineEvents <= 3 ||
                (slowPipelineEvents & (slowPipelineEvents - 1)) == 0)) {
                std::cerr << "NATIVE DVB RELAY PIPELINE SLOW stream="
                          << config_.streamId
                          << " packets=" << packets.size()
                          << " elapsed_ms=" << pipelineMs
                          << " occurrences=" << slowPipelineEvents
                          << std::endl;
            }
        }
        // UDP-CBR is drained by WisiCbrOutputWorker on its own absolute
        // CLOCK_MONOTONIC sender thread. Never send CBR datagrams here.

        if (diagnosticHttpRelay && diagRelayDidChunk) {
            const auto loopMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - diagRelayChunkStart).count());
            diagRelayLoopMaxMs = std::max(diagRelayLoopMaxMs, loopMs);
            if (loopMs > 100) ++diagRelayLoopsOver100Ms;
            if (++diagRelayChunks >= 125) {
                std::cerr << "NATIVE HTTP RELAY CADENCE"
                          << " stream_id=" << config_.streamId
                          << " chunks=" << diagRelayChunks
                          << " bytes=" << diagRelayBytes
                          << " relay_gap_max_ms=" << diagRelayGapMaxMs
                          << " relay_gaps_over_200ms="
                          << diagRelayGapsOver200Ms
                          << " transform_max_ms=" << diagRelayTransformMaxMs
                          << " loop_max_ms=" << diagRelayLoopMaxMs
                          << " loops_over_100ms=" << diagRelayLoopsOver100Ms
                          << " queue_peak_bytes=" << diagRelayQueuePeakBytes
                          << std::endl;
                diagRelayChunks = 0;
                diagRelayBytes = 0;
                diagRelayGapMaxMs = 0;
                diagRelayGapsOver200Ms = 0;
                diagRelayTransformMaxMs = 0;
                diagRelayLoopMaxMs = 0;
                diagRelayLoopsOver100Ms = 0;
                diagRelayQueuePeakBytes = 0;
            }
        }
    }

    if(vbrObserverWorker) vbrObserverWorker->stop();
    for(auto& output:outputs)
        if(output.vbrWorker) output.vbrWorker->stop();
    {
        std::lock_guard<std::mutex> lock(observedCbrMutex);
        observedCbrStop = true;
    }
    observedCbrCondition.notify_all();
    if (observedCbrWorker.joinable()) {
        observedCbrWorker.join();
    }

    running_.store(false, std::memory_order_release);
    httpQueueCondition_.notify_all();
    if (fileInput_.is_open()) {
        fileInput_.close();
    }
    fileInputSource_ = false;
    dvbInputSource_ = false;
    dvbInput_.close();
}

} // namespace dvbstreamer5::media::network
