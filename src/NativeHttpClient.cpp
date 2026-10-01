#include "NativeHttpClient.h"

#include <cpp-httplib/httplib.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace dvbstreamer5::http {
namespace {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string origin;
    std::string target;
};

bool parseUrl(const std::string& url, ParsedUrl& parsed) {
    httplib::detail::UrlComponents components;
    if (!httplib::detail::parse_url(url, components) || components.host.empty() ||
        (components.scheme != "http" && components.scheme != "https")) {
        return false;
    }

    parsed.scheme = components.scheme;
    parsed.host = components.host;
    parsed.port = components.port;
    const bool ipv6 = parsed.host.find(':') != std::string::npos;
    parsed.origin = parsed.scheme + "://" + (ipv6 ? "[" + parsed.host + "]" : parsed.host);
    if (!parsed.port.empty()) parsed.origin += ":" + parsed.port;
    parsed.target = components.path.empty() ? "/" : components.path;
    parsed.target += components.query;
    return true;
}

std::string normalizedPath(std::string path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos
            ? std::string::npos : slash - start);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string result = "/";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) result += '/';
        result += parts[i];
    }
    return result;
}

std::string resolveRedirect(const std::string& base, const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    ParsedUrl parsed;
    if (!parseUrl(base, parsed)) return {};
    if (location.rfind("//", 0) == 0) return parsed.scheme + ":" + location;
    if (location.empty()) return base;
    if (location.front() == '/') return parsed.origin + location;
    if (location.front() == '?') {
        const auto query = parsed.target.find('?');
        return parsed.origin + parsed.target.substr(0, query) + location;
    }
    if (location.front() == '#') return parsed.origin + parsed.target + location;
    const auto query = parsed.target.find('?');
    std::string path = parsed.target.substr(0, query);
    const auto slash = path.rfind('/');
    const auto suffixPosition = location.find_first_of("?#");
    const std::string relativePath = location.substr(0, suffixPosition);
    const std::string suffix = suffixPosition == std::string::npos
        ? std::string()
        : location.substr(suffixPosition);
    path = (slash == std::string::npos ? "/" : path.substr(0, slash + 1)) + relativePath;
    return parsed.origin + normalizedPath(path) + suffix;
}

bool isRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::chrono::milliseconds timeout(long value, long fallback) {
    return std::chrono::milliseconds(value > 0 ? value : fallback);
}

} // namespace

std::string encodeQueryComponent(const std::string& value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded << static_cast<char>(ch);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(ch);
        }
    }
    return encoded.str();
}

