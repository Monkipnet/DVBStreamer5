#include "protocols/stream/inputs/StreamVdrInputProtocol.h"

#include <algorithm>
#include <cctype>

namespace dvbstreamer5::stream_protocols::inputs {
namespace {

bool startsWithInsensitive(const std::string& value, const char* prefix) {
    const std::size_t prefixSize = std::char_traits<char>::length(prefix);
    return value.size() >= prefixSize &&
        std::equal(prefix, prefix + prefixSize, value.begin(),
            [](unsigned char left, unsigned char right) {
                return std::tolower(left) == std::tolower(right);
            });
}

bool validPortText(const std::string& text, int& port) {
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](unsigned char ch) {
            return ch >= '0' && ch <= '9';
        })) {
        return false;
    }
    try {
        port = std::stoi(text);
    } catch (...) {
        return false;
    }
    return port >= 1 && port <= 65535;
}

} // namespace

bool isVdrInput(const std::string& input, const std::string& mode, bool testPattern) {
    if (testPattern) return false;
    return mode == "vdr" || startsWithInsensitive(input, "vdr://");
}

bool resolveVdrStreamdevUri(
    const std::string& input,
    std::string& resolvedHttpUri,
    std::string& error) {
    resolvedHttpUri.clear();
    error.clear();

    if (!startsWithInsensitive(input, "vdr://")) {
        error = "VDR CI+ input must use vdr://host[:port]/channel";
        return false;
    }

    std::string rest = input.substr(6);
    const auto fragment = rest.find('#');
    if (fragment != std::string::npos) rest.resize(fragment);

    const auto slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
        error = "VDR CI+ input requires a channel number or VDR channel id";
        return false;
    }

    std::string authority = rest.substr(0, slash);
    std::string channelPath = rest.substr(slash + 1);
    if (channelPath.empty()) {
        error = "VDR CI+ input channel is empty";
        return false;
    }

    std::string host;
    int port = 3000;

    // Bracketed IPv6, for example vdr://[::1]:3000/1.
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            error = "VDR CI+ input has invalid IPv6 host";
            return false;
        }
        host = authority.substr(0, close + 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                error = "VDR CI+ input has invalid host/port";
                return false;
            }
            if (!validPortText(authority.substr(close + 2), port)) {
                error = "VDR CI+ input port is invalid";
                return false;
            }
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            // Unbracketed IPv6 is intentionally rejected to avoid ambiguous parsing.
            if (authority.find(':') != colon) {
                error = "VDR CI+ IPv6 host must be enclosed in brackets";
                return false;
            }
            host = authority.substr(0, colon);
            if (!validPortText(authority.substr(colon + 1), port)) {
                error = "VDR CI+ input port is invalid";
                return false;
            }
        } else {
            host = authority;
        }
    }

    if (host.empty()) {
        error = "VDR CI+ input host is empty";
        return false;
    }

    // Allow an explicit streamdev path while keeping the short form convenient.
    //   vdr://host/3      -> http://host:3000/TS/3
    //   vdr://host/TS/3   -> http://host:3000/TS/3
    if (channelPath.rfind("TS/", 0) != 0 &&
        channelPath.rfind("ts/", 0) != 0) {
        channelPath = "TS/" + channelPath;
    }

    resolvedHttpUri =
        "http://" + host + ":" + std::to_string(port) + "/" + channelPath;
    return true;
}

} // namespace dvbstreamer5::stream_protocols::inputs
