#include "ExternalXmltv.h"

#include <curl/curl.h>
#include <libxml/xmlreader.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>

namespace {
constexpr std::size_t kMaxDownload = 32U * 1024U * 1024U;
constexpr std::size_t kMaxXml = 64U * 1024U * 1024U;
constexpr std::size_t kMaxEvents = 180000U;
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

size_t acceptCurl(void* bytes, size_t elementSize, size_t count, void* user) {
    auto* body = static_cast<std::string*>(user);
    if (!elementSize || count > std::numeric_limits<size_t>::max() / elementSize)
        return 0;
    const size_t n = elementSize * count;
    if (n > kMaxDownload || body->size() > kMaxDownload - n) return 0;
    body->append(static_cast<const char*>(bytes), n);
    return n;
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
    if (result.size() > 4096) {
        // Never split a multibyte UTF-8 character in long descriptions.
        std::size_t length = 4096;
        while (length > 0 &&
               (static_cast<unsigned char>(result[length]) & 0xc0U) == 0x80U)
            --length;
        result.resize(length);
    }
    return result;
}

bool unpackGzip(std::string& input, std::string& error) {
    if (input.size() < 2 ||
        static_cast<unsigned char>(input[0]) != 0x1f ||
        static_cast<unsigned char>(input[1]) != 0x8b) return true;
    z_stream zs {};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {
        error = "gzip inflate initialization failed"; return false;
    }
    std::string decoded;
    decoded.reserve(std::min(kMaxXml, input.size() * 4U));
    zs.next_in = reinterpret_cast<Bytef*>(input.data());
    zs.avail_in = static_cast<uInt>(input.size());
    int status = Z_OK;
    char chunk[32768];
    while (status == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(chunk);
        zs.avail_out = sizeof(chunk);
        status = inflate(&zs, Z_NO_FLUSH);
        const size_t n = sizeof(chunk) - zs.avail_out;
        if (decoded.size() > kMaxXml - n) {
            error = "XMLTV uncompressed size exceeds 64 MiB";
            inflateEnd(&zs);
            return false;
        }
        decoded.append(chunk, n);
    }
    inflateEnd(&zs);
    if (status != Z_STREAM_END) {
        error = "invalid or truncated XMLTV gzip";
        return false;
    }
    input.swap(decoded);
    return true;
}
} // namespace

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
            found = true;
        }
    }
    wake_.notify_all();
    return found;
}

bool ExternalXmltvManager::download(const std::string& url,
                                     std::string& body, std::string& error) {
    if ((url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) ||
        url.size() > 2048) {
        error = "XMLTV URL must be HTTP(S)";
        return false;
    }
    using CurlPtr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlPtr curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) { error = "curl init failed"; return false; }
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, acceptCurl);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 55L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "DVBStreamer5-XMLTV/1.0");
#ifdef CURLOPT_PROTOCOLS_STR
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    const CURLcode code = curl_easy_perform(curl.get());
    long httpStatus = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpStatus);
    if (code != CURLE_OK) {
        error = std::string("XMLTV HTTP download failed: ") +
                curl_easy_strerror(code);
        return false;
    }
    if (httpStatus != 200 || body.empty()) {
        error = "XMLTV HTTP status " + std::to_string(httpStatus) +
                (body.empty() ? " (empty)" : "");
        return false;
    }
    if (body.size() > kMaxXml) { error = "XMLTV too large"; return false; }
    return unpackGzip(body, error);
}

bool ExternalXmltvManager::parse(
    const std::string& body, Source& data, std::string& error) {
    xmlTextReaderPtr reader = xmlReaderForMemory(
        body.data(), static_cast<int>(body.size()), nullptr, nullptr,
        XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
        XML_PARSE_COMPACT);
    if (!reader) { error = "XMLTV XML parser initialization failed"; return false; }

    std::size_t totalEvents = 0;
    std::size_t totalChannels = 0;
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
            } else if (tag == "programme" && inProgramme) {
                if (totalEvents < kMaxEvents && !event.title.empty()) {
                    // Some providers omit <channel> definitions entirely.
                    // Keep the programme ID selectable in the UI in that case.
                    if (data.channels.size() < kMaxChannels &&
                        !data.channels.count(programmeId))
                        data.channels.emplace(programmeId, programmeId);
                    data.programmes[programmeId].push_back(std::move(event));
                    ++totalEvents;
                }
                inProgramme = false;
            }
        }
        if (totalEvents >= kMaxEvents) {
            error = "XMLTV exceeded event count safety limit (180000)";
            break;
        }
    }
    xmlFreeTextReader(reader);
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
                break;
            }
            if (selected.id.empty()) {
                wake_.wait_for(lock, std::chrono::seconds(15));
                continue;
            }
        }
        std::string payload;
        std::string error;
        Source loaded;
        loaded.settings = selected;
        bool success = download(selected.url, payload, error);
        if (success) success = parse(payload, loaded, error);
        const std::int64_t finished = nowSeconds();
        const int day = dayStamp(localDay(static_cast<std::time_t>(finished)));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = sources_.find(selected.id);
            if (it == sources_.end() || it->second.settings.url != selected.url)
                continue;
            auto& source = it->second;
            source.downloading = false;
            if (success) {
                source.channels.swap(loaded.channels);
                source.programmes.swap(loaded.programmes);
                source.lastSuccess = finished;
                // Startup prefetch before HH:MM is not the day's scheduled
                // refresh. Preserve the scheduled run even if prefetch finishes
                // after the clock rolls past the requested time.
                if (selectedScheduled) source.lastSuccessDay = day;
                source.error.clear();
                std::clog << "EXTERNAL XMLTV updated source=" << selected.id
                          << " channels=" << source.channels.size()
                          << " programmes=" << source.programmes.size() << '\n';
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
        entry["last_error"] = source.error;
        entry["channel_count"] = Json::UInt64(source.channels.size());
        std::size_t events = 0;
        for (const auto& [channelId, list] : source.programmes) events += list.size();
        entry["event_count"] = Json::UInt64(events);
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
