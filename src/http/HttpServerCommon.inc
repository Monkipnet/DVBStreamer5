#include "HttpServer.h"
#include "AppVersion.h"
#include "EmbeddedWebAssets.h"
#include "StableUdpOutput.h"

#include "utils.h"
#include "TranscoderModule.h"
#include "DvbSatellite.h"
#include "CardManager.h"
#include "OscamMiniManager.h"
#include "UdpInputFirewall.h"
#include "protocols/SrtVpsProfile.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <mutex>
#include <sstream>
#include <set>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

// HTTP control plane and embedded web UI. This module translates API requests into
// validated StreamConfig changes and publishes runtime state; media packet processing
// remains in StreamManager/media modules so UI code cannot become a transport path.

namespace {


uint64_t nativeMuxBitrate(const StreamConfig& cfg) {
    const uint64_t video = std::max<uint64_t>(500000, cfg.transcodeVideoBitrate);
    const uint64_t audio = std::max<uint64_t>(64000, cfg.transcodeAudioBitrate);
    const uint64_t minimum = video + audio + 1200000;
    return cfg.targetBitrate > 0 ? std::max<uint64_t>(cfg.targetBitrate, minimum) : minimum;
}
constexpr const char* kProgramVersion = dvbstreamer5::app::kProgramVersion;

// 202.50: quality history must not follow the web UI polling rate.  /api/state
// is normally requested every ~2 seconds and recording one string-heavy sample
// per stream on every poll made malloc_inuse grow almost linearly with
// quality_samples.  Keep at most one sample every 30 seconds per stream, compact
// older data further, and enforce a hard per-stream cap as a final safety bound.
constexpr int64_t kQualityRetentionSeconds = 30LL * 24LL * 60LL * 60LL;
constexpr int64_t kQualityRecordIntervalSeconds = 30LL;
constexpr int64_t kQualityFullResolutionSeconds = 60LL * 60LL;
constexpr int64_t kQualityDayResolutionSeconds = 60LL;
constexpr int64_t kQualityMonthResolutionSeconds = 5LL * 60LL;
constexpr int64_t kQualityCompactionIntervalSeconds = 60LL;
constexpr int64_t kQualityPersistIntervalSeconds = 5LL * 60LL;
constexpr std::size_t kQualityMaxSamplesPerStream = 10000;
constexpr const char* kQualityHistoryPath = "/var/lib/dvbstreamer5/quality-history.json";

// 202.47: the web UI polls /api/state and /api/system-metrics continuously.
// Creating a detached pthread for every request churned pthread stacks and
// glibc allocator arenas, making anonymous RSS climb even while all
// streaming queues stayed bounded. Reuse a fixed worker pool instead.
constexpr std::size_t kHttpSessionWorkers = 24;
constexpr uint32_t kMaxQueuedHttpSessions = 256;

struct CaDecodeUiMemory {
    std::string state;
    std::chrono::steady_clock::time_point holdUntil{};
};

std::mutex& caDecodeUiMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, CaDecodeUiMemory>& caDecodeUiMemory() {
    static std::map<std::string, CaDecodeUiMemory> memory;
    return memory;
}

std::string smoothCaDecodeState(const std::string& id, const std::string& rawState, bool active) {
    if (id.empty() || rawState == "not_applicable" || !active) {
        std::lock_guard<std::mutex> lock(caDecodeUiMutex());
        caDecodeUiMemory().erase(id);
        return active ? rawState : "offline";
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(caDecodeUiMutex());
    auto& item = caDecodeUiMemory()[id];
    if (rawState == "clear") {
        item.state = "clear";
        item.holdUntil = now + std::chrono::seconds(6);
        return rawState;
    }
    if (item.state == "clear" && now < item.holdUntil &&
        (rawState == "waiting" || rawState == "invalid" || rawState == "scrambled")) {
        return "clear";
    }
    item.state = rawState;
    item.holdUntil = now + std::chrono::seconds(2);
    return rawState;
}
std::string queryValue(const std::string& target, const std::string& key) {
    const auto queryPos = target.find('?');
    if (queryPos == std::string::npos) {
        return "";
    }

    std::string query = target.substr(queryPos + 1);
    std::istringstream stream(query);
    std::string part;
    while (std::getline(stream, part, '&')) {
        const auto eq = part.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (part.substr(0, eq) == key) {
            return part.substr(eq + 1);
        }
    }
    return "";
}

std::string urlDecode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            char* end = nullptr;
            const long ch = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0') {
                decoded.push_back(static_cast<char>(ch));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return decoded;
}

int64_t unixNowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string cleanPathToken(const std::string& value, bool allowDot = false) {
    std::string cleaned;
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || (allowDot && ch == '.')) {
            cleaned.push_back(ch);
        }
    }
    return cleaned;
}

