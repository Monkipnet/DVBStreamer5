#include "ExternalXmltv.h"
#include "EpgDiskCache.h"

#include <curl/curl.h>
#include <libxml/xmlreader.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>
#include <unistd.h>

namespace {
constexpr std::size_t kMaxDownload = 512U * 1024U * 1024U; // Temporary file, NOT RAM
constexpr std::uint64_t kMaxXml = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxEvents = 300000U; // Maximum retained events in RAM
constexpr std::size_t kMaxEventsPerChannel = 512U;
constexpr std::size_t kMaxScannedEvents = 5000000U;
constexpr std::size_t kMaxChannels = 15000U;

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::tm localDay(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

int dayStamp(const std::tm& tm) {
    return (tm.tm_year + 1900) * 1000 + tm.tm_yday;
}

bool dueForDailyRun(const std::string& clock, const std::tm& local) {
    if (clock.size() != 5 || clock[2] != ':') return false;
    const int hours = (clock[0] - '0') * 10 + clock[1] - '0';
    const int minutes = (clock[3] - '0') * 10 + clock[4] - '0';
    return local.tm_hour * 60 + local.tm_min >= hours * 60 + minutes;
}

bool xmltvTime(const std::string& value, std::int64_t& unixTime) {
    if (value.size() < 12) return false;
    for (std::size_t i = 0; i < 12; ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (std::sscanf(value.c_str(), "%4d%2d%2d%2d%2d",
                    &year, &month, &day, &hour, &minute) != 5) return false;
    int second = 0;
    std::size_t tzStart = 12;
    if (value.size() >= 14 && value[12] >= '0' && value[12] <= '9' &&
        value[13] >= '0' && value[13] <= '9') {
        second = (value[12] - '0') * 10 + value[13] - '0';
        tzStart = 14;
    }
    if (year < 1970 || year > 2100 || month < 1 || month > 12 || day < 1 ||
        day > 31 || hour > 23 || minute > 59 || second > 60) return false;
    std::tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
#ifdef _WIN32
    const std::time_t base = _mkgmtime(&utc);
#else
    const std::time_t base = timegm(&utc);
#endif
    if (base < 0) return false;
    unixTime = static_cast<std::int64_t>(base);
    const auto sign = value.find_first_of("+-", tzStart);
    if (sign != std::string::npos && sign + 5 <= value.size()) {
        const std::string offset = value.substr(sign + 1, 4);
        if (offset.find_first_not_of("0123456789") != std::string::npos)
            return false;
        const int offsetMinutes = std::stoi(offset.substr(0, 2)) * 60 +
                                  std::stoi(offset.substr(2, 2));
        if (offsetMinutes > 1439) return false;
        unixTime += (value[sign] == '+' ? -1 : 1) *
                    static_cast<std::int64_t>(offsetMinutes) * 60;
    }
    return true;
}

struct DownloadSink {
    std::FILE* file = nullptr;
    std::size_t total = 0;
    bool sizeLimit = false;
    int writeErrno = 0;
};

// libcurl returning zero deliberately aborts the transfer. Record our
// particular reason so CURLE_WRITE_ERROR is never shown as a generic disk
// error (the previous 32-MiB in-memory cap caused that confusing message).
std::size_t acceptCurl(void* bytes, std::size_t elementSize,
                       std::size_t count, void* user) {
    auto& sink = *static_cast<DownloadSink*>(user);
    if (!elementSize || count > std::numeric_limits<std::size_t>::max() / elementSize) {
        sink.sizeLimit = true;
        return 0;
    }
    const std::size_t n = elementSize * count;
    if (n > kMaxDownload || sink.total > kMaxDownload - n) {
        sink.sizeLimit = true;
        return 0;
    }
    const std::size_t saved = std::fwrite(bytes, 1, n, sink.file);
    sink.total += saved;
    if (saved != n) sink.writeErrno = errno ? errno : EIO;
    return saved;
}

// zlib's gzread transparently accepts both XML and gzip-compressed XML.
// The temporary file is only a byte spool; the XML reader processes it
// sequentially without creating a second decompressed copy.
struct XmlInput {
    gzFile file = nullptr;
    std::uint64_t uncompressed = 0;
    bool sizeLimit = false;
    bool readError = false;
};
int xmlReadCallback(void* user, char* buffer, int count) {
    auto& input = *static_cast<XmlInput*>(user);
    if (!input.file || count <= 0) return 0;
    const auto available = input.uncompressed < kMaxXml
        ? kMaxXml - input.uncompressed : std::uint64_t{0};
    if (!available) {
        char extra;
        const int got = gzread(input.file, &extra, 1);
        if (got > 0) input.sizeLimit = true;
        else if (got < 0) input.readError = true;
        return got == 0 ? 0 : -1;
    }
    const unsigned ask = static_cast<unsigned>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(count), available));
    const int got = gzread(input.file, buffer, ask);
    if (got < 0) {
        input.readError = true;
        return -1;
    }
    input.uncompressed += static_cast<unsigned>(got);
    return got;
}
int xmlCloseCallback(void* user) {
    auto& input = *static_cast<XmlInput*>(user);
    if (!input.file) return 0;
    const int result = gzclose(input.file);
    input.file = nullptr;
    if (result != Z_OK) input.readError = true;
    return result == Z_OK ? 0 : -1;
}

std::string xmlProperty(xmlTextReaderPtr reader, const char* name) {
    xmlChar* value = xmlTextReaderGetAttribute(
        reader, reinterpret_cast<const xmlChar*>(name));
    if (!value) return {};
    std::string result(reinterpret_cast<const char*>(value));
    xmlFree(value);
    return result;
}

std::string xmlValue(xmlTextReaderPtr reader) {
    xmlChar* text = xmlTextReaderReadString(reader);
    if (!text) return {};
    std::string result(reinterpret_cast<const char*>(text));
    xmlFree(text);
    // Ignore descriptions too large to be useful to a guide reader.
    if (result.size() > 1536) {
        // Avoid per-event multikilobyte strings in large XMLTV feeds. Titles
        // are much shorter; even DVB short_event descriptors have tiny limits.
        std::size_t length = 1536;
        while (length > 0 &&
               (static_cast<unsigned char>(result[length]) & 0xc0U) == 0x80U)
            --length;
        result.resize(length);
    }
    return result;
}

} // namespace

