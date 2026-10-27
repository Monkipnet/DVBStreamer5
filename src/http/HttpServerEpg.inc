// EPG sidecars for every supported output transport.  Media pipelines, PCR,
// EIT PID filtering and existing DVB remapping are intentionally untouched.
#include <ctime>
#include <functional>
#include <limits>

#ifdef DVBSTREAMER5_HAVE_EPG_ZLIB
#include <zlib.h>
#endif

namespace {

std::string epgXmlEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 12U);
    for (unsigned char ch : value) {
        switch (ch) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default:
                // XML 1.0 prohibits C0 controls except TAB/LF/CR.
                if (ch >= 0x20U || ch == '\t' || ch == '\r' || ch == '\n')
                    result += static_cast<char>(ch);
        }
    }
    return result;
}

std::string epgM3uAttribute(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\r' || ch == '\n' || ch < 0x20U)
            result.push_back(' ');
        else result.push_back(static_cast<char>(ch));
    }
    return result;
}

std::string epgM3uLine(const std::string& value) {
    std::string result = value;
    for (char& ch : result) if (ch == '\r' || ch == '\n') ch = ' ';
    return result;
}

std::string epgXmlTime(Json::Int64 seconds) {
    if (seconds <= 0) return {};
    const auto when = static_cast<std::time_t>(seconds);
    if (static_cast<Json::Int64>(when) != seconds) return {};
    std::tm utc {};
#ifdef _WIN32
    if (gmtime_s(&utc, &when) != 0) return {};
#else
    if (!gmtime_r(&when, &utc)) return {};
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y%m%d%H%M%S +0000");
    return out.str();
}

std::string epgChannelId(const StreamConfig& cfg) {
    // One channel identifier for every configured output of a service. This
    // deliberately stays stable when its output SID or protocol changes.
    return cfg.id;
}

std::string epgChannelName(const StreamConfig& cfg) {
    return !cfg.serviceName.empty() ? cfg.serviceName :
        (!cfg.name.empty() ? cfg.name : cfg.id);
}

using EpgAllowed = std::function<bool(const std::string&)>;

std::string buildEpgXmltv(
    const std::vector<StreamConfig>& streams,
    StreamManager& manager, const EpgAllowed& allowed) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n"
        << "<tv generator-info-name=\"DVBStreamer5\">\n";

    // An enabled channel must remain listed even before EIT becomes available.
    for (const auto& cfg : streams) {
        if (!cfg.epgEnabled || cfg.id.empty() || !allowed(cfg.id)) continue;
        xml << "  <channel id=\"" << epgXmlEscape(epgChannelId(cfg)) << "\">\n"
            << "    <display-name>" << epgXmlEscape(epgChannelName(cfg))
            << "</display-name>\n"
            << "  </channel>\n";
    }

    for (const auto& cfg : streams) {
        if (!cfg.epgEnabled || cfg.id.empty() || !allowed(cfg.id)) continue;
        const Json::Value snapshot = manager.epgSnapshot(cfg.id);
        if (!snapshot.isMember("events") || !snapshot["events"].isArray())
            continue;
        for (const auto& event : snapshot["events"]) {
            if (!event.isObject()) continue;
            const Json::Int64 start = event.get("start_utc", 0).asInt64();
            const Json::Int64 stop = event.get("end_utc", 0).asInt64();
            if (stop <= start || event.get("title", "").asString().empty()) continue;
            const std::string startText = epgXmlTime(start);
            const std::string stopText = epgXmlTime(stop);
            if (startText.empty() || stopText.empty()) continue;
            xml << "  <programme start=\"" << startText
                << "\" stop=\"" << stopText
                << "\" channel=\"" << epgXmlEscape(epgChannelId(cfg)) << "\">\n"
                << "    <title lang=\"und\">"
                << epgXmlEscape(event.get("title", "").asString())
                << "</title>\n";
            const std::string desc = event.get("description", "").asString();
            if (!desc.empty())
                xml << "    <desc lang=\"und\">" << epgXmlEscape(desc)
                    << "</desc>\n";
            xml << "  </programme>\n";
        }
    }
    xml << "</tv>\n";
    return xml.str();
}

// Use only a validated Host supplied by the client. No file paths, arbitrary
// URLs or upstream credentials are ever copied into an advertised EPG URL.
std::string epgHttpBase(const std::string& host, int defaultPort) {
    const bool valid = !host.empty() && host.size() <= 255U &&
        std::all_of(host.begin(), host.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-' || c == '_' ||
                   c == ':' || c == '[' || c == ']';
        });
    return "http://" + (valid ? host :
        ("127.0.0.1:" + std::to_string(defaultPort)));
}

std::string buildEpgIptvPlaylist(
    const std::vector<StreamConfig>& streams,
    const EpgAllowed& allowed, const std::string& httpBase,
    int httpPort) {
    std::ostringstream playlist;
    playlist << "#EXTM3U url-tvg=\"" << epgM3uAttribute(httpBase)
             << "/epg.xml\" x-tvg-url=\"" << epgM3uAttribute(httpBase)
             << "/epg.xml\"\n";
    for (const auto& cfg : streams) {
        if (cfg.id.empty() || !allowed(cfg.id)) continue;
        const auto outputs = streamOutputs(cfg);
        for (const auto& output : outputs) {
            const std::string type = normalizedOutputType(output);
            // RTMP/YouTube is a push endpoint, possibly with a private stream
            // key, not a generally playable IPTV client URL.
            if (type == "youtube" || type == "rtmp") continue;
            const std::string url = streamLink(output, httpPort);
            if (url.empty()) continue;
            const std::string name = epgChannelName(cfg);
            const std::uint32_t sid = cfg.remapEnabled ? cfg.serviceId : cfg.inputServiceId;
            playlist << "#EXTINF:-1 tvg-id=\""
                     << epgM3uAttribute(epgChannelId(cfg))
                     << "\" tvg-name=\"" << epgM3uAttribute(name)
                     << "\" group-title=\"DVBStreamer5\"";
            if (sid > 0 && sid <= 65535U)
                playlist << " tvg-sid=\"" << sid << "\"";
            playlist << "," << epgM3uLine(name)
                     << (outputs.size() > 1U ? " [" + type + "]" : "")
                     << "\n" << epgM3uLine(url) << "\n";
        }
    }
    return playlist.str();
}

bool gzipEpgXmltv(const std::string& xml, std::string& output) {
#ifdef DVBSTREAMER5_HAVE_EPG_ZLIB
    if (xml.size() > static_cast<std::size_t>(std::numeric_limits<uInt>::max()))
        return false;
    z_stream zs {};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    output.resize(static_cast<std::size_t>(compressBound(
        static_cast<uLong>(xml.size()))) + 32U);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(xml.data()));
    zs.avail_in = static_cast<uInt>(xml.size());
    zs.next_out = reinterpret_cast<Bytef*>(output.data());
    zs.avail_out = static_cast<uInt>(output.size());
    const int result = deflate(&zs, Z_FINISH);
    const bool ok = result == Z_STREAM_END;
    if (ok) output.resize(zs.total_out);
    else output.clear();
    deflateEnd(&zs);
    return ok;
#else
    (void)xml;
    (void)output;
    return false;
#endif
}

} // namespace