std::string cleanHlsRelativePath(const std::string& value) {
    if (value.empty() || value.front() == '/' || value.back() == '/') return {};
    std::string cleaned;
    std::string segment;
    for (char ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '-' || ch == '_' || ch == '.') {
            segment.push_back(ch);
        } else if (ch == '/') {
            if (segment.empty() || segment == "." || segment == "..") return {};
            if (!cleaned.empty()) cleaned.push_back('/');
            cleaned += segment;
            segment.clear();
        } else {
            return {};
        }
    }
    if (segment.empty() || segment == "." || segment == "..") return {};
    if (!cleaned.empty()) cleaned.push_back('/');
    cleaned += segment;
    return cleaned;
}

std::string hlsPublicName(const StreamConfig& cfg) {
    const std::string raw = !cfg.name.empty() ? cfg.name : (!cfg.serviceName.empty() ? cfg.serviceName : cfg.id);
    std::string result;
    result.reserve(raw.size());
    bool underscore = false;
    for (unsigned char ch : raw) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            result.push_back(static_cast<char>(ch));
            underscore = false;
        } else if (!result.empty() && !underscore) {
            result.push_back('_');
            underscore = true;
        }
    }
    while (!result.empty() && result.back() == '_') result.pop_back();
    return result.empty() ? cleanPathToken(cfg.id) : result;
}


const StreamConfig* findStreamConfigById(const std::vector<StreamConfig>& streams, const std::string& id) {
    for (const auto& cfg : streams) if (cfg.id == id) return &cfg;
    return nullptr;
}

std::filesystem::path hlsStorageDirectory(const StreamConfig& cfg) {
    if (cfg.hlsArchiveEnabled) return std::filesystem::path(cfg.hlsArchivePath) / cfg.id;
    return std::filesystem::path("/tmp/dvbstreamer5-hls") / cfg.id;
}

struct HlsArchiveSegment {
    std::string name;
    int64_t timestamp = 0;
};

std::vector<HlsArchiveSegment> hlsArchiveSegments(const StreamConfig& cfg) {
    std::vector<HlsArchiveSegment> result;
    const auto directory = hlsStorageDirectory(cfg);
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return result;
    const int64_t cutoff = unixNowSeconds() - static_cast<int64_t>(cfg.hlsArchiveHours) * 3600;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || (entry.path().extension() != ".ts" && entry.path().extension() != ".m4s")) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("segment", 0) != 0) continue;
        const auto ft = entry.last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        const auto st = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        const int64_t stamp = std::chrono::duration_cast<std::chrono::seconds>(st.time_since_epoch()).count();
        if (stamp < cutoff) {
            std::filesystem::remove(entry.path(), ec);
            ec.clear();
            continue;
        }
        result.push_back({name, stamp});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.name < b.name;
    });
    return result;
}

bool parseHlsArchivePlaylistRequest(const std::string& fileName, int64_t& from, int64_t& to, bool& event) {
    from = 0; to = 0; event = false;
    std::smatch match;
    const int64_t now = unixNowSeconds();
    if (std::regex_match(fileName, match, std::regex(R"(^archive-([0-9]+)-([0-9]+)\.m3u8$)"))) {
        from = std::stoll(match[1].str());
        to = from + std::stoll(match[2].str());
        return true;
    }
    if (std::regex_match(fileName, match, std::regex(R"(^timeshift_rel-([0-9]+)\.m3u8$)")) ||
        std::regex_match(fileName, match, std::regex(R"(^rewind-([0-9]+)\.m3u8$)"))) {
        from = now - std::stoll(match[1].str());
        to = now + 10;
        event = true;
        return true;
    }
    if (std::regex_match(fileName, match, std::regex(R"(^timeshift_abs-([0-9]+)\.m3u8$)"))) {
        from = std::stoll(match[1].str());
        to = now + 10;
        event = true;
        return true;
    }
    return false;
}

