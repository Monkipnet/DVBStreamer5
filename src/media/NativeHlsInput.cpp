#include "media/NativeHlsInput.h"

#include "NativeHttpClient.h"
#include "media/NativeCmaf.h"
#include "media/NativeSampleAes.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kLowAhead = std::chrono::seconds(4);
constexpr auto kTargetAhead = std::chrono::seconds(8);
constexpr auto kMaxAhead = std::chrono::seconds(12);
constexpr auto kPlaylistPollMin = std::chrono::milliseconds(400);
constexpr auto kPlaylistPollMax = std::chrono::milliseconds(2000);
constexpr long kPlaylistTimeoutMs = 3000;
constexpr long kSegmentTimeoutMs = 12000;
constexpr std::size_t kMaxPlaylistBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxSegmentBytes = 64U * 1024U * 1024U;

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string resolveUrl(const std::string& base, const std::string& reference) {
    if (reference.empty()) return base;
    const std::string refLower = lower(reference);
    if (refLower.rfind("http://", 0) == 0 || refLower.rfind("https://", 0) == 0) return reference;
    const auto schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return reference;
    const auto authorityEnd = base.find('/', schemeEnd + 3);
    const std::string origin = authorityEnd == std::string::npos ? base : base.substr(0, authorityEnd);
    if (reference.front() == '/') return origin + reference;
    std::string path = base.substr(0, base.find_first_of("?#"));
    const auto slash = path.rfind('/');
    if (slash == std::string::npos || slash < schemeEnd + 3) return origin + "/" + reference;
    return path.substr(0, slash + 1) + reference;
}

std::string queryAuthenticatedUrl(const std::string& raw, const StreamConfig& cfg) {
    if (lower(cfg.hlsAccessKeyMode) != "query" || cfg.hlsAccessKeyName.empty() || cfg.hlsAccessKeyValue.empty()) {
        return raw;
    }
    const std::string name = dvbstreamer5::http::encodeQueryComponent(cfg.hlsAccessKeyName);
    const std::string value = dvbstreamer5::http::encodeQueryComponent(cfg.hlsAccessKeyValue);
    std::string url = raw;
    const auto fragmentPos = url.find('#');
    const std::string fragment = fragmentPos == std::string::npos ? std::string() : url.substr(fragmentPos);
    if (fragmentPos != std::string::npos) url.erase(fragmentPos);
    const auto queryPos = url.find('?');
    const std::string query = queryPos == std::string::npos ? std::string() : url.substr(queryPos + 1);
    const std::string key = name + "=";
    if (query.rfind(key, 0) != 0 && query.find("&" + key) == std::string::npos) {
        url += queryPos == std::string::npos ? "?" : "&";
        url += name + "=" + value;
    }
    return url + fragment;
}

bool fetch(const std::string& rawUrl,
           const StreamConfig& cfg,
           std::atomic<bool>& stopping,
           std::size_t maxBytes,
           long timeoutMs,
           std::vector<std::uint8_t>& body,
           std::string& effectiveUrl,
           std::string& error) {
    dvbstreamer5::http::RequestOptions options;
    options.connectTimeoutMs = std::min<long>(3000, timeoutMs);
    options.readTimeoutMs = timeoutMs;
    options.writeTimeoutMs = options.connectTimeoutMs;
    options.totalTimeoutMs = timeoutMs;
    options.maxRedirects = 8;
    options.forwardHeadersAcrossOrigins = true;
    options.userAgent = cfg.hlsUserAgent.empty() ? "Mozilla/5.0 DVBStreamer5" : cfg.hlsUserAgent;
    options.maxBodyBytes = maxBytes;
    options.stopping = &stopping;
    if (lower(cfg.hlsAccessKeyMode) == "header" && !cfg.hlsAccessKeyName.empty() && !cfg.hlsAccessKeyValue.empty()) {
        options.headers.emplace_back(cfg.hlsAccessKeyName, cfg.hlsAccessKeyValue);
    }
    dvbstreamer5::http::Response response;
    const bool ok = dvbstreamer5::http::get(queryAuthenticatedUrl(rawUrl, cfg), options, response, error);
    body = std::move(response.body);
    effectiveUrl = response.effectiveUrl.empty() ? rawUrl : response.effectiveUrl;
    if (!ok && response.status != 0) error += " (HTTP " + std::to_string(response.status) + ")";
    return ok;
}

struct Variant {
    std::uint64_t bandwidth = 0;
    std::string url;
};

