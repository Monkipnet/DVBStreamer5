#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dvbstreamer5::http {

using Headers = std::vector<std::pair<std::string, std::string>>;
using ContentReceiver = std::function<bool(const std::uint8_t*, std::size_t)>;

struct RequestOptions {
    Headers headers;
    std::string userAgent = "Mozilla/5.0 DVBStreamer5";
    long connectTimeoutMs = 3000;
    long readTimeoutMs = 12000;
    long writeTimeoutMs = 3000;
    long totalTimeoutMs = 0;
    int maxRedirects = 8;
    bool verifyTlsPeer = true;
    bool keepAlive = false;
    bool forwardHeadersAcrossOrigins = false;
    std::size_t maxBodyBytes = 64U * 1024U * 1024U;
    std::atomic<bool>* stopping = nullptr;
};

struct Response {
    int status = 0;
    std::string effectiveUrl;
    std::string contentType;
    std::vector<std::uint8_t> body;
};

std::string encodeQueryComponent(const std::string& value);

bool get(const std::string& url,
         const RequestOptions& options,
         Response& response,
         std::string& error,
         const ContentReceiver& receiver = {});

} // namespace dvbstreamer5::http