bool get(const std::string& url,
         const RequestOptions& options,
         Response& response,
         std::string& error,
         const ContentReceiver& receiver) {
    response = {};
    error.clear();
    std::string currentUrl = url;
    Headers requestHeaders = options.headers;
    std::string previousOrigin;
    const auto started = std::chrono::steady_clock::now();
    const auto totalTimedOut = [&] {
        return options.totalTimeoutMs > 0 &&
            std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(options.totalTimeoutMs);
    };

    if (options.userAgent.find_first_of("\r\n") != std::string::npos) {
        error = "HTTP user agent contains a line break";
        return false;
    }
    for (const auto& header : requestHeaders) {
        if (header.first.empty() ||
            header.first.find_first_of("\r\n") != std::string::npos ||
            header.second.find_first_of("\r\n") != std::string::npos) {
            error = "HTTP header contains an invalid name or line break";
            return false;
        }
    }

    for (int redirect = 0; redirect <= std::max(0, options.maxRedirects); ++redirect) {
        if (totalTimedOut()) {
            error = "request timeout";
            return false;
        }
        if (options.stopping && options.stopping->load(std::memory_order_relaxed)) {
            error = "request cancelled";
            return false;
        }

        ParsedUrl parsed;
        if (!parseUrl(currentUrl, parsed)) {
            error = "unsupported or invalid HTTP URL: " + currentUrl;
            return false;
        }
        if (!previousOrigin.empty() && parsed.origin != previousOrigin &&
            !options.forwardHeadersAcrossOrigins) {
            requestHeaders.clear();
        }

        httplib::Client client(parsed.origin);
        if (!client.is_valid()) {
            error = "failed to create HTTP client for " + parsed.origin;
            return false;
        }
        client.set_follow_location(false);
        // The URL parser already gives us the caller's encoded request target.
        // Re-encoding here would change RFC 3986 query escapes (for example
        // `%20` into `+`) and can break signed HLS URLs.
        client.set_path_encode(false);
        client.set_keep_alive(options.keepAlive);
        client.set_connection_timeout(timeout(options.connectTimeoutMs, 3000));
        client.set_read_timeout(timeout(options.readTimeoutMs, 12000));
        client.set_write_timeout(timeout(options.writeTimeoutMs, 3000));
        client.enable_server_certificate_verification(options.verifyTlsPeer);

        httplib::Headers headers;
        headers.emplace("Accept", "*/*");
        headers.emplace("Accept-Encoding", "identity");
        if (!options.userAgent.empty()) headers.emplace("User-Agent", options.userAgent);
        for (const auto& header : requestHeaders) headers.emplace(header.first, header.second);

        int status = 0;
        std::string contentType;
        std::string location;
        std::size_t receivedBytes = 0;
        bool callbackStopped = false;
        std::vector<std::uint8_t> body;

        auto responseHandler = [&](const httplib::Response& incoming) {
            status = incoming.status;
            contentType = incoming.get_header_value("Content-Type");
            location = incoming.get_header_value("Location");
            return true;
        };
        auto contentHandler = [&](const char* data, std::size_t size) {
            if (totalTimedOut()) {
                error = "request timeout";
                callbackStopped = true;
                return false;
            }
            if (options.stopping && options.stopping->load(std::memory_order_relaxed)) {
                callbackStopped = true;
                return false;
            }
            // Never forward redirect or error-page bodies to a streaming
            // receiver. A 4xx/5xx HTML body is not MPEG-TS and must not be
            // allowed to poison the relay queue before the HTTP status is
            // reported to the reconnect loop.
            if (isRedirect(status) || (status != 0 && (status < 200 || status >= 300))) {
                return true;
            }
            const std::size_t available = receivedBytes < options.maxBodyBytes
                ? options.maxBodyBytes - receivedBytes
                : 0;
            const std::size_t accepted = (std::min)(size, available);
            receivedBytes += accepted;
            if (receiver) {
                const bool keepGoing = accepted == 0 ||
                    receiver(reinterpret_cast<const std::uint8_t*>(data), accepted);
                callbackStopped = !keepGoing;
                if (!keepGoing) return false;
            } else if (accepted != 0) {
                const auto* first = reinterpret_cast<const std::uint8_t*>(data);
                body.insert(body.end(), first, first + accepted);
            }
            if (accepted != size) {
                error = "HTTP response exceeds configured body limit";
                callbackStopped = true;
                return false;
            }
            return true;
        };

        auto result = client.Get(parsed.target, headers, responseHandler, contentHandler);
        if (!result) {
            response.status = status;
            response.effectiveUrl = currentUrl;
            response.contentType = std::move(contentType);
            response.body = std::move(body);
            if (callbackStopped || (options.stopping && options.stopping->load(std::memory_order_relaxed))) {
                if (error.empty()) error = "request cancelled";
            } else {
                error = "HTTP transport error: " + httplib::to_string(result.error());
            }
            return false;
        }

        status = result->status;
        if (contentType.empty()) contentType = result->get_header_value("Content-Type");
        if (location.empty()) location = result->get_header_value("Location");
        response.status = status;
        response.effectiveUrl = currentUrl;
        response.contentType = contentType;
        if (isRedirect(status) && !location.empty()) {
            if (redirect >= options.maxRedirects) {
                error = "HTTP redirect limit exceeded";
                return false;
            }
            const std::string next = resolveRedirect(currentUrl, location);
            if (next.empty()) {
                error = "invalid HTTP redirect target: " + location;
                return false;
            }
            previousOrigin = parsed.origin;
            currentUrl = next;
            continue;
        }

        response.contentType = std::move(contentType);
        response.body = std::move(body);
        if (status < 200 || status >= 300) {
            error = "HTTP " + std::to_string(status);
            return false;
        }
        return true;
    }

    error = "HTTP redirect limit exceeded";
    return false;
}

} // namespace dvbstreamer5::http
