#include "media/NativeUdpRelay.h"

#include "NativeHttpClient.h"
#include "media/CbrTsPacer.h"
#include "media/RtpMpegTs.h"
#include "media/TransportStream.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iostream>
#include <limits>
#include <regex>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

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

bool sendCbrDatagram(
    const dvbstreamer5::media::mpegts::CbrDatagram& packets,
    UdpSocket& output,
    std::atomic<std::uint64_t>& outputBytes,
    std::string& error) {
    std::array<std::uint8_t,
        dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram *
            dvbstreamer5::media::mpegts::kPacketSize> bytes {};
    for (std::size_t index = 0; index < packets.size(); ++index) {
        std::copy(
            packets[index].begin(),
            packets[index].end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(
                index * dvbstreamer5::media::mpegts::kPacketSize));
    }
    if (!output.send(bytes.data(), bytes.size(), error)) return false;
    outputBytes.fetch_add(bytes.size(), std::memory_order_relaxed);
    return true;
}

std::string inputInterfaceFor(const NativeUdpRelayConfig& config, bool multicastOrWildcard) {
    if (config.inputInterfaceAddressConfigured) {
        return config.inputInterfaceAddress;
    }
    return multicastOrWildcard ? config.interfaceAddress : std::string();
}

} // namespace

NativeUdpRelay::~NativeUdpRelay() {
    stop();
}