struct Segment {
    std::uint64_t sequence = 0;
    double duration = 0.0;
    std::string url;
    bool discontinuity = false;
    std::string keyUri;
    std::string iv;
    std::string keyMethod;
};

struct Playlist {
    bool master = false;
    bool endList = false;
    bool hasMap = false;
    std::string mapUri;
    std::uint64_t mediaSequence = 0;
    double targetDuration = 2.0;
    std::vector<Variant> variants;
    std::vector<Segment> segments;
};

std::uint64_t unsignedAttribute(const std::string& line, const std::string& name) {
    const auto p = line.find(name + "=");
    if (p == std::string::npos) return 0;
    std::size_t first = p + name.size() + 1;
    std::size_t last = first;
    while (last < line.size() && std::isdigit(static_cast<unsigned char>(line[last]))) ++last;
    if (last == first) return 0;
    try { return std::stoull(line.substr(first, last - first)); } catch (...) { return 0; }
}

std::string attribute(const std::string& line, const std::string& name) {
    const auto p = line.find(name + "=");
    if (p == std::string::npos) return {};
    std::size_t first = p + name.size() + 1;
    if (first >= line.size()) return {};
    if (line[first] == '"') {
        ++first;
        const auto last = line.find('"', first);
        return last == std::string::npos ? line.substr(first) : line.substr(first, last - first);
    }
    const auto last = line.find(',', first);
    return trim(line.substr(first, last == std::string::npos ? std::string::npos : last - first));
}

Playlist parsePlaylist(const std::string& text, const std::string& baseUrl) {
    Playlist out;
    std::istringstream input(text);
    std::string line;
    double pendingDuration = -1.0;
    bool pendingDiscontinuity = false;
    std::uint64_t pendingBandwidth = 0;
    std::uint64_t nextSequence = 0;
    std::string keyUri;
    std::string keyIv;
    std::string keyMethod;
    while (std::getline(input, line)) {
        line = trim(std::move(line));
        if (line.empty()) continue;
        if (line.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
            try { out.mediaSequence = std::stoull(trim(line.substr(22))); } catch (...) { out.mediaSequence = 0; }
            nextSequence = out.mediaSequence;
        } else if (line.rfind("#EXT-X-TARGETDURATION:", 0) == 0) {
            try { out.targetDuration = std::max(0.25, std::stod(trim(line.substr(22)))); } catch (...) {}
        } else if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
            out.master = true;
            pendingBandwidth = unsignedAttribute(line, "BANDWIDTH");
        } else if (line.rfind("#EXTINF:", 0) == 0) {
            const auto comma = line.find(',');
            try { pendingDuration = std::stod(line.substr(8, comma == std::string::npos ? std::string::npos : comma - 8)); }
            catch (...) { pendingDuration = out.targetDuration; }
        } else if (line == "#EXT-X-DISCONTINUITY") {
            pendingDiscontinuity = true;
        } else if (line.rfind("#EXT-X-MAP:", 0) == 0) {
            out.hasMap = true;
            out.mapUri = resolveUrl(baseUrl, attribute(line, "URI"));
        } else if (line.rfind("#EXT-X-KEY:", 0) == 0) {
            const std::string method = lower(attribute(line, "METHOD"));
            if (method.empty() || method == "none") {
                keyUri.clear(); keyIv.clear(); keyMethod.clear();
            } else if (method == "aes-128" || method == "sample-aes") {
                keyMethod = method;
                keyUri = resolveUrl(baseUrl, attribute(line, "URI"));
                keyIv = attribute(line, "IV");
            } else {
                keyMethod = method;
                keyUri = "unsupported:" + method;
                keyIv.clear();
            }
        } else if (line == "#EXT-X-ENDLIST") {
            out.endList = true;
        } else if (line.front() != '#') {
            if (out.master || pendingBandwidth != 0) {
                out.variants.push_back({pendingBandwidth, resolveUrl(baseUrl, line)});
                pendingBandwidth = 0;
            } else if (pendingDuration >= 0.0) {
                out.segments.push_back({nextSequence++, pendingDuration > 0.0 ? pendingDuration : out.targetDuration,
                                        resolveUrl(baseUrl, line), pendingDiscontinuity, keyUri, keyIv, keyMethod});
                pendingDuration = -1.0;
                pendingDiscontinuity = false;
            }
        }
    }
    return out;
}