std::string buildHlsArchivePlaylist(const StreamConfig& cfg, const std::string& fileName) {
    int64_t from = 0, to = 0;
    bool event = false;
    if (!cfg.hlsArchiveEnabled || !parseHlsArchivePlaylistRequest(fileName, from, to, event)) return {};
    const auto segments = hlsArchiveSegments(cfg);
    std::vector<HlsArchiveSegment> selected;
    for (const auto& segment : segments) {
        // file mtime is close to segment completion, so include one target
        // duration of margin at the requested beginning.
        if (segment.timestamp >= from - 2 && segment.timestamp <= to + 2) selected.push_back(segment);
    }
    if (selected.empty()) return {};

    std::ostringstream out;
    out << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n";
    out << "#EXT-X-MEDIA-SEQUENCE:0\n";
    if (event) out << "#EXT-X-PLAYLIST-TYPE:EVENT\n";
    else out << "#EXT-X-PLAYLIST-TYPE:VOD\n";
    for (size_t i = 0; i < selected.size(); ++i) {
        double duration = 2.0;
        if (i + 1 < selected.size()) {
            duration = static_cast<double>(selected[i + 1].timestamp - selected[i].timestamp);
            duration = std::clamp(duration, 1.0, 6.0);
        } else if (i > 0) {
            duration = static_cast<double>(selected[i].timestamp - selected[i - 1].timestamp);
            duration = std::clamp(duration, 1.0, 6.0);
        }
        out << "#EXT-X-PROGRAM-DATE-TIME:";
        const std::time_t t = static_cast<std::time_t>(selected[i].timestamp);
        std::tm tm{};
        gmtime_r(&t, &tm);
        out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ") << "\n";
        out << "#EXTINF:" << std::fixed << std::setprecision(3) << duration << ",\n";
        out << selected[i].name << "\n";
    }
    if (!event) out << "#EXT-X-ENDLIST\n";
    return out.str();
}

bool resolveHlsTarget(const std::vector<StreamConfig>& streams, const std::string& target,
                      std::string& streamId, std::string& fileName) {
    streamId.clear();
    fileName.clear();
    const std::string path = target.substr(0, target.find('?'));
    const std::string legacyPrefix = "/hls/";
    if (path.rfind(legacyPrefix, 0) == 0) {
        const auto slash = path.find('/', legacyPrefix.size());
        if (slash == std::string::npos) return false;
        streamId = cleanPathToken(path.substr(legacyPrefix.size(), slash - legacyPrefix.size()));
        fileName = cleanHlsRelativePath(path.substr(slash + 1));
        if (fileName == "playlist.m3u8" || fileName == "index.m3u8") fileName = "video.m3u8";
        return !streamId.empty() && !fileName.empty();
    }
    if (path.size() < 3 || path.front() != '/') return false;
    const auto slash = path.find('/', 1);
    if (slash == std::string::npos) return false;
    const std::string publicName = cleanPathToken(path.substr(1, slash - 1));
    fileName = cleanHlsRelativePath(path.substr(slash + 1));
    if (fileName == "index.m3u8") fileName = "video.m3u8";
    if (publicName.empty() || fileName.empty()) return false;
    for (const auto& cfg : streams) {
        bool hasHls = toLower(cfg.outputType) == "hls";
        for (const auto& extra : cfg.additionalOutputs) {
            if (toLower(extra.outputType) == "hls") { hasHls = true; break; }
        }
        if (hasHls && hlsPublicName(cfg) == publicName) {
            streamId = cfg.id;
            return true;
        }
    }
    return false;
}


bool hasOutputType(const StreamConfig& cfg, const std::string& wantedType) {
    if (toLower(cfg.outputType) == wantedType) return true;
    for (const auto& extra : cfg.additionalOutputs) {
        if (toLower(extra.outputType) == wantedType) return true;
    }
    return false;
}