bool NativeUdpRelay::start(const NativeUdpRelayConfig& config, std::string& error) {
    stop();
    config_ = config;
    if (config_.outputs.empty() && !config_.allowNoNetworkOutput) {
        config_.outputs.push_back({
            config.outputType,
            config.outputHost,
            config.outputPort,
            config.interfaceAddress});
    }
    fileInput_.close();
    fileInput_.clear();
    fileInputSource_ = false;
    httpInputSource_ = false;
    externalInputSource_ = false;
    dvbInputSource_ = false;
    {
        std::lock_guard<std::mutex> lock(httpQueueMutex_);
        httpQueue_.clear();
        httpQueuedBytes_ = 0;
        httpFinished_ = false;
    }
    inputBytes_.store(0, std::memory_order_relaxed);
    httpReceivedBytes_.store(0, std::memory_order_relaxed);
    outputBytes_.store(0, std::memory_order_relaxed);
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
    if (config_.remapEnabled &&
        !remapper_.initialize(config_.remapConfig, error)) {
        return false;
    }
    if (fileInputSource_ &&
        std::any_of(config_.outputs.begin(), config_.outputs.end(),
            [](const NativeUdpRelayOutputConfig& output) {
                return output.outputType != "udp-cbr";
            })) {
        fileInputSource_ = false;
        error = "native file input currently supports only UDP CBR outputs";
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
        fileInput_.open(filePath, std::ios::binary);
        if (!fileInput_.is_open()) {
            fileInputSource_ = false;
            error = "native file input could not be opened: " + filePath;
            return false;
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

void NativeUdpRelay::stop() noexcept {
    httpStopRequested_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    httpQueueCondition_.notify_all();
    if (httpWorker_.joinable() && httpWorker_.get_id() != std::this_thread::get_id()) {
        httpWorker_.join();
    }
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
    inputSocket_.close();
    fileInput_.close();
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
        httpQueuedBytes_ = 0;
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

std::uint64_t NativeUdpRelay::outputBytes() const noexcept {
    return outputBytes_.load(std::memory_order_relaxed);
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
        const std::size_t chunkSize =
            (std::min)(kMaximumChunkBytes, size - offset);
        if (chunkSize > kMaximumHttpQueueBytes) {
            return false;
        }
        std::vector<std::uint8_t> chunk(
            data + offset, data + offset + chunkSize);
        std::unique_lock<std::mutex> lock(httpQueueMutex_);
        httpQueueCondition_.wait(lock, [this, chunkSize, kMaximumHttpQueueBytes] {
            return !running_.load(std::memory_order_acquire) ||
                httpQueuedBytes_ + chunkSize <= kMaximumHttpQueueBytes;
        });
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        httpQueuedBytes_ += chunk.size();
        httpQueue_.push_back(std::move(chunk));
        lock.unlock();
        httpQueueCondition_.notify_all();
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
            [this, &receivedAny, &receivedBytes, attempt](const std::uint8_t* data, std::size_t size) {
                if (size != 0 && !receivedAny) {
                    receivedAny = true;
                    std::cerr << "NATIVE HTTP INPUT connected attempt=" << attempt
                              << " first_chunk=" << size << std::endl;
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_.clear();
                }
                receivedBytes += size;
                httpReceivedBytes_.fetch_add(size, std::memory_order_relaxed);
                return enqueueHttpData(data, size);
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
        std::unique_ptr<dvbstreamer5::media::rtp::MpegTsPacketizer> packetizer;
        std::unique_ptr<dvbstreamer5::media::mpegts::CbrTsPacer> cbrPacer;
    };

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
    std::vector<std::uint8_t> observedTransport;
    std::vector<OutputWorker> outputs;
    outputs.reserve(config_.outputs.size());
    auto seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (std::size_t index = 0; index < config_.outputs.size(); ++index) {
        OutputWorker output;
        output.socket = outputSockets_[index].get();
        output.index = index;
        output.type = config_.outputs[index].outputType;
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
        if (config_.outputs[index].outputType == "udp-cbr") {
            try {
                output.cbrPacer = std::make_unique<dvbstreamer5::media::mpegts::CbrTsPacer>(
                    config_.targetBitrate);
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

    // observeTransport consumers such as SRT/RTSP/RTMP receive the already
    // muxed transport stream.  Do not put that stream through CbrTsPacer:
    // the transcoder mux has already inserted the configured CBR null packets,
    // and a second packet queue both double-stuffs the stream and can overflow
    // after timestamp catch-up bursts.  Instead apply wall-clock backpressure
    // directly while delivering the existing packets.
    bool observedPacingStarted = false;
    std::chrono::steady_clock::time_point observedNextDeadline {};
    std::uint64_t observedPacingRemainder = 0;

    auto observePackets = [&](
        const std::vector<dvbstreamer5::media::mpegts::Packet>& observedPackets) -> bool {
        if (!config_.observeTransport || observedPackets.empty()) return true;

        constexpr std::size_t kObservedPacketsPerBatch = 7;
        constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
        const bool paced = config_.paceObservedTransport && config_.targetBitrate > 0;

        for (std::size_t first = 0; first < observedPackets.size();
             first += kObservedPacketsPerBatch) {
            if (!running_.load(std::memory_order_acquire)) return false;

            const std::size_t count = (std::min)(
                kObservedPacketsPerBatch, observedPackets.size() - first);

            if (paced) {
                auto now = std::chrono::steady_clock::now();
                if (!observedPacingStarted) {
                    observedPacingStarted = true;
                    observedNextDeadline = now;
                }
                if (observedNextDeadline > now) {
                    std::this_thread::sleep_until(observedNextDeadline);
                    now = std::chrono::steady_clock::now();
                }
                // If the process was suspended or the source jumped far ahead,
                // do not attempt a long high-speed catch-up burst.
                if (now - observedNextDeadline > std::chrono::milliseconds(250)) {
                    observedNextDeadline = now;
                    observedPacingRemainder = 0;
                }
            }

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

            if (paced) {
                const std::uint64_t bits =
                    static_cast<std::uint64_t>(count) *
                    dvbstreamer5::media::mpegts::kPacketSize * 8ULL;
                const std::uint64_t numerator =
                    bits * kNanosecondsPerSecond + observedPacingRemainder;
                const std::uint64_t nanoseconds = numerator / config_.targetBitrate;
                observedPacingRemainder = numerator % config_.targetBitrate;
                observedNextDeadline += std::chrono::nanoseconds(nanoseconds);
            }
        }
        return true;
    };

    std::array<std::uint8_t, 65536> datagram {};
    std::string error;

    while (running_.load(std::memory_order_acquire)) {
        std::size_t received = 0;
        packets.clear();
        if (fileInputSource_) {
            const bool needsFileData = std::any_of(
                outputs.begin(), outputs.end(), [](const OutputWorker& output) {
                    return output.cbrPacer &&
                        output.cbrPacer->queuedPackets() <
                            dvbstreamer5::media::mpegts::kPacketsPerCbrDatagram;
                });
            if (!fileInputEof && needsFileData) {
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
                }
            }
            if (fileInputEof && packets.empty() && std::all_of(
                    outputs.begin(), outputs.end(), [](const OutputWorker& output) {
                        return !output.cbrPacer || output.cbrPacer->queuedPackets() == 0;
                    })) {
                if (!fileHadTsPackets) {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ =
                        "native file input contains no complete MPEG-TS packets";
                }
                break;
            }
            if (received == 0) {
                std::chrono::steady_clock::time_point nextDeadline {};
                bool hasDeadline = false;
                for (const auto& output : outputs) {
                    if (output.cbrPacer && output.cbrPacer->started()) {
                        const auto outputDeadline = output.cbrPacer->nextDeadline();
                        if (!hasDeadline || outputDeadline < nextDeadline) {
                            nextDeadline = outputDeadline;
                            hasDeadline = true;
                        }
                    }
                }
                if (hasDeadline &&
                    nextDeadline > std::chrono::steady_clock::now()) {
                    std::this_thread::sleep_until(nextDeadline);
                }
            }
        } else if (httpInput) {
            int receiveTimeoutMs = 250;
            auto nextDeadline = std::chrono::steady_clock::time_point {};
            bool hasDeadline = false;
            for (const auto& output : outputs) {
                if (output.cbrPacer && output.cbrPacer->started()) {
                    const auto deadline = output.cbrPacer->nextDeadline();
                    if (!hasDeadline || deadline < nextDeadline) {
                        nextDeadline = deadline;
                        hasDeadline = true;
                    }
                }
            }
            std::unique_lock<std::mutex> lock(httpQueueMutex_);
            if (httpQueue_.empty()) {
                const bool cbrDrained = std::all_of(
                    outputs.begin(), outputs.end(), [](const OutputWorker& output) {
                        return !output.cbrPacer ||
                            output.cbrPacer->queuedPackets() == 0;
                    });
                if (httpFinished_ && cbrDrained) {
                    break;
                }
                if (hasDeadline) {
                    if (httpFinished_) {
                        httpQueueCondition_.wait_until(lock, nextDeadline);
                    } else {
                        httpQueueCondition_.wait_until(
                            lock, nextDeadline, [this] {
                                return !httpQueue_.empty() || httpFinished_ ||
                                    !running_.load(std::memory_order_acquire);
                            });
                    }
                } else if (!httpFinished_) {
                    httpQueueCondition_.wait_for(
                        lock, std::chrono::milliseconds(receiveTimeoutMs), [this] {
                            return !httpQueue_.empty() || httpFinished_ ||
                                !running_.load(std::memory_order_acquire);
                        });
                }
            }
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            if (!httpQueue_.empty()) {
                auto chunk = std::move(httpQueue_.front());
                httpQueue_.pop_front();
                httpQueuedBytes_ -= chunk.size();
                lock.unlock();
                httpQueueCondition_.notify_all();

                if (chunk.empty()) {
                    // HTTP reconnect boundary.  Do not join an incomplete TS
                    // packet from the previous response with bytes from the
                    // next response.  Continuity counters are also expected to
                    // jump when a live origin reconnects.
                    inputFramer.reset();
                    transformedFramer.reset();
                    continuity.reset();
                    continue;
                }

                received = chunk.size();
                std::copy(chunk.begin(), chunk.end(), datagram.begin());
                inputBytes_.fetch_add(received, std::memory_order_relaxed);
                inputFramer.push(datagram.data(), received, packets);
            } else if (httpFinished_) {
                const bool cbrDrained = std::all_of(
                    outputs.begin(), outputs.end(), [](const OutputWorker& output) {
                        return !output.cbrPacer ||
                            output.cbrPacer->queuedPackets() == 0;
                    });
                if (cbrDrained) {
                    break;
                }
            }
        } else {
            int receiveTimeoutMs = 250;
            for (const auto& output : outputs) {
                if (!output.cbrPacer || !output.cbrPacer->started()) {
                    continue;
                }
                const auto remaining = output.cbrPacer->nextDeadline() -
                    std::chrono::steady_clock::now();
                const int outputTimeout =
                    remaining <= std::chrono::steady_clock::duration::zero()
                    ? 0
                    : static_cast<int>((std::min)(
                        std::int64_t{250},
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            remaining + std::chrono::milliseconds(1)).count()));
                receiveTimeoutMs = (std::min)(receiveTimeoutMs, outputTimeout);
            }
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
            std::array<std::uint8_t,
                kCaBatchPackets * dvbstreamer5::media::mpegts::kPacketSize> caBatch {};
            for (std::size_t first = 0; first < packets.size(); first += kCaBatchPackets) {
                const std::size_t count = (std::min)(
                    kCaBatchPackets, packets.size() - first);
                const std::size_t bytes =
                    count * dvbstreamer5::media::mpegts::kPacketSize;
                for (std::size_t index = 0; index < count; ++index) {
                    std::memcpy(
                        caBatch.data() + index * dvbstreamer5::media::mpegts::kPacketSize,
                        packets[first + index].data(),
                        dvbstreamer5::media::mpegts::kPacketSize);
                }
                if (!config_.processTransport(caBatch.data(), bytes)) {
                    error = "native conditional-access transport processing failed";
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = error;
                    running_.store(false, std::memory_order_release);
                    break;
                }
                for (std::size_t index = 0; index < count; ++index) {
                    std::memcpy(
                        packets[first + index].data(),
                        caBatch.data() + index * dvbstreamer5::media::mpegts::kPacketSize,
                        dvbstreamer5::media::mpegts::kPacketSize);
                }
            }
            if (!running_.load(std::memory_order_acquire)) break;
        }

        if (!packets.empty() && (config_.transformTransport || config_.observeInputTransport)) {
            observedTransport.resize(packets.size() * dvbstreamer5::media::mpegts::kPacketSize);
            for (std::size_t index = 0; index < packets.size(); ++index) {
                std::memcpy(observedTransport.data() + index * dvbstreamer5::media::mpegts::kPacketSize,
                            packets[index].data(), dvbstreamer5::media::mpegts::kPacketSize);
            }
            if (config_.observeInputTransport) {
                config_.observeInputTransport(observedTransport.data(), observedTransport.size());
            }
        }

        if (!packets.empty() && config_.transformTransport) {
            // observedTransport was already materialized above so that ABR taps and
            // the primary transcoder consume byte-identical post-remap/post-CA TS.
            std::vector<std::uint8_t> transformed;
            if (!config_.transformTransport(observedTransport.data(), observedTransport.size(), transformed, error)) {
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
                if (output.cbrPacer) {
                    for (const auto& packet : packets) {
                        if (!output.cbrPacer->enqueue(packet)) {
                            error = "native UDP CBR input exceeded the bounded 2 MiB pacing queue";
                            break;
                        }
                    }
                    if (!error.empty()) {
                        std::lock_guard<std::mutex> lock(errorMutex_);
                        lastError_ = error;
                        break;
                    }
                } else if (!flushPackets(
                        packets, output.rtp, output.packetizer.get(), *output.socket,
                        outputBytes_, error)) {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = error.empty() ? "UDP output send failed" : error;
                    break;
                } else {
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

        for (auto& output : outputs) {
            if (!output.cbrPacer || !output.cbrPacer->started()) {
                continue;
            }

            // Drain every datagram whose pacing deadline has already arrived.
            // The previous code emitted at most one CBR datagram per input-loop
            // iteration, so an asynchronous transcoder could make UDP-CBR lag
            // badly behind the requested bitrate.
            for (unsigned drained = 0; drained < 32U; ++drained) {
                dvbstreamer5::media::mpegts::CbrDatagram cbrDatagram {};
                if (!output.cbrPacer->nextDatagram(
                        std::chrono::steady_clock::now(), cbrDatagram)) {
                    break;
                }
                if (!sendCbrDatagram(
                        cbrDatagram, *output.socket, outputBytes_, error)) {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = error.empty()
                        ? "UDP CBR output send failed" : error;
                    break;
                }
                ++output.datagramsSent;
                if (!output.firstSendLogged) {
                    output.firstSendLogged = true;
                    std::cerr << "NATIVE UDP OUTPUT first_send index="
                              << output.index
                              << " type=" << output.type
                              << " queued_packets="
                              << output.cbrPacer->queuedPackets()
                              << " target_kbps="
                              << (output.cbrPacer->targetBitrate() / 1000ULL)
                              << std::endl;
                }
            }
            if (!error.empty()) break;
        }
        if (!error.empty()) {
            std::lock_guard<std::mutex> lock(errorMutex_);
            lastError_ = error;
            break;
        }
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