std::optional<Variant> chooseVariant(const std::vector<Variant>& variants, std::uint64_t targetBitrate) {
    if (variants.empty()) return std::nullopt;
    const Variant* below = nullptr;
    const Variant* above = nullptr;
    for (const auto& item : variants) {
        if (item.bandwidth <= targetBitrate && (!below || item.bandwidth > below->bandwidth)) below = &item;
        if (item.bandwidth > targetBitrate && (!above || item.bandwidth < above->bandwidth)) above = &item;
    }
    if (below) return *below;
    if (above) return *above;
    return variants.front();
}

std::optional<std::size_t> findSequence(const Playlist& playlist, std::uint64_t sequence) {
    for (std::size_t i = 0; i < playlist.segments.size(); ++i) {
        if (playlist.segments[i].sequence == sequence) return i;
    }
    return std::nullopt;
}

std::uint64_t startupSequence(const Playlist& playlist) {
    if (playlist.segments.empty()) return playlist.mediaSequence;
    if (playlist.endList) return playlist.segments.front().sequence;
    double accumulated = 0.0;
    std::size_t start = playlist.segments.size() - 1;
    for (std::size_t i = playlist.segments.size(); i-- > 0;) {
        accumulated += playlist.segments[i].duration;
        start = i;
        if (accumulated >= 6.0 || playlist.segments.size() - i >= 3) break;
    }
    return playlist.segments[start].sequence;
}

bool parseIv(const Segment& segment, std::array<unsigned char, 16>& iv) {
    iv.fill(0);
    if (segment.iv.empty()) {
        std::uint64_t seq = segment.sequence;
        for (int i = 15; i >= 8; --i) { iv[static_cast<std::size_t>(i)] = static_cast<unsigned char>(seq & 0xffU); seq >>= 8; }
        return true;
    }
    std::string hex = segment.iv;
    if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) hex.erase(0, 2);
    if (hex.size() > 32) return false;
    if (hex.size() % 2 != 0) hex.insert(hex.begin(), '0');
    const std::size_t offset = 16 - hex.size() / 2;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        try { iv[offset + i / 2] = static_cast<unsigned char>(std::stoul(hex.substr(i, 2), nullptr, 16)); }
        catch (...) { return false; }
    }
    return true;
}

bool decryptSegment(const Segment& segment,
                   const StreamConfig& cfg,
                   std::atomic<bool>& stopping,
                   std::vector<std::uint8_t>& data,
                   std::string& error) {
    if (segment.keyUri.empty()) return true;
    if (segment.keyUri.rfind("unsupported:", 0) == 0) {
        error = "native HLS input does not support " + segment.keyUri.substr(12) + " encryption";
        return false;
    }
    std::vector<std::uint8_t> key;
    std::string effective;
    if (!fetch(segment.keyUri, cfg, stopping, 1024, 4000, key, effective, error)) {
        error = "HLS AES-128 key fetch failed: " + error;
        return false;
    }
    if (key.size() < 16) { error = "HLS AES-128 key is shorter than 16 bytes"; return false; }
    std::array<unsigned char, 16> iv{};
    if (!parseIv(segment, iv)) { error = "HLS AES-128 IV is invalid"; return false; }

    if (segment.keyMethod == "sample-aes") {
        std::array<std::uint8_t, 16> k{};
        std::copy_n(key.begin(), 16, k.begin());
        std::array<std::uint8_t, 16> siv{};
        std::copy(iv.begin(), iv.end(), siv.begin());
        std::vector<std::uint8_t> plain;
        if (!dvbstreamer5::media::hls::transformSampleAesMpegTs(data.data(), data.size(), k, siv, false, plain, error)) {
            error = "HLS SAMPLE-AES decrypt failed: " + error;
            return false;
        }
        data.swap(plain);
        return true;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { error = "EVP_CIPHER_CTX_new failed"; return false; }
    std::vector<std::uint8_t> plain(data.size() + 16);
    int written = 0, finalWritten = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key.data(), iv.data()) == 1 &&
              EVP_DecryptUpdate(ctx, plain.data(), &written, data.data(), static_cast<int>(data.size())) == 1 &&
              EVP_DecryptFinal_ex(ctx, plain.data() + written, &finalWritten) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) { error = "HLS AES-128 decrypt failed"; return false; }
    plain.resize(static_cast<std::size_t>(written + finalWritten));
    data.swap(plain);
    return true;
}