bool resolveHttpMpegTsTarget(const std::vector<StreamConfig>& streams,
                             const std::string& target,
                             std::string& streamId) {
    streamId.clear();
    const std::string path = target.substr(0, target.find('?'));

    // Legacy URL kept for existing receivers:
    //   /stream/<internal-id>.ts
    const std::string legacyPrefix = "/stream/";
    if (path.rfind(legacyPrefix, 0) == 0) {
        const auto start = legacyPrefix.size();
        if (path.size() <= start + 3 || path.substr(path.size() - 3) != ".ts") return false;
        const auto end = path.find('.', start);
        streamId = cleanPathToken(path.substr(start,
            end == std::string::npos ? std::string::npos : end - start));
        return !streamId.empty();
    }

    // 202.71 public HTTP MPEG-TS URL:
    //   /<channel-name>/mpegts
    if (path.size() < 9 || path.front() != '/') return false;
    const auto slash = path.find('/', 1);
    if (slash == std::string::npos || path.substr(slash) != "/mpegts") return false;
    const std::string publicName = cleanPathToken(path.substr(1, slash - 1));
    if (publicName.empty()) return false;

    for (const auto& cfg : streams) {
        if (hasOutputType(cfg, "http") && hlsPublicName(cfg) == publicName) {
            streamId = cfg.id;
            return !streamId.empty();
        }
    }
    return false;
}

// Never expose the private relay as a legacy public /stream URL. This route
// inherits the control-panel Basic-auth requirement, irrespective of whether
// the configured channel has an HTTP output or subscriber filtering enabled.
bool resolvePrivatePreviewTarget(const std::string& target, std::string& id) {
    id.clear();
    constexpr const char* prefix = "/api/streams/";
    constexpr const char* suffix = "/preview.ts";
    const std::string path = target.substr(0, target.find('?'));
    if (path.compare(0, std::strlen(prefix), prefix) != 0 ||
        path.size() <= std::strlen(prefix) + std::strlen(suffix) ||
        path.compare(path.size() - std::strlen(suffix), std::strlen(suffix), suffix) != 0) {
        return false;
    }
    id = path.substr(std::strlen(prefix),
        path.size() - std::strlen(prefix) - std::strlen(suffix));
    return !id.empty() && cleanPathToken(id) == id;
}

// Only tokens generated by crypto.getRandomValues() are accepted. No user
// controlled token ever becomes part of a filesystem path, shell or URL.
bool validPreviewSessionToken(const std::string& token) {
    return token.size() == 32 &&
        std::all_of(token.begin(), token.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        });
}

bool privatePreviewTokenFromTarget(const std::string& target, std::string& token) {
    token.clear();
    const auto question = target.find('?');
    if (question == std::string::npos) return false;
    const std::string query = target.substr(question + 1);
    constexpr const char* prefix = "session=";
    if (query.compare(0, std::strlen(prefix), prefix) != 0) return false;
    token = query.substr(std::strlen(prefix));
    return validPreviewSessionToken(token);
}

bool resolvePrivatePreviewCloseTarget(const std::string& target, std::string& id) {
    id.clear();
    constexpr const char* prefix = "/api/streams/";
    constexpr const char* suffix = "/preview/close";
    if (target.compare(0, std::strlen(prefix), prefix) != 0 ||
        target.size() <= std::strlen(prefix) + std::strlen(suffix) ||
        target.compare(target.size() - std::strlen(suffix), std::strlen(suffix), suffix) != 0) {
        return false;
    }
    id = target.substr(std::strlen(prefix),
        target.size() - std::strlen(prefix) - std::strlen(suffix));
    return !id.empty() && cleanPathToken(id) == id;
}

std::string cleanFileName(const std::string& value) {
    std::string name;
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') {
            name.push_back(ch);
        } else if (std::isspace(static_cast<unsigned char>(ch))) {
            name.push_back('_');
        }
    }
    if (name.empty() || name == "." || name == "..") {
        name = "backup-file.ts";
    }
    if (!name.empty() && name.front() == '.') {
        name.insert(name.begin(), '_');
    }
    if (name.size() > 96) {
        name = name.substr(name.size() - 96);
    }
    return name;
}