bool ExternalXmltvManager::restoreCache(
    const XmltvSourceConfig& settings, Source& data) {
    epgcache::Reader reader(epgcache::pathFor("xmltv", settings.id));
    if (!reader.valid()) return false;
    Json::Value header;
    if (!reader.next(header) || header.get("schema", 0).asInt() != 1 ||
        header.get("kind", "").asString() != "xmltv" ||
        header.get("source_id", "").asString() != settings.id ||
        header.get("url", "").asString() != settings.url) return false;
    Source next;
    next.settings = settings;
    next.lastSuccess = header.get("last_success", 0).asInt64();
    const std::int64_t now = nowSeconds();
    const std::int64_t earliest = now - 6 * 3600;
    const std::int64_t latest = now + 7 * 86400;
    Json::Value item;
    std::size_t count = 0;
    while (reader.next(item)) {
        const auto type = item.get("kind", "").asString();
        if (type == "channel" && next.channels.size() < kMaxChannels) {
            const std::string id = item.get("id", "").asString();
            const std::string name = item.get("name", "").asString();
            if (id.size() <= 256 && !id.empty())
                next.channels.emplace(id, name.substr(0, 1536));
        } else if (type == "event" && count < kMaxEvents) {
            const std::string channel = item.get("channel", "").asString();
            if (channel.empty() || channel.size() > 256) continue;
            Event event;
            event.start = item.get("start", 0).asInt64();
            event.stop = item.get("stop", 0).asInt64();
            event.title = item.get("title", "").asString().substr(0, 1536);
            event.description = item.get("description", "").asString().substr(0, 1536);
            if (event.start > latest || event.stop < earliest ||
                event.stop <= event.start || event.title.empty()) continue;
            auto& list = next.programmes[channel];
            if (list.size() >= kMaxEventsPerChannel) continue;
            list.push_back(std::move(event));
            ++count;
        }
    }
    if (!reader.good()) {
        std::clog << "XMLTV CACHE invalid or truncated source=" << settings.id << '\n';
        return false;
    }
    if (next.channels.empty() && next.programmes.empty()) return false;
    data.channels.swap(next.channels);
    data.programmes.swap(next.programmes);
    data.lastSuccess = next.lastSuccess;
    std::clog << "XMLTV CACHE restored source=" << settings.id
              << " channels=" << data.channels.size()
              << " programmes=" << count << '\n';
    return true;
}

