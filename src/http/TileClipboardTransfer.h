#pragma once
// V588: portable, non-secret, single-tile clipboard format.
// The full DVBStreamer5 backup is intentionally NOT a tile-transfer format.
#include <jsoncpp/json/json.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace dvbstreamer5::http::tile_clipboard {

inline constexpr const char* kSchema = "DVBStreamer5/tile";
inline constexpr int kFormat = 1;

inline void warn(Json::Value& warnings, const std::string& value) {
    if (warnings.size() < 40U) warnings.append(value);
}
inline bool isDvbUri(const std::string& value) {
    return value.rfind("dvb://", 0) == 0 ||
           value.rfind("t2mi://", 0) == 0;
}
inline std::string safeUri(std::string uri, Json::Value& warnings,
                           const std::string& field) {
    if (isDvbUri(uri)) {
        warn(warnings, field + ": check tuner, adapter and frontend on target");
        return uri; // Tuning options and #pnr are required DVB URI syntax.
    }
    const auto scheme = uri.find("://");
    if (scheme != std::string::npos) {
        const auto authority = scheme + 3U;
        const auto end = uri.find_first_of("/?#", authority);
        const auto at = uri.find('@', authority);
        if (at != std::string::npos &&
            (end == std::string::npos || at < end)) {
            uri.erase(authority, at - authority + 1U);
            warn(warnings, field + ": URL username/password omitted");
        }
        // Never include bearer tokens / API keys / SRT streamid options from
        // source URL queries or fragments in the system clipboard.
        const auto query = uri.find_first_of("?#", authority);
        if (query != std::string::npos) {
            uri.erase(query);
            warn(warnings, field + ": URL query/fragment omitted; review before starting");
        }
    }
    return uri;
}
inline Json::Value scrubStream(Json::Value tile, Json::Value& warnings) {
    if (!tile.isObject()) return Json::Value(Json::nullValue);
    tile["id"] = "";
    tile["auto_start"] = false; // Clipboard import must never auto-start.
    tile["srt_input_passphrase"] = "";
    tile["srt_input_streamid"] = "";
    tile["srt_output_passphrase"] = "";
    tile["srt_output_streamid"] = "";
    tile["hls_access_key_value"] = "";
    tile["hls_access_key_mode"] = "none";
    tile["hls_encryption_key_hex"] = "";
    tile["hls_encryption"] = "none";
    tile["hls_encryption_key_uri"] = "key.bin";
    tile["conditional_access_client"] = "";
    tile["interface_address"] = "";
    tile["input_interface_address"] = "";
    tile["stream_auth_mode"] = "inherit";
    tile["xmltv_source_id"] = "";
    // Local file archive directories cannot be assumed to exist on target.
    tile["hls_archive_enabled"] = false;
    tile["hls_archive_path"] = "/var/lib/dvbstreamer5/archive";
    tile["input_uri"] = safeUri(tile.get("input_uri", "").asString(),
                                warnings, "input_uri");
    tile["backup_input_uri"] = safeUri(tile.get("backup_input_uri", "").asString(),
                                       warnings, "backup_input_uri");
    auto& outputs = tile["additional_outputs"];
    if (outputs.isArray()) {
        for (auto& output : outputs) {
            if (!output.isObject()) continue;
            output["interface_address"] = "";
            output["srt_passphrase"] = "";
            output["srt_streamid"] = "";
        }
    }
    warn(warnings, "Credentials, CAM, XMLTV provider, interface addresses and autostart omitted");
    warn(warnings, "Check target output addresses/ports and decoder/GPU before starting");
    return tile;
}
inline Json::Value scrubWisi(Json::Value tile, Json::Value& warnings) {
    if (!tile.isObject()) return Json::Value(Json::nullValue);
    tile["id"] = "";
    tile["enabled"] = false;
    tile["auto_start"] = false;
    tile["interface_address"] = "";
    warn(warnings, "WISI EPG remains stopped; verify multicast IP/port and XMLTV mappings");
    return tile;
}
inline Json::Value scrubMpts(Json::Value tile, Json::Value& warnings) {
    if (!tile.isObject()) return Json::Value(Json::nullValue);
    tile["id"] = "";
    tile["auto_start"] = false;
    tile["interface_address"] = "";
    warn(warnings, "MPTS remains stopped. Reassign channels to local stream IDs before starting");
    warn(warnings, "Verify destination multicast IP, port, SID/PMT mapping and output interface");
    return tile;
}
inline Json::Value envelope(const std::string& kind, Json::Value tile) {
    Json::Value output(Json::objectValue), warnings(Json::arrayValue);
    output["schema"] = kSchema;
    output["format_version"] = kFormat;
    output["kind"] = kind;
    if (kind == "stream") output["tile"] = scrubStream(std::move(tile), warnings);
    else if (kind == "wisi_epg") output["tile"] = scrubWisi(std::move(tile), warnings);
    else if (kind == "mpts") output["tile"] = scrubMpts(std::move(tile), warnings);
    else output["tile"] = Json::nullValue;
    output["warnings"] = warnings;
    return output;
}
inline bool isEnvelope(const Json::Value& root) {
    if (!root.isObject() || root.get("schema", "").asString() != kSchema ||
        root.get("format_version", 0).asInt() != kFormat ||
        !root["tile"].isObject()) return false;
    const auto kind = root.get("kind", "").asString();
    return kind == "stream" || kind == "wisi_epg" || kind == "mpts";
}
} // namespace dvbstreamer5::http::tile_clipboard