std::string base64Decode(const std::string& value) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    int buffer = 0;
    int bits = -8;

    for (unsigned char ch : value) {
        if (std::isspace(ch)) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        const auto pos = alphabet.find(static_cast<char>(ch));
        if (pos == std::string::npos) {
            return "";
        }
        buffer = (buffer << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return decoded;
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }

    unsigned char diff = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

bool isLoopbackHost(const std::string& host) {
    const std::string lower = toLower(host);
    return lower == "localhost" || lower == "127.0.0.1" || lower == "::1";
}

std::string advertisedHost(const StreamConfig& cfg, bool replaceLoopback = false) {
    if (cfg.outputHost.empty() || cfg.outputHost == "0.0.0.0" || cfg.outputHost == "::" ||
        (replaceLoopback && isLoopbackHost(cfg.outputHost))) {
        if (!cfg.interfaceAddress.empty()) {
            return cfg.interfaceAddress;
        }
        const auto interfaces = enumerateNetworkInterfaces();
        if (!interfaces.empty()) {
            return interfaces.front().address;
        }
        return "127.0.0.1";
    }
    return cfg.outputHost;
}

int validPortOrDefault(int port, int defaultPort) {
    return port > 0 && port <= 65535 ? port : defaultPort;
}

std::string normalizedOutputType(const StreamConfig& cfg) {
    std::string type = toLower(cfg.outputType);
    if (type == "udp_vbr" || type == "udpvbr") {
        type = "udp-vbr";
    } else if (type == "udp_cbr" || type == "udpcbr") {
        type = "udp-cbr";
    }
    if (type == "udp") {
        return cfg.cbr ? "udp-cbr" : "udp-vbr";
    }
    if (type != "udp-vbr" && type != "udp-cbr" && type != "rtp" &&
        type != "srt" && type != "http" && type != "hls" &&
        type != "rtsp" && type != "rtmp" && type != "youtube") {
        return "udp-vbr";
    }
    return type;
}

StreamOutputConfig primaryOutputConfig(const StreamConfig& cfg) {
    StreamOutputConfig output;
    output.outputType = cfg.outputType;
    output.outputMode = cfg.outputMode;
    output.outputHost = cfg.outputHost;
    output.outputPort = cfg.outputPort;
    output.interfaceAddress = cfg.interfaceAddress;
    output.srtLatencyMs = cfg.srtOutputLatencyMs;
    output.srtPassphrase = cfg.srtOutputPassphrase;
    output.srtStreamId = cfg.srtOutputStreamId;
    output.srtPbKeyLen = cfg.srtOutputPbKeyLen;
    return output;
}

StreamConfig configForOutput(const StreamConfig& base, const StreamOutputConfig& output) {
    StreamConfig cfg = base;
    cfg.outputType = output.outputType;
    cfg.outputMode = output.outputMode;
    cfg.outputHost = output.outputHost;
    cfg.outputPort = output.outputPort;
    cfg.srtOutputLatencyMs = output.srtLatencyMs;
    cfg.srtOutputPassphrase = output.srtPassphrase;
    cfg.srtOutputStreamId = output.srtStreamId;
    cfg.srtOutputPbKeyLen = output.srtPbKeyLen;
    if (!output.interfaceAddress.empty()) {
        cfg.interfaceAddress = output.interfaceAddress;
    }
    cfg.additionalOutputs.clear();
    const std::string type = normalizedOutputType(cfg);
    if (type == "udp-cbr") {
        cfg.cbr = true;
    } else if (type == "udp-vbr") {
        cfg.cbr = false;
    }
    return cfg;
}

std::vector<StreamConfig> streamOutputs(const StreamConfig& cfg) {
    std::vector<StreamConfig> outputs;
    outputs.push_back(configForOutput(cfg, primaryOutputConfig(cfg)));
    for (const auto& output : cfg.additionalOutputs) {
        outputs.push_back(configForOutput(cfg, output));
    }
    return outputs;
}

int streamHttpPort(const StreamConfig& cfg, int defaultPort) {
    const std::string type = normalizedOutputType(cfg);
    if (type == "http" || type == "hls") {
        return validPortOrDefault(cfg.outputPort, defaultPort);
    }
    return defaultPort;
}

std::map<std::string, StreamConfig> streamConfigById(const std::vector<StreamConfig>& streams) {
    std::map<std::string, StreamConfig> result;
    for (const auto& stream : streams) {
        if (!stream.id.empty()) {
            result[stream.id] = stream;
        }
    }
    return result;
}

std::string srtQueryEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%'); out.push_back(hex[(c >> 4) & 15]); out.push_back(hex[c & 15]);
        }
    }
    return out;
}