bool ExternalXmltvManager::persistCache(
    const XmltvSourceConfig& settings, const Source& data) {
    epgcache::Writer writer(epgcache::pathFor("xmltv", settings.id));
    if (!writer.valid()) return false;
    Json::Value header;
    header["schema"] = 1;
    header["kind"] = "xmltv";
    header["source_id"] = settings.id;
    header["url"] = settings.url;
    header["last_success"] = Json::Int64(data.lastSuccess);
    if (!writer.append(header)) return false;
    for (const auto& [id, name] : data.channels) {
        Json::Value item;
        item["kind"] = "channel";
        item["id"] = id;
        item["name"] = name;
        if (!writer.append(item)) return false;
    }
    for (const auto& [id, events] : data.programmes) {
        for (const auto& event : events) {
            Json::Value item;
            item["kind"] = "event";
            item["channel"] = id;
            item["start"] = Json::Int64(event.start);
            item["stop"] = Json::Int64(event.stop);
            item["title"] = event.title;
            item["description"] = event.description;
            if (!writer.append(item)) return false;
        }
    }
    return writer.finish();
}

ExternalXmltvManager::ExternalXmltvManager(
    const std::vector<XmltvSourceConfig>& sources) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    configure(sources);
    thread_ = std::thread(&ExternalXmltvManager::run, this);
}

ExternalXmltvManager::~ExternalXmltvManager() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    curl_global_cleanup();
}

void ExternalXmltvManager::configure(
    const std::vector<XmltvSourceConfig>& sources) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, Source> next;
    for (const auto& cfg : sources) {
        if (cfg.id.empty() || next.size() >= 32) continue;
        Source data;
        const auto it = sources_.find(cfg.id);
        if (it != sources_.end() && it->second.settings.url == cfg.url) {
            data = it->second;
            if (data.settings.refreshTime != cfg.refreshTime ||
                data.settings.refreshPeriod != cfg.refreshPeriod ||
                data.settings.refreshWeekday != cfg.refreshWeekday)
                data.lastSuccessDay = -1;
        } else {
            // Before any network traffic, rehydrate the last successfully
            // downloaded XMLTV guide. URL changes invalidate old records.
            (void)restoreCache(cfg, data);
            data.queued = cfg.enabled;
        }
        data.settings = cfg;
        next.emplace(cfg.id, std::move(data));
    }
    sources_.swap(next);
    wake_.notify_all();
}

bool ExternalXmltvManager::refresh(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool found = false;
    for (auto& [key, source] : sources_) {
        if (id.empty() || key == id) {
            source.queued = true;
            ++source.refreshRequested;
            found = true;
        }
    }
    wake_.notify_all();
    return found;
}

bool ExternalXmltvManager::download(const std::string& url,
                                    std::FILE* output, std::string& error) {
    if ((url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) ||
        url.size() > 2048) {
        error = "XMLTV URL must be HTTP(S)";
        return false;
    }
    if (!output) {
        error = "XMLTV temporary download file unavailable";
        return false;
    }
    using CurlPtr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlPtr curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) { error = "curl init failed"; return false; }
    DownloadSink sink;
    sink.file = output;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, acceptCurl);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &sink);
    // Defer gzip decompression to the bounded XML reader; also accept
    // Content-Encoding: gzip, and let libcurl perform that HTTP-layer decode.
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 45L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "DVBStreamer5-XMLTV/1.1");
#ifdef CURLOPT_PROTOCOLS_STR
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    const CURLcode code = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (sink.sizeLimit) {
        error = "XMLTV download exceeds 512 MiB disk spool limit";
        return false;
    }
    if (sink.writeErrno) {
        error = std::string("XMLTV temporary file write failed: ") +
                std::strerror(sink.writeErrno);
        return false;
    }
    if (code != CURLE_OK) {
        error = std::string("XMLTV HTTP download failed: ") +
                curl_easy_strerror(code) +
                " (received " + std::to_string(sink.total) + " bytes)";
        return false;
    }
    if (status != 200 || sink.total == 0) {
        error = "XMLTV HTTP status " + std::to_string(status) +
                (sink.total ? "" : " (empty response)");
        return false;
    }
    if (std::fflush(output) != 0) {
        error = std::string("XMLTV temporary file flush failed: ") +
                std::strerror(errno);
        return false;
    }
    if (std::fseek(output, 0, SEEK_SET) != 0) {
        error = "XMLTV temporary file rewind failed";
        return false;
    }
    return true;
}