std::chrono::milliseconds pollInterval(const Playlist& playlist) {
    auto value = std::chrono::milliseconds(static_cast<int>(std::llround(playlist.targetDuration * 500.0)));
    return std::clamp(value, kPlaylistPollMin, kPlaylistPollMax);
}

} // namespace

namespace dvbstreamer5::media::hls {

NativeHlsInput::~NativeHlsInput() { stop(); }

bool NativeHlsInput::start(const StreamConfig& config,
                           DataCallback dataCallback,
                           FinishCallback finishCallback,
                           std::string& error) {
    stop();
    if (!dataCallback) { error = "native HLS input requires a data callback"; return false; }
    config_ = config;
    dataCallback_ = std::move(dataCallback);
    finishCallback_ = std::move(finishCallback);
    inputBytes_.store(0);
    mediaBitrate_.store(0);
    stopping_.store(false);
    running_.store(true);
    setError({});
    try { worker_ = std::thread(&NativeHlsInput::run, this); }
    catch (const std::exception& ex) {
        running_.store(false);
        error = std::string("native HLS worker could not start: ") + ex.what();
        return false;
    }
    error.clear();
    return true;
}

void NativeHlsInput::stop() noexcept {
    stopping_.store(true);
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
    running_.store(false);
}

bool NativeHlsInput::isRunning() const noexcept { return running_.load(); }
std::uint64_t NativeHlsInput::inputBytes() const noexcept { return inputBytes_.load(); }
std::uint64_t NativeHlsInput::mediaBitrate() const noexcept { return mediaBitrate_.load(); }
std::string NativeHlsInput::lastError() const { std::lock_guard<std::mutex> lock(errorMutex_); return lastError_; }
void NativeHlsInput::setError(const std::string& error) { std::lock_guard<std::mutex> lock(errorMutex_); lastError_ = error; }

void NativeHlsInput::run() {
    std::string root = config_.inputUri;
    if (root.rfind("hls://", 0) == 0) root = "http://" + root.substr(6);
    std::string active = root;
    std::string error;
    auto fail = [&](const std::string& value) {
        setError(value);
        if (finishCallback_) finishCallback_(value);
    };

    auto loadPlaylist = [&](Playlist& playlist) -> bool {
        std::vector<std::uint8_t> bytes;
        std::string effective;
        if (!fetch(active, config_, stopping_, kMaxPlaylistBytes, kPlaylistTimeoutMs, bytes, effective, error)) return false;
        const std::string text(bytes.begin(), bytes.end());
        playlist = parsePlaylist(text, effective);
        if (playlist.master) {
            const auto variant = chooseVariant(playlist.variants, config_.targetBitrate);
            if (!variant) { error = "HLS master playlist contains no variants"; return false; }
            active = variant->url;
            bytes.clear(); effective.clear();
            if (!fetch(active, config_, stopping_, kMaxPlaylistBytes, kPlaylistTimeoutMs, bytes, effective, error)) return false;
            playlist = parsePlaylist(std::string(bytes.begin(), bytes.end()), effective);
        }
        if (playlist.segments.empty()) { error = "HLS media playlist contains no segments"; return false; }
        return true;
    };

    Playlist playlist;
    const auto startupDeadline = Clock::now() + std::chrono::seconds(15);
    while (!stopping_.load() && !loadPlaylist(playlist)) {
        if (Clock::now() >= startupDeadline) {
            fail("native HLS playlist load failed: " + error);
            running_.store(false);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        active = root;
    }
    if (stopping_.load()) { running_.store(false); return; }

    std::uint64_t nextSequence = startupSequence(playlist);
    double pushedSeconds = 0.0;
    std::uint64_t rateBytes = 0;
    double rateSeconds = 0.0;
    const auto playbackStart = Clock::now();
    auto lastReload = Clock::now();
    dvbstreamer5::media::cmaf::Fmp4ToMpegTs fmp4;
    std::string initializedMap;

    // MPEG-TS HLS segments arrive from the origin as multi-megabyte bursts.
    // Feeding a complete 6-second segment to the relay in one call produces
    // burst -> silence -> burst timing on UDP-VBR and also builds a very deep
    // queue in UDP-CBR.  Keep a media-time wall-clock deadline and release
    // conventional 7-packet (1316-byte) chunks smoothly across EXTINF.
    bool tsPacingStarted = false;
    Clock::time_point tsSegmentDeadline {};

    auto waitUntil = [&](Clock::time_point deadline) -> bool {
        constexpr auto kStopPoll = std::chrono::milliseconds(50);
        while (!stopping_.load()) {
            const auto now = Clock::now();
            if (now >= deadline) return true;
            const auto remaining = deadline - now;
            std::this_thread::sleep_for((std::min)(
                remaining,
                std::chrono::duration_cast<Clock::duration>(kStopPoll)));
        }
        return false;
    };

    auto pushTsSegmentPaced = [&](const Segment& segment,
                                  const std::vector<std::uint8_t>& bytes,
                                  double fetchSeconds) -> bool {
        constexpr std::size_t kTsPacketSize = 188;
        constexpr std::size_t kPacketsPerBatch = 7;
        constexpr std::size_t kBatchBytes = kTsPacketSize * kPacketsPerBatch;

        if (bytes.empty()) {
            error = "HLS MPEG-TS segment is empty";
            return false;
        }
        if ((bytes.size() % kTsPacketSize) != 0 || bytes.front() != 0x47) {
            error = "HLS MPEG-TS segment is not 188-byte aligned";
            return false;
        }

        const double durationSeconds = std::max(0.001, segment.duration);
        const auto mediaDuration = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(durationSeconds));
        const auto now = Clock::now();

        bool resync = false;
        if (!tsPacingStarted || segment.discontinuity) {
            tsPacingStarted = true;
            tsSegmentDeadline = now + mediaDuration;
            resync = segment.discontinuity;
        } else {
            tsSegmentDeadline += mediaDuration;
            // A long origin/network stall should not be followed by a huge
            // high-speed catch-up burst.  Re-anchor after two seconds late.
            if (now > tsSegmentDeadline + std::chrono::seconds(2)) {
                tsSegmentDeadline = now + mediaDuration;
                resync = true;
            }
        }

        const auto pacingStart = Clock::now();
        const auto pacingSpan = tsSegmentDeadline > pacingStart
            ? tsSegmentDeadline - pacingStart
            : Clock::duration::zero();
        const auto pacingNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            pacingSpan).count();

        std::size_t offset = 0;
        while (offset < bytes.size() && !stopping_.load()) {
            const std::size_t chunk = (std::min)(kBatchBytes, bytes.size() - offset);
            if (!dataCallback_(bytes.data() + offset, chunk)) {
                error = "native HLS relay rejected paced segment data";
                return false;
            }
            offset += chunk;

            if (offset < bytes.size() && pacingNs > 0) {
                const auto elapsedNs = static_cast<std::int64_t>(
                    (static_cast<long double>(pacingNs) *
                     static_cast<long double>(offset)) /
                    static_cast<long double>(bytes.size()));
                if (!waitUntil(pacingStart + std::chrono::nanoseconds(elapsedNs))) {
                    return false;
                }
            }
        }

        if (stopping_.load()) return false;
        if (!waitUntil(tsSegmentDeadline)) return false;

        const auto actualPacingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - pacingStart).count();
        const std::uint64_t mediaKbps = static_cast<std::uint64_t>(
            (static_cast<long double>(bytes.size()) * 8.0L) /
            durationSeconds / 1000.0L);
        std::cerr << "NATIVE HLS SEGMENT"
                  << " seq=" << segment.sequence
                  << " duration_ms=" << static_cast<long long>(durationSeconds * 1000.0)
                  << " bytes=" << bytes.size()
                  << " media_kbps=" << mediaKbps
                  << " fetch_ms=" << static_cast<long long>(fetchSeconds * 1000.0)
                  << " pace_ms=" << actualPacingMs
                  << " resync=" << (resync ? 1 : 0)
                  << std::endl;
        return true;
    };

    auto ensureMap = [&](const Playlist& pl) -> bool {
        if (!pl.hasMap) return true;
        if (pl.mapUri.empty()) { error = "HLS EXT-X-MAP is missing URI"; return false; }
        if (initializedMap == pl.mapUri) return true;
        std::vector<std::uint8_t> init;
        std::string effective;
        if (!fetch(pl.mapUri, config_, stopping_, kMaxSegmentBytes, kSegmentTimeoutMs, init, effective, error)) {
            error = "HLS EXT-X-MAP fetch failed: " + error; return false;
        }
        if (!fmp4.initialize(init,
              [this](const std::uint8_t* d, std::size_t n) { return dataCallback_ && dataCallback_(d,n); }, error)) {
            error = "HLS fMP4 init parse failed: " + error; return false;
        }
        inputBytes_.fetch_add(init.size());
        initializedMap = pl.mapUri;
        return true;
    };

    if (!ensureMap(playlist)) { fail(error); running_.store(false); return; }

    while (!stopping_.load()) {
        const double elapsed = std::chrono::duration<double>(Clock::now() - playbackStart).count();
        const double ahead = std::max(0.0, pushedSeconds - elapsed);
        const auto index = findSequence(playlist, nextSequence);
        if (index && ahead < std::chrono::duration<double>(kTargetAhead).count()) {
            const Segment segment = playlist.segments[*index];
            std::vector<std::uint8_t> bytes;
            std::string effective;
            const auto segmentFetchStart = Clock::now();
            if (!fetch(segment.url, config_, stopping_, kMaxSegmentBytes, kSegmentTimeoutMs, bytes, effective, error)) {
                if (stopping_.load()) break;
                std::cerr << "NATIVE HLS SEGMENT FETCH ERROR"
                          << " seq=" << segment.sequence
                          << " error=" << error << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                Playlist refreshed;
                if (loadPlaylist(refreshed)) { playlist = std::move(refreshed); lastReload = Clock::now(); }
                continue;
            }
            const double segmentFetchSeconds = std::chrono::duration<double>(
                Clock::now() - segmentFetchStart).count();
            if (playlist.hasMap && segment.keyMethod == "sample-aes") {
                std::vector<std::uint8_t> keyBytes;
                std::string keyEffective;
                if (!fetch(segment.keyUri, config_, stopping_, 1024, 4000, keyBytes, keyEffective, error)) {
                    if (!stopping_.load()) fail("HLS CMAF SAMPLE-AES key fetch failed: " + error);
                    break;
                }
                if (keyBytes.size() < 16) {
                    if (!stopping_.load()) fail("HLS CMAF SAMPLE-AES key is shorter than 16 bytes");
                    break;
                }
                std::array<std::uint8_t,16> key{};
                std::copy_n(keyBytes.begin(), 16, key.begin());
                if (!dvbstreamer5::media::cmaf::decryptSampleAesFragment(bytes, key, error)) {
                    if (!stopping_.load()) fail("native HLS fMP4 SAMPLE-AES/cbcs decrypt failed: " + error);
                    break;
                }
            } else if (!decryptSegment(segment, config_, stopping_, bytes, error)) {
                if (!stopping_.load()) fail("native HLS segment decrypt failed: " + error);
                break;
            }
            bool accepted = false;
            if (!playlist.hasMap) {
                accepted = pushTsSegmentPaced(segment, bytes, segmentFetchSeconds);
            } else {
                if (!ensureMap(playlist)) { if (!stopping_.load()) fail(error); break; }
                accepted = !bytes.empty() && fmp4.pushFragment(bytes, error);
            }
            if (!accepted) {
                if (!stopping_.load()) fail(error.empty() ? "native HLS relay rejected segment data" : error);
                break;
            }
            inputBytes_.fetch_add(bytes.size());
            pushedSeconds += std::max(0.001, segment.duration);
            rateBytes += bytes.size();
            rateSeconds += std::max(0.001, segment.duration);
            if (rateSeconds >= 1.0) {
                mediaBitrate_.store(static_cast<std::uint64_t>((static_cast<long double>(rateBytes) * 8.0L) / rateSeconds));
                if (rateSeconds >= 12.0) { rateBytes = 0; rateSeconds = 0.0; }
            }
            nextSequence = segment.sequence + 1;
            continue;
        }

        if (playlist.endList && !index && nextSequence > playlist.segments.back().sequence) {
            if (finishCallback_) finishCallback_({});
            break;
        }

        const auto interval = pollInterval(playlist);
        const auto now = Clock::now();
        if (now - lastReload >= interval || !index) {
            Playlist refreshed;
            if (loadPlaylist(refreshed)) {
                playlist = std::move(refreshed);
                if (nextSequence < playlist.segments.front().sequence) nextSequence = startupSequence(playlist);
            }
            lastReload = Clock::now();
        }
        const double currentElapsed = std::chrono::duration<double>(Clock::now() - playbackStart).count();
        const double currentAhead = std::max(0.0, pushedSeconds - currentElapsed);
        const auto sleepFor = currentAhead > std::chrono::duration<double>(kLowAhead).count()
            ? std::chrono::milliseconds(100)
            : std::chrono::milliseconds(25);
        std::this_thread::sleep_for(sleepFor);
    }

    running_.store(false);
}

} // namespace dvbstreamer5::media::hls