bool streamUsesSrt(const StreamConfig& cfg) {
    if (toLower(cfg.inputUri).rfind("srt://", 0) == 0 || toLower(cfg.outputType) == "srt") {
        return true;
    }
    for (const auto& output : cfg.additionalOutputs) {
        if (toLower(output.outputType) == "srt") return true;
    }
    return false;
}

bool sameStreamConfig(const StreamConfig& left, const StreamConfig& right) {
    if (left.toJson() != right.toJson()) return false;
    if (!streamUsesSrt(left) && !streamUsesSrt(right)) return true;
    return left.srtVpsVdsOptimization == right.srtVpsVdsOptimization;
}

std::string streamLink(const StreamConfig& cfg, int httpPort) {
    const std::string type = normalizedOutputType(cfg);
    if (type == "rtp") {
        return "rtp://" + cfg.outputHost + ":" + std::to_string(cfg.outputPort);
    }
    if (type == "srt") {
        const std::string mode = toLower(cfg.outputMode) == "caller" ? "listener" : "caller";
        const bool listener = toLower(cfg.outputMode) != "caller";
        std::string link = "srt://" + advertisedHost(cfg, listener) + ":" +
            std::to_string(validPortOrDefault(cfg.outputPort, 7001)) + "?mode=" + mode +
            "&latency=" + std::to_string(dvbstreamer5::protocols::srt_vps::latencyMs(
            cfg, std::clamp(cfg.srtOutputLatencyMs, 20, 60000)));
        if (!cfg.srtOutputStreamId.empty() && mode == "caller") {
            link += "&streamid=" + srtQueryEncode(cfg.srtOutputStreamId);
        }
        return link;
    }
    if (type == "youtube") {
        const std::string hostLower = toLower(cfg.outputHost);
        return hostLower.rfind("rtmp", 0) == 0
            ? cfg.outputHost
            : "rtmp://a.rtmp.youtube.com/live2/" + cfg.outputHost;
    }
    if (type == "rtmp") {
        const std::string hostLower = toLower(cfg.outputHost);
        return hostLower.rfind("rtmp", 0) == 0
            ? cfg.outputHost
            : "rtmp://" + advertisedHost(cfg) + ":" + std::to_string(cfg.outputPort) + "/live/" + cfg.id;
    }
    if (type == "http") {
        return "http://" + advertisedHost(cfg, true) + ":" + std::to_string(streamHttpPort(cfg, httpPort)) +
            "/" + hlsPublicName(cfg) + "/mpegts";
    }
    if (type == "rtsp") {
        const std::string hostLower = toLower(cfg.outputHost);
        return hostLower.rfind("rtsp://", 0) == 0 || hostLower.rfind("rtsps://", 0) == 0
            ? cfg.outputHost
            : "rtsp://" + advertisedHost(cfg) + ":" + std::to_string(cfg.outputPort > 0 ? cfg.outputPort : 8554) + "/" + cfg.id;
    }
    if (type == "hls") {
        const std::string playlist = cfg.transcodeEnabled && cfg.transcodeMultibitrateEnabled
            ? "master.m3u8" : "video.m3u8";
        return "http://" + advertisedHost(cfg, true) + ":" + std::to_string(streamHttpPort(cfg, httpPort)) +
            "/" + hlsPublicName(cfg) + "/" + playlist;
    }
    return "udp://@" + cfg.outputHost + ":" + std::to_string(cfg.outputPort);
}

} // namespace

std::string extractStreamIdFromTarget(const std::string& target) {
    const std::string tsPrefix = "/stream/";
    if (target.rfind(tsPrefix, 0) == 0) {
        const auto start = tsPrefix.size();
        if (target.size() <= start + 3 || target.substr(target.size() - 3) != ".ts") {
            return "";
        }
        const auto end = target.find('.', start);
        return cleanPathToken(target.substr(start, end == std::string::npos ? std::string::npos : end - start));
    }

    const std::string hlsPrefix = "/hls/";
    if (target.rfind(hlsPrefix, 0) == 0) {
        const auto slash = target.find('/', hlsPrefix.size());
        if (slash == std::string::npos) {
            return "";
        }
        return cleanPathToken(target.substr(hlsPrefix.size(), slash - hlsPrefix.size()));
    }

    return "";
}