bool ExternalXmltvManager::parse(
    std::FILE* input, Source& data, std::string& error) {
    if (!input) { error = "XMLTV temporary file is missing"; return false; }
    // Dup is essential: gzclose() belongs to libxml2 while the original
    // descriptor is owned by the scoped tmpfile and closed after parsing.
    const int fd = ::dup(::fileno(input));
    if (fd < 0) {
        error = std::string("XMLTV stream dup failed: ") + std::strerror(errno);
        return false;
    }
    XmlInput xmlInput;
    xmlInput.file = gzdopen(fd, "rb");
    if (!xmlInput.file) {
        ::close(fd);
        error = "XMLTV gzip/stream reader initialization failed";
        return false;
    }
    gzbuffer(xmlInput.file, 64U * 1024U);
    xmlTextReaderPtr reader = xmlReaderForIO(
        xmlReadCallback, xmlCloseCallback, &xmlInput, nullptr, nullptr,
        XML_PARSE_NONET | XML_PARSE_NOERROR |
        XML_PARSE_NOWARNING | XML_PARSE_COMPACT);
    if (!reader) {
        if (xmlInput.file) xmlCloseCallback(&xmlInput);
        error = "XMLTV streaming XML parser initialization failed";
        return false;
    }
    std::size_t totalEvents = 0;
    std::size_t totalChannels = 0;
    std::size_t scannedEvents = 0;
    const std::int64_t now = nowSeconds();
    const std::int64_t startWindow = now - 6 * 3600;
    const std::int64_t endWindow = now + 7 * 86400;
    std::string channelId, channelName;
    std::string programmeId;
    Event event;
    bool inChannel = false, inProgramme = false;
    int status = 0;
    while ((status = xmlTextReaderRead(reader)) == 1) {
        const char* raw = reinterpret_cast<const char*>(xmlTextReaderConstLocalName(reader));
        if (!raw) continue;
        const std::string tag(raw);
        const int type = xmlTextReaderNodeType(reader);
        if (type == XML_READER_TYPE_ELEMENT) {
            if (tag == "channel") {
                channelId = xmlProperty(reader, "id");
                channelName.clear();
                inChannel = !channelId.empty() && channelId.size() <= 256;
                if (inChannel && xmlTextReaderIsEmptyElement(reader)) {
                    if (data.channels.size() < kMaxChannels) {
                        data.channels.emplace(channelId, channelId);
                        ++totalChannels;
                    }
                    inChannel = false;
                }
            } else if (tag == "programme") {
                programmeId = xmlProperty(reader, "channel");
                const std::string start = xmlProperty(reader, "start");
                const std::string stop = xmlProperty(reader, "stop");
                event = {};
                inProgramme = !programmeId.empty() &&
                    programmeId.size() <= 256 &&
                    xmltvTime(start, event.start) &&
                    xmltvTime(stop, event.stop) &&
                    event.stop > event.start;
            } else if (tag == "display-name" && inChannel && channelName.empty()) {
                channelName = xmlValue(reader);
            } else if (tag == "title" && inProgramme && event.title.empty()) {
                event.title = xmlValue(reader);
            } else if (tag == "desc" && inProgramme && event.description.empty()) {
                event.description = xmlValue(reader);
            }
        } else if (type == XML_READER_TYPE_END_ELEMENT) {
            if (tag == "channel" && inChannel) {
                if (data.channels.size() < kMaxChannels) {
                    data.channels[channelId] = channelName.empty()
                        ? channelId : channelName;
                    ++totalChannels;
                }
                inChannel = false;
            } else if (tag == "programme") {
                ++scannedEvents;
                if (scannedEvents > kMaxScannedEvents) {
                    error = "XMLTV contains more than 5 million programmes";
                    break;
                }
                if (inProgramme && !event.title.empty() &&
                    event.stop >= startWindow && event.start <= endWindow) {
                    // Some providers omit <channel> definitions entirely.
                    // Keep the programme ID selectable even if EPG is capped.
                    if (data.channels.size() < kMaxChannels &&
                        !data.channels.count(programmeId))
                        data.channels.emplace(programmeId, programmeId);
                    auto& programmes = data.programmes[programmeId];
                    if (totalEvents < kMaxEvents &&
                        programmes.size() < kMaxEventsPerChannel) {
                        programmes.push_back(std::move(event));
                        ++totalEvents;
                    } else {
                        ++data.truncatedEvents;
                    }
                }
                inProgramme = false;
            }
        }
    }
    xmlFreeTextReader(reader); // invokes xmlCloseCallback
    if (xmlInput.file) xmlCloseCallback(&xmlInput);
    if (xmlInput.sizeLimit) {
        error = "XMLTV uncompressed content exceeds 2 GiB safety limit";
        return false;
    }
    if (xmlInput.readError) {
        error = "XMLTV stream/gzip read error (truncated or corrupt data)";
        return false;
    }
    if (status < 0 || !error.empty()) {
        if (error.empty()) error = "Malformed XMLTV XML document";
        return false;
    }
    if (totalChannels == 0 && totalEvents == 0) {
        error = "XMLTV has no channels or programmes";
        return false;
    }
    for (auto& [id, events] : data.programmes) {
        std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
            return a.start < b.start;
        });
    }
    return true;
}

