#include "media/NativeUdpRelay.h"

#include "media/CbrTsPacer.h"
#include "media/RtpMpegTs.h"
#include "media/TransportStream.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <climits>
#include <iostream>
#include <limits>
#include <regex>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace tvs::media::network {
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
    const std::vector<tvs::media::mpegts::Packet>& packets,
    bool rtpOutput,
    tvs::media::rtp::MpegTsPacketizer& packetizer,
    UdpSocket& output,
    std::atomic<std::uint64_t>& outputBytes,
    std::string& error) {
    if (packets.empty()) return true;

    if (rtpOutput) {
        std::vector<std::vector<std::uint8_t>> datagrams;
        if (!packetizer.packetize(packets, currentRtpTimestamp(), datagrams)) {
            error = "failed to packetize MPEG-TS RTP output";
            return false;
        }
        for (const auto& datagram : datagrams) {
            if (!output.send(datagram.data(), datagram.size(), error)) return false;
        }
    } else {
        constexpr std::size_t kPacketsPerDatagram = 7;
        for (std::size_t first = 0; first < packets.size(); first += kPacketsPerDatagram) {
            const std::size_t count =
                (std::min)(kPacketsPerDatagram, packets.size() - first);
            std::array<std::uint8_t, kPacketsPerDatagram * tvs::media::mpegts::kPacketSize> bytes {};
            for (std::size_t index = 0; index < count; ++index) {
                std::copy(
                    packets[first + index].begin(),
                    packets[first + index].end(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(
                        index * tvs::media::mpegts::kPacketSize));
            }
            const std::size_t size = count * tvs::media::mpegts::kPacketSize;
            if (!output.send(bytes.data(), size, error)) return false;
        }
    }

    outputBytes.fetch_add(
        packets.size() * tvs::media::mpegts::kPacketSize, std::memory_order_relaxed);
    return true;
}

bool sendCbrDatagram(
    const tvs::media::mpegts::CbrDatagram& packets,
    UdpSocket& output,
    std::atomic<std::uint64_t>& outputBytes,
    std::string& error) {
    std::array<std::uint8_t,
        tvs::media::mpegts::kPacketsPerCbrDatagram *
            tvs::media::mpegts::kPacketSize> bytes {};
    for (std::size_t index = 0; index < packets.size(); ++index) {
        std::copy(
            packets[index].begin(),
            packets[index].end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(
                index * tvs::media::mpegts::kPacketSize));
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
    if (config_.outputs.empty()) {
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
    {
        std::lock_guard<std::mutex> lock(httpQueueMutex_);
        httpQueue_.clear();
        httpQueuedBytes_ = 0;
        httpFinished_ = false;
    }
    inputBytes_.store(0, std::memory_order_relaxed);
    outputBytes_.store(0, std::memory_order_relaxed);
    continuityErrors_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_.clear();
    }

    UdpInputEndpoint input;
    const bool networkInput = parseInputEndpoint(config.inputUri, input);
    httpInputSource_ = !networkInput && isHttpInput(config.inputUri);
    std::string filePath;
    fileInputSource_ = !networkInput && !httpInputSource_ &&
        resolveFileInputPath(config.inputUri, filePath);
    if (!networkInput && !httpInputSource_ && !fileInputSource_) {
        error = "native relay requires a valid UDP/RTP, HTTP(S), or local file input";
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
    } else if (!httpInputSource_) {
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

    outputSockets_.clear();
    for (const auto& output : config_.outputs) {
        std::string outputHost;
        std::uint16_t parsedOutputPort = 0;
        if (!parseOutputEndpoint(
                output.outputHost, output.outputPort, outputHost, parsedOutputPort)) {
            inputSocket_.close();
            fileInput_.close();
            outputSockets_.clear();
            error = "native UDP relay output endpoint is invalid";
            return false;
        }
        auto outputSocket = std::make_unique<UdpSocket>();
        if (!outputSocket->openSender(
                outputHost, parsedOutputPort, output.interfaceAddress, error)) {
            inputSocket_.close();
            fileInput_.close();
            outputSockets_.clear();
            error = "native UDP output setup failed: " + error;
            return false;
        }
        outputSockets_.push_back(std::move(outputSocket));
    }

    running_.store(true, std::memory_order_release);
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

std::uint64_t NativeUdpRelay::outputBytes() const noexcept {
    return outputBytes_.load(std::memory_order_relaxed);
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
        httpQueueCondition_.wait(lock, [this, chunkSize] {
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

std::size_t NativeUdpRelay::curlWrite(
    char* data, std::size_t size, std::size_t count, void* userData) {
    auto* relay = static_cast<NativeUdpRelay*>(userData);
    if (!relay || (size != 0 && count >
            (std::numeric_limits<std::size_t>::max)() / size)) {
        return 0;
    }
    const std::size_t bytes = size * count;
    if (bytes == 0) {
        return 0;
    }
    return relay->enqueueHttpData(
        reinterpret_cast<const std::uint8_t*>(data), bytes)
        ? bytes
        : 0;
}

int NativeUdpRelay::curlProgress(
    void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* relay = static_cast<const NativeUdpRelay*>(userData);
    return !relay || !relay->running_.load(std::memory_order_acquire) ? 1 : 0;
}

void NativeUdpRelay::runHttpInput() {
    static std::once_flag curlInitialization;
    static CURLcode initializationResult = CURLE_FAILED_INIT;
    std::call_once(curlInitialization, [] {
        initializationResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    if (initializationResult != CURLE_OK) {
        finishHttpInput("native HTTP input: curl_global_init failed");
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        finishHttpInput("native HTTP input: curl_easy_init failed");
        return;
    }

    std::string location = config_.inputUri;
    struct curl_slist* headers = nullptr;
    auto appendHeader = [&headers](const std::string& value) {
        struct curl_slist* updated = curl_slist_append(headers, value.c_str());
        if (!updated) {
            return false;
        }
        headers = updated;
        return true;
    };
    auto cleanup = [&headers, curl] {
        if (headers) {
            curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curl);
    };
    if (config_.accessKeyMode == "header" &&
        !config_.accessKeyName.empty() && !config_.accessKeyValue.empty()) {
        if (config_.accessKeyName.find_first_of("\r\n") != std::string::npos ||
            config_.accessKeyValue.find_first_of("\r\n") != std::string::npos) {
            cleanup();
            finishHttpInput("native HTTP input: access header contains a line break");
            return;
        }
        const std::string header =
            config_.accessKeyName + ": " + config_.accessKeyValue;
        if (!appendHeader(header)) {
            cleanup();
            finishHttpInput("native HTTP input: failed to allocate access header");
            return;
        }
    } else if (config_.accessKeyMode == "query" &&
        !config_.accessKeyName.empty() && !config_.accessKeyValue.empty()) {
        if (config_.accessKeyName.size() > static_cast<std::size_t>(INT_MAX) ||
            config_.accessKeyValue.size() > static_cast<std::size_t>(INT_MAX)) {
            cleanup();
            finishHttpInput("native HTTP input: access query value is too large");
            return;
        }
        char* name = curl_easy_escape(
            curl, config_.accessKeyName.c_str(),
            static_cast<int>(config_.accessKeyName.size()));
        char* value = curl_easy_escape(
            curl, config_.accessKeyValue.c_str(),
            static_cast<int>(config_.accessKeyValue.size()));
        if (!name || !value) {
            if (name) curl_free(name);
            if (value) curl_free(value);
            cleanup();
            finishHttpInput("native HTTP input: failed to encode access query");
            return;
        }
        const std::string key = std::string(name) + "=";
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
        curl_free(name);
        curl_free(value);
    }
    if (!appendHeader("Accept: */*") ||
        !appendHeader("Accept-Encoding: identity")) {
        cleanup();
        finishHttpInput("native HTTP input: failed to allocate request headers");
        return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, location.c_str());
    const bool hasAccessKey =
        !config_.accessKeyName.empty() && !config_.accessKeyValue.empty() &&
        (config_.accessKeyMode == "header" ||
            config_.accessKeyMode == "query");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, hasAccessKey ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &NativeUdpRelay::curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &NativeUdpRelay::curlProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        config_.userAgent.empty()
            ? "Mozilla/5.0 DVBStreamer5"
            : config_.userAgent.c_str());
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    const CURLcode result = curl_easy_perform(curl);
    long responseCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    const bool cancelled = !running_.load(std::memory_order_acquire);
    std::string error;
    if (!cancelled &&
        (result != CURLE_OK || responseCode < 200 || responseCode >= 300)) {
        error = std::string("native HTTP input failed: ") +
            (result == CURLE_OK
                ? "unexpected HTTP response"
                : curl_easy_strerror(result)) +
            " (HTTP " + std::to_string(responseCode) + ")";
    }
    cleanup();
    finishHttpInput(error);
}

void NativeUdpRelay::run() {
    struct OutputWorker {
        UdpSocket* socket = nullptr;
        bool rtp = false;
        std::unique_ptr<tvs::media::rtp::MpegTsPacketizer> packetizer;
        std::unique_ptr<tvs::media::mpegts::CbrTsPacer> cbrPacer;
    };

    UdpInputEndpoint inputEndpoint;
    const bool networkInput = parseInputEndpoint(config_.inputUri, inputEndpoint);
    const bool httpInput = httpInputSource_;
    if (!networkInput && !httpInput && !fileInputSource_) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = "native input endpoint became invalid";
        running_.store(false, std::memory_order_release);
        httpQueueCondition_.notify_all();
        return;
    }

    const bool rtpInput = networkInput && inputEndpoint.scheme == "rtp";
    bool fileInputEof = false;
    bool fileHadTsPackets = false;
    tvs::media::mpegts::PacketFramer framer;
    tvs::media::mpegts::ContinuityTracker continuity;
    std::vector<tvs::media::mpegts::Packet> packets;
    std::vector<OutputWorker> outputs;
    outputs.reserve(config_.outputs.size());
    auto seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (std::size_t index = 0; index < config_.outputs.size(); ++index) {
        OutputWorker output;
        output.socket = outputSockets_[index].get();
        output.rtp = config_.outputs[index].outputType == "rtp";
        if (output.rtp) {
            const auto outputSeed = seed + index;
            const std::uint32_t sourceId =
                static_cast<std::uint32_t>(outputSeed ^ (outputSeed >> 32));
            output.packetizer = std::make_unique<tvs::media::rtp::MpegTsPacketizer>(
                sourceId,
                static_cast<std::uint16_t>(outputSeed >> 16),
                std::uint8_t{33},
                std::size_t{7});
        }
        if (config_.outputs[index].outputType == "udp-cbr") {
            try {
                output.cbrPacer = std::make_unique<tvs::media::mpegts::CbrTsPacer>(
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
                            tvs::media::mpegts::kPacketsPerCbrDatagram;
                });
            if (!fileInputEof && needsFileData) {
                constexpr std::size_t kFileReadSize =
                    tvs::media::mpegts::kPacketsPerCbrDatagram *
                    tvs::media::mpegts::kPacketSize;
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
                    framer.push(datagram.data(), received, packets);
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
                received = chunk.size();
                std::copy(chunk.begin(), chunk.end(), datagram.begin());
                lock.unlock();
                httpQueueCondition_.notify_all();
                inputBytes_.fetch_add(received, std::memory_order_relaxed);
                framer.push(datagram.data(), received, packets);
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
            if (!inputSocket_.receive(
                    datagram.data(), datagram.size(), received, receiveTimeoutMs, error)) {
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = error.empty() ? "UDP receive failed" : error;
                }
                break;
            }

            if (received > 0) {
                inputBytes_.fetch_add(received, std::memory_order_relaxed);
                if (rtpInput) {
                    tvs::media::rtp::PacketView rtp;
                    if (!tvs::media::rtp::parsePacket(datagram.data(), received, rtp) ||
                        !tvs::media::rtp::decodeMpegTsPayload(rtp, packets)) {
                        continuityErrors_.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                } else {
                    framer.push(datagram.data(), received, packets);
                }
            }
        }

        if (!packets.empty()) {
            for (const auto& packet : packets) {
                const auto continuityStatus =
                    continuity.observe(packet.data(), packet.size());
                if (continuityStatus == tvs::media::mpegts::ContinuityStatus::Gap ||
                    continuityStatus == tvs::media::mpegts::ContinuityStatus::Duplicate ||
                    continuityStatus == tvs::media::mpegts::ContinuityStatus::TransportError ||
                    continuityStatus == tvs::media::mpegts::ContinuityStatus::InvalidPacket) {
                    continuityErrors_.fetch_add(1, std::memory_order_relaxed);
                }
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
                        packets, output.rtp, *output.packetizer, *output.socket,
                        outputBytes_, error)) {
                    std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = error.empty() ? "UDP output send failed" : error;
                    break;
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
            tvs::media::mpegts::CbrDatagram cbrDatagram {};
            if (output.cbrPacer->nextDatagram(
                    std::chrono::steady_clock::now(), cbrDatagram) &&
                !sendCbrDatagram(cbrDatagram, *output.socket, outputBytes_, error)) {
                std::lock_guard<std::mutex> lock(errorMutex_);
                    lastError_ = error.empty() ? "UDP CBR output send failed" : error;
                break;
            }
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
}

} // namespace tvs::media::network