void ExternalXmltvManager::run() {
    for (;;) {
        XmltvSourceConfig selected;
        bool selectedScheduled = false;
        std::uint64_t selectedRefreshId = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) break;
            const std::time_t timestamp = std::time(nullptr);
            const std::tm local = localDay(timestamp);
            const int day = dayStamp(local);
            const std::int64_t now = nowSeconds();
            for (auto& [id, source] : sources_) {
                // Manual refresh is allowed for a disabled source, but the
                // source remains unavailable for EPG until enabled.
                if (!source.settings.enabled && !source.queued) continue;
                const bool scheduleDay = source.settings.refreshPeriod != "weekly" ||
                    local.tm_wday == source.settings.refreshWeekday;
                const bool scheduled = scheduleDay &&
                    source.lastSuccessDay != day &&
                    dueForDailyRun(source.settings.refreshTime, local);
                const bool first = source.lastAttempt == 0;
                const bool retryAfterError = !source.error.empty() &&
                    now - source.lastAttempt >= 15 * 60;
                if (!source.queued && !first && !scheduled && !retryAfterError)
                    continue;
                // After a network error, back off even if scheduled/queued.
                if (source.lastAttempt && !source.queued &&
                    !source.error.empty() &&
                    now - source.lastAttempt < 15 * 60) continue;
                source.queued = false;
                source.lastAttempt = now;
                source.downloading = true;
                selected = source.settings;
                selectedScheduled = scheduled;
                selectedRefreshId = source.refreshRequested;
                break;
            }
            if (selected.id.empty()) {
                wake_.wait_for(lock, std::chrono::seconds(15));
                continue;
            }
        }
        std::string error;
        Source loaded;
        loaded.settings = selected;
        std::unique_ptr<std::FILE, decltype(&std::fclose)> temp(
            std::tmpfile(), &std::fclose);
        if (!temp) {
            error = std::string("XMLTV cannot create temporary spool file: ") +
                    std::strerror(errno);
        }
        bool success = static_cast<bool>(temp);
        if (success) success = download(selected.url, temp.get(), error);
        if (success) success = parse(temp.get(), loaded, error);
        temp.reset(); // delete temporary file immediately after XML parsing
        const std::int64_t finished = nowSeconds();
        if (success) {
            loaded.lastSuccess = finished;
            // Only replace a good cache after a completely parsed feed.
            // Disk errors do not throw away freshly downloaded EPG.
            if (!persistCache(selected, loaded))
                std::clog << "XMLTV CACHE write failed source=" << selected.id << '\n';
        }
        const int day = dayStamp(localDay(static_cast<std::time_t>(finished)));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = sources_.find(selected.id);
            if (it == sources_.end() || it->second.settings.url != selected.url)
                continue;
            auto& source = it->second;
            source.downloading = false;
            source.refreshCompleted = std::max(
                source.refreshCompleted, selectedRefreshId);
            if (success) {
                source.channels.swap(loaded.channels);
                source.programmes.swap(loaded.programmes);
                source.truncatedEvents = loaded.truncatedEvents;
                source.lastSuccess = finished;
                // Startup prefetch before HH:MM is not the day's scheduled
                // refresh. Preserve the scheduled run even if prefetch finishes
                // after the clock rolls past the requested time.
                if (selectedScheduled) source.lastSuccessDay = day;
                source.error.clear();
                std::clog << "EXTERNAL XMLTV updated source=" << selected.id
                          << " channels=" << source.channels.size()
                          << " programmes=" << source.programmes.size()
                          << " capped_events=" << source.truncatedEvents << '\n';
            } else {
                source.error = error;
                std::clog << "EXTERNAL XMLTV error source=" << selected.id
                          << " reason=" << error << '\n';
            }
        }
    }
}

Json::Value ExternalXmltvManager::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value root(Json::objectValue);
    Json::Value sources(Json::arrayValue);
    for (const auto& [id, source] : sources_) {
        Json::Value entry = source.settings.toJson();
        entry["last_success"] = Json::Int64(source.lastSuccess);
        entry["last_attempt"] = Json::Int64(source.lastAttempt);
        entry["downloading"] = source.downloading;
        entry["queued"] = source.queued;
        entry["refresh_requested"] = Json::UInt64(source.refreshRequested);
        entry["refresh_completed"] = Json::UInt64(source.refreshCompleted);
        entry["last_error"] = source.error;
        entry["channel_count"] = Json::UInt64(source.channels.size());
        std::size_t events = 0;
        for (const auto& [channelId, list] : source.programmes) events += list.size();
        entry["event_count"] = Json::UInt64(events);
        entry["truncated_events"] = Json::UInt64(source.truncatedEvents);
        entry["last_warning"] = source.truncatedEvents
            ? "XMLTV EPG events outside retention caps: " +
                std::to_string(source.truncatedEvents) + " omitted" : "";
        sources.append(std::move(entry));
    }
    root["sources"] = std::move(sources);
    return root;
}

Json::Value ExternalXmltvManager::channels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value root(Json::arrayValue);
    for (const auto& [id, source] : sources_) {
        for (const auto& [channelId, name] : source.channels) {
            if (root.size() >= 15000) break;
            Json::Value item;
            item["source_id"] = id;
            item["id"] = channelId;
            item["name"] = name;
            root.append(std::move(item));
        }
    }
    return root;
}

Json::Value ExternalXmltvManager::snapshot(
    const std::string& sourceId, const std::string& channelId,
    unsigned days) const {
    Json::Value events(Json::arrayValue);
    if (channelId.empty()) return events;
    const std::int64_t now = nowSeconds();
    const std::int64_t minTime = now - 6 * 3600;
    const std::int64_t maxTime = now +
        static_cast<std::int64_t>(std::clamp(days, 1U, 7U)) * 86400;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, source] : sources_) {
        if ((!sourceId.empty() && sourceId != id) || !source.settings.enabled)
            continue;
        const auto found = source.programmes.find(channelId);
        if (found == source.programmes.end()) continue;
        for (const auto& event : found->second) {
            if (event.stop < minTime || event.start > maxTime) continue;
            Json::Value item;
            item["service_id"] = Json::UInt(0);
            item["event_id"] = Json::UInt(0);
            item["start_utc"] = Json::Int64(event.start);
            item["end_utc"] = Json::Int64(event.stop);
            item["duration_seconds"] = Json::UInt64(event.stop - event.start);
            item["title"] = event.title;
            item["description"] = event.description;
            events.append(std::move(item));
            if (events.size() >= 512) break;
        }
        // In auto-source mode, use the first source with this channel ID.
        if (!events.empty()) break;
    }
    return events;
}
