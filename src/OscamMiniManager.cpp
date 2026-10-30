#include "OscamMiniManager.h"
#include "ConfigManager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
using IniSection = std::map<std::string, std::string>;

std::string trimLocal(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return !std::isspace(c);
    }).base(), value.end());
    return value;
}

std::vector<IniSection> parseIniSections(const std::string& text, const std::string& wanted) {
    std::vector<IniSection> result;
    std::istringstream input(text);
    std::string line;
    IniSection* current = nullptr;

    while (std::getline(input, line)) {
        auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        line = trimLocal(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            const std::string sectionName = trimLocal(line.substr(1, line.size() - 2));
            if (sectionName == wanted) {
                result.emplace_back();
                current = &result.back();
            } else {
                current = nullptr;
            }
            continue;
        }

        if (!current) {
            continue;
        }

        auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        (*current)[trimLocal(line.substr(0, equals))] = trimLocal(line.substr(equals + 1));
    }
    return result;
}

IniSection parseFirstSection(const std::string& text, const std::string& wanted) {
    auto sections = parseIniSections(text, wanted);
    return sections.empty() ? IniSection{} : sections.front();
}

std::string getValue(const IniSection& section, const std::string& key, const std::string& fallback = {}) {
    auto it = section.find(key);
    return it == section.end() ? fallback : it->second;
}

int getInt(const IniSection& section, const std::string& key, int fallback) {
    try {
        return std::stoi(getValue(section, key, std::to_string(fallback)));
    } catch (...) {
        return fallback;
    }
}

bool isTrue(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value == "1" || value == "yes" || value == "true" || value == "on";
}

std::string safeIni(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
    return trimLocal(value);
}

bool isHex(const std::string& value, std::size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool isSafeName(const std::string& value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '@';
    });
}

bool validGroups(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    bool expectDigit = true;
    int current = 0;
    for (unsigned char c : value) {
        if (std::isdigit(c)) {
            current = current * 10 + (c - '0');
            if (current > 64) {
                return false;
            }
            expectDigit = false;
        } else if (c == ',') {
            if (expectDigit || current < 1) {
                return false;
            }
            current = 0;
            expectDigit = true;
        } else if (!std::isspace(c)) {
            return false;
        }
    }
    return !expectDigit && current >= 1 && current <= 64;
}

struct PortDef {
    int port = 0;
    std::string caid;
    std::string provider;
};

std::vector<PortDef> parsePortDefs(const std::string& value) {
    std::vector<PortDef> result;
    std::istringstream input(value);
    std::string item;
    while (std::getline(input, item, ';')) {
        item = trimLocal(item);
        if (item.empty()) {
            continue;
        }
        auto at = item.find('@');
        auto colon = item.find(':', at == std::string::npos ? 0 : at + 1);
        if (at == std::string::npos || colon == std::string::npos) {
            continue;
        }
        PortDef def;
        try {
            def.port = std::stoi(trimLocal(item.substr(0, at)));
        } catch (...) {
            continue;
        }
        def.caid = trimLocal(item.substr(at + 1, colon - at - 1));
        def.provider = trimLocal(item.substr(colon + 1));
        if (def.port > 0 && def.port <= 65535 && isHex(def.caid, 4) && isHex(def.provider, 6)) {
            result.push_back(def);
        }
    }
    return result;
}

std::pair<std::string, std::string> accountCaidProvider(const IniSection& account) {
    std::string caid = getValue(account, "caid", "");
    std::string provider = "";
    std::string ident = getValue(account, "ident", "");
    if (!ident.empty()) {
        auto colon = ident.find(':');
        if (colon != std::string::npos) {
            if (caid.empty()) {
                caid = trimLocal(ident.substr(0, colon));
            }
            provider = trimLocal(ident.substr(colon + 1));
            auto comma = provider.find(',');
            if (comma != std::string::npos) {
                provider.resize(comma);
            }
        }
    }
    return {caid, provider};
}

std::string normalizedCamToken(std::string value) {
    std::string result;
    bool separator = false;
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            result.push_back(static_cast<char>(std::tolower(c)));
            separator = false;
        } else if (!result.empty() && !separator) {
            result.push_back('_');
            separator = true;
        }
    }
    while (!result.empty() && result.back() == '_') result.pop_back();
    return result;
}

bool isLoopbackNewcamdHost(std::string host) {
    host = trimLocal(std::move(host));
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

bool isRemoteReaderProtocol(const std::string& protocol) {
    static const std::set<std::string> protocols = {
        "newcamd", "newcamd524", "newcamd525",
        "camd35", "cs357x", "cs378x",
        "cccam", "cccam_ext",
        "radegast", "gbox", "scam", "ghttp", "camd33"
    };
    return protocols.find(protocol) != protocols.end();
}

bool remoteReaderNeedsDesKey(const std::string& protocol) {
    return protocol == "newcamd" || protocol == "newcamd524" || protocol == "newcamd525";
}

bool remoteReaderNeedsCredentials(const std::string& protocol) {
    return remoteReaderNeedsDesKey(protocol) ||
           protocol == "camd35" || protocol == "cs357x" || protocol == "cs378x" ||
           protocol == "cccam" || protocol == "cccam_ext" ||
           protocol == "scam" || protocol == "ghttp";
}

bool validCaidTable(const std::string& value) {
    if (value.empty() || value.size() > 256) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) || ch == ',' || ch == ':' || ch == '&';
    });
}

Json::Value parseCamBackendConfigText(const std::string& text) {
    Json::Value parsed(Json::objectValue);
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(text.empty() ? "{}" : text);
    if (!Json::parseFromStream(builder, input, &parsed, &errors) || !parsed.isObject()) {
        return Json::Value(Json::objectValue);
    }
    return parsed;
}

std::string providerFromReaderIdent(const std::string& ident, const std::string& caid) {
    std::string first = trimLocal(ident);
    const auto comma = first.find(',');
    if (comma != std::string::npos) first.resize(comma);
    const auto colon = first.find(':');
    if (colon == std::string::npos) return "000000";
    const std::string identCaid = trimLocal(first.substr(0, colon));
    const std::string provider = trimLocal(first.substr(colon + 1));
    if ((!identCaid.empty() && normalizedCamToken(identCaid) != normalizedCamToken(caid)) ||
        !isHex(provider, 6)) {
        return "000000";
    }
    return provider;
}

}

OscamMiniManager& OscamMiniManager::instance() {
    static OscamMiniManager instance;
    return instance;
}

std::string OscamMiniManager::configDir() {
    const char* value = std::getenv("DVBSTREAMER5_OSCAM_CONFIG_DIR");
    if (value && *value && fs::path(value).is_absolute()) return value;
    return "/etc/dvbstreamer5/oscam-mini";
}

std::string OscamMiniManager::binaryPath() {
    const char* value = std::getenv("DVBSTREAMER5_OSCAM_BINARY");
    if (value && *value && fs::path(value).is_absolute()) return value;
    return "/opt/DVBStreamer5/oscam-mini/oscam-mini";
}

std::string OscamMiniManager::trim(std::string value) {
    return trimLocal(std::move(value));
}

std::string OscamMiniManager::readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

bool OscamMiniManager::writeAtomic(const std::string& path, const std::string& content, std::string& error) {
    try {
        fs::create_directories(fs::path(path).parent_path());
        const std::string temp = path + ".tmp." + std::to_string(getpid());
        {
            std::ofstream file(temp, std::ios::trunc);
            if (!file) {
                error = "Не удалось создать временный файл " + temp;
                return false;
            }
            file << content;
            if (!file.good()) {
                error = "Ошибка записи " + temp;
                return false;
            }
        }
        fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
        fs::rename(temp, path);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::string OscamMiniManager::run(const std::string& command, int* rc) {
    std::array<char, 512> buffer{};
    std::string output;
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) {
        if (rc) {
            *rc = -1;
        }
        return "popen failed";
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    int status = pclose(pipe);
    if (rc) {
        *rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return output;
}

std::vector<std::string> OscamMiniManager::ttyDevices() {
    std::vector<std::string> stableDevices;
    std::vector<std::string> fallbackDevices;
    std::set<std::string> representedTargets;

    const auto resolvedKey = [](const fs::path& path) {
        std::error_code ec;
        const fs::path resolved = fs::canonical(path, ec);
        return ec ? path.string() : resolved.string();
    };

    try {
        if (fs::exists("/dev/serial/by-id")) {
            for (const auto& entry : fs::directory_iterator("/dev/serial/by-id")) {
                const std::string path = entry.path().string();
                const std::string target = resolvedKey(entry.path());
                if (representedTargets.insert(target).second) stableDevices.push_back(path);
            }
        }
        if (fs::exists("/dev")) {
            for (const auto& entry : fs::directory_iterator("/dev")) {
                const std::string name = entry.path().filename().string();
                if (name.rfind("ttyUSB", 0) != 0 && name.rfind("ttyACM", 0) != 0) continue;
                const std::string target = resolvedKey(entry.path());
                if (representedTargets.insert(target).second)
                    fallbackDevices.push_back(entry.path().string());
            }
        }
    } catch (...) {
    }

    std::sort(stableDevices.begin(), stableDevices.end());
    std::sort(fallbackDevices.begin(), fallbackDevices.end());
    stableDevices.insert(stableDevices.end(), fallbackDevices.begin(), fallbackDevices.end());
    return stableDevices;
}

std::vector<OscamMiniManager::DetectedReader> OscamMiniManager::detectedReaders() {
    std::vector<DetectedReader> result;

    for (const auto& device : ttyDevices()) {
        DetectedReader reader;
        reader.kind = "serial";
        reader.protocol = "mouse";
        reader.device = device;
        reader.stable = device.rfind("/dev/serial/by-id/", 0) == 0;
        reader.name = reader.stable ? fs::path(device).filename().string() : device;
        std::error_code ec;
        const auto resolved = fs::canonical(device, ec);
        if (!ec && resolved.string() != device) reader.detail = resolved.string();
        if (reader.detail.empty()) reader.detail = reader.stable ? "stable serial path" : "serial device";
        result.push_back(std::move(reader));
    }

    // pcsc_scan is installed by the OSCam-mini package dependencies. It is a
    // discovery helper only: OSCam still opens the selected reader itself by
    // zero-based PC/SC index. Keep this out of statusLocked(), because status
    // refreshes every three seconds and pcsc_scan intentionally stays alive.
    int pcscRc = 0;
    const std::string pcscOutput = run(
        "LC_ALL=C timeout 2s pcsc_scan -n </dev/null", &pcscRc);
    (void)pcscRc; // timeout(1) normally exits 124 after it printed the readers.

    std::set<std::string> pcscSeen;
    std::istringstream lines(pcscOutput);
    std::string line;
    while (std::getline(lines, line)) {
        line = trimLocal(line);
        std::size_t digits = 0;
        while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])))
            ++digits;
        if (digits == 0 || digits >= line.size() || line[digits] != ':') continue;
        const std::string index = line.substr(0, digits);
        const std::string name = trimLocal(line.substr(digits + 1));
        if (name.empty()) continue;
        const std::string key = index + "\n" + name;
        if (!pcscSeen.insert(key).second) continue;

        DetectedReader reader;
        reader.kind = "pcsc";
        reader.name = name;
        reader.protocol = "pcsc";
        reader.device = index;
        reader.detail = "PC/SC index " + index;
        reader.stable = false;
        result.push_back(std::move(reader));
    }

    return result;
}

std::string OscamMiniManager::json(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

Json::Value OscamMiniManager::parse(const std::string& body, std::string& error) {
    Json::Value value;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(body.data(), body.data() + body.size(), &value, &error)) {
        return {};
    }
    return value;
}

OscamMiniManager::Settings OscamMiniManager::loadLocked() {
    Settings settings;

    const std::string oscamConfText = readFile(configDir() + "/oscam.conf");
    const auto newcamd = parseFirstSection(oscamConfText, "newcamd");
    settings.bindIp = getValue(newcamd, "serverip", "127.0.0.1");
    settings.key = getValue(newcamd, "key", settings.key);
    settings.keepalive = isTrue(getValue(newcamd, "keepalive", "1"));

    const auto streamrelay = parseFirstSection(oscamConfText, "streamrelay");
    settings.streamRelayEnabled = isTrue(getValue(streamrelay, "stream_relay_enabled", "0"));
    settings.streamRelayPort = getInt(streamrelay, "stream_relay_port", 17999);
    settings.streamSourceHost = getValue(streamrelay, "stream_source_host", "127.0.0.1");
    settings.streamSourcePort = getInt(streamrelay, "stream_source_port", 8001);
    settings.streamClientSourceHost = isTrue(getValue(streamrelay, "stream_client_source_host", "0"));
    settings.streamRelayBufferTime = getInt(streamrelay, "stream_relay_buffer_time", 0);
    settings.streamRelayCaid = getValue(streamrelay, "stream_relay_ctab", "");

    const auto radegast = parseFirstSection(oscamConfText, "radegast");
    settings.streamRadegastPort = getInt(radegast, "port", 9090);

    auto ports = parsePortDefs(getValue(newcamd, "port", ""));
    auto accounts = parseIniSections(readFile(configDir() + "/oscam.user"), "account");
    std::vector<bool> usedPorts(ports.size(), false);

    for (std::size_t i = 0; i < accounts.size(); ++i) {
        const auto& account = accounts[i];
        NewcamdUser user;
        user.user = getValue(account, "user", "tvstreamer" + std::to_string(i + 1));
        user.password = getValue(account, "pwd", "tvstreamer");
        user.groups = getValue(account, "group", "1");
        user.au = isTrue(getValue(account, "au", "1"));

        auto [accountCaid, accountProvider] = accountCaidProvider(account);
        int selected = -1;
        for (std::size_t p = 0; p < ports.size(); ++p) {
            if (usedPorts[p]) {
                continue;
            }
            if (!accountCaid.empty() && ports[p].caid != accountCaid) {
                continue;
            }
            if (!accountProvider.empty() && ports[p].provider != accountProvider) {
                continue;
            }
            selected = static_cast<int>(p);
            break;
        }
        if (selected < 0) {
            for (std::size_t p = 0; p < ports.size(); ++p) {
                if (!usedPorts[p]) {
                    selected = static_cast<int>(p);
                    break;
                }
            }
        }
        if (selected >= 0) {
            usedPorts[static_cast<std::size_t>(selected)] = true;
            user.port = ports[static_cast<std::size_t>(selected)].port;
            user.caid = ports[static_cast<std::size_t>(selected)].caid;
            user.provider = ports[static_cast<std::size_t>(selected)].provider;
        } else {
            user.port = 11220 + static_cast<int>(i);
            user.caid = isHex(accountCaid, 4) ? accountCaid : "0652";
            user.provider = isHex(accountProvider, 6) ? accountProvider : "000000";
        }
        settings.users.push_back(user);
    }

    // Preserve port definitions even if a legacy config has fewer accounts than ports.
    for (std::size_t p = 0; p < ports.size(); ++p) {
        if (usedPorts[p]) {
            continue;
        }
        NewcamdUser user;
        user.user = "user" + std::to_string(settings.users.size() + 1);
        user.password = "change_me";
        user.port = ports[p].port;
        user.caid = ports[p].caid;
        user.provider = ports[p].provider;
        user.groups = "1";
        settings.users.push_back(user);
    }

    if (settings.users.empty()) {
        settings.users.push_back(NewcamdUser{});
    }

    for (const auto& entry : parseIniSections(readFile(configDir() + "/oscam.server"), "reader")) {
        Reader reader;
        reader.label = getValue(entry, "label", "Reader");
        reader.protocol = getValue(entry, "protocol", "mouse");
        reader.device = getValue(entry, "device", "");
        reader.remoteUser = getValue(entry, "user", "");
        reader.remotePassword = getValue(entry, "password", "");
        reader.remoteKey = getValue(entry, "key", "");
        if (isRemoteReaderProtocol(reader.protocol)) {
            std::vector<std::string> parts;
            std::istringstream deviceParts(reader.device);
            std::string part;
            while (std::getline(deviceParts, part, ',')) parts.push_back(trimLocal(part));
            if (!parts.empty()) reader.remoteHost = parts[0];
            if (parts.size() > 1) {
                try { reader.remotePort = std::stoi(parts[1]); } catch (...) { reader.remotePort = 0; }
            }
            if (parts.size() > 2) {
                try { reader.remoteLocalPort = std::stoi(parts[2]); } catch (...) { reader.remoteLocalPort = 0; }
            }
        }
        reader.cccVersion = getValue(entry, "cccversion", "");
        reader.cccMaxHops = getInt(entry, "cccmaxhops", 10);
        reader.cccKeepalive = isTrue(getValue(entry, "ccckeepalive", "0"));
        reader.remoteKeepalive = isTrue(getValue(entry, "keepalive", "0"));
        reader.remoteUseSsl = isTrue(getValue(entry, "use_ssl", "0"));
        reader.caid = getValue(entry, "caid", "");
        reader.detect = getValue(entry, "detect", "cd");
        reader.mhz = getInt(entry, "mhz", 600);
        reader.cardmhz = getInt(entry, "cardmhz", 600);
        reader.group = getInt(entry, "group", 1);
        reader.ident = getValue(entry, "ident", "");
        reader.boxkey = getValue(entry, "boxkey", "");
        reader.rsakey = getValue(entry, "rsakey", "");
        reader.auprovid = getValue(entry, "auprovid", "");
        reader.emmcache = getValue(entry, "emmcache", "1,3,2,0");
        reader.enabled = !isTrue(getValue(entry, "disable", "0"));
        settings.readers.push_back(reader);
    }

    return settings;
}


bool OscamMiniManager::mergeCamClientsLocked(Settings& settings,
                                               const std::vector<CamClientConfig>& clients,
                                               std::string& error, bool& changed) {
    changed = false;
    std::set<int> requestedPorts;
    std::string commonDes;

    for (const auto& client : clients) {
        const std::string backend = client.backendId.empty() ? "newcamd" : client.backendId;
        if (backend != "newcamd") continue;

        const Json::Value cfg = parseCamBackendConfigText(client.backendConfig);
        const std::string host = cfg.get("host", "").asString();
        if (!isLoopbackNewcamdHost(host)) continue; // direct remote Newcamd client: no local OSCam listener

        const int port = cfg.get("port", 0).asInt();
        const std::string username = cfg.get("user", "").asString();
        const std::string password = cfg.get("pass", "").asString();
        const std::string des = cfg.get("des", "").asString();
        const std::string clientName = client.id.empty() ? client.name : client.id;

        if (clientName.empty()) {
            error = "Local Newcamd CAM client has no id";
            return false;
        }
        if (port < 1 || port > 65535 || !requestedPorts.insert(port).second) {
            error = "Local Newcamd CAM client " + clientName + " has an invalid or duplicate port";
            return false;
        }
        if (!isSafeName(username) || password.empty() || password.size() > 64 ||
            password.find_first_of("\r\n;#") != std::string::npos) {
            error = "Local Newcamd CAM client " + clientName + " has invalid OSCam credentials";
            return false;
        }
        if (!isHex(des, 28)) {
            error = "Local Newcamd CAM client " + clientName + " has invalid DES key";
            return false;
        }
        if (commonDes.empty()) commonDes = des;
        else if (commonDes != des) {
            error = "Local Newcamd CAM clients must use the same OSCam server DES key";
            return false;
        }

        const std::string idToken = normalizedCamToken(client.id);
        const std::string nameToken = normalizedCamToken(client.name);
        const std::string remoteIdToken = normalizedCamToken("remote_" + client.id);
        const std::string remoteNameToken = normalizedCamToken("remote_" + client.name);

        const Reader* matchedReader = nullptr;
        for (const auto& reader : settings.readers) {
            if (!reader.enabled || reader.protocol != "newcamd") continue;
            const std::string label = normalizedCamToken(reader.label);
            if ((!remoteIdToken.empty() && label == remoteIdToken) ||
                (!idToken.empty() && label == idToken) ||
                (!remoteNameToken.empty() && label == remoteNameToken) ||
                (!nameToken.empty() && label == nameToken)) {
                matchedReader = &reader;
                break;
            }
        }

        int existingIndex = -1;
        for (std::size_t i = 0; i < settings.users.size(); ++i) {
            if (settings.users[i].user == username) {
                existingIndex = static_cast<int>(i);
                break;
            }
        }
        if (existingIndex < 0) {
            for (std::size_t i = 0; i < settings.users.size(); ++i) {
                if (settings.users[i].port == port) {
                    existingIndex = static_cast<int>(i);
                    break;
                }
            }
        }

        std::string caid;
        std::string provider;
        std::string groups = "1";
        if (matchedReader) {
            caid = matchedReader->caid;
            provider = providerFromReaderIdent(matchedReader->ident, caid);
            groups = std::to_string(matchedReader->group);
        } else if (existingIndex >= 0) {
            const auto& existing = settings.users[static_cast<std::size_t>(existingIndex)];
            caid = existing.caid;
            provider = existing.provider;
            groups = existing.groups;
        } else {
            error = "Local Newcamd CAM client " + clientName +
                    " has no matching OSCam Remote Newcamd reader (expected remote_" + client.id + ")";
            return false;
        }

        if (!isHex(caid, 4)) {
            error = "Matching OSCam reader for " + clientName + " has invalid CAID";
            return false;
        }
        if (!isHex(provider, 6)) provider = "000000";
        if (!validGroups(groups)) groups = "1";

        for (std::size_t i = 0; i < settings.users.size(); ++i) {
            if (static_cast<int>(i) == existingIndex) continue;
            if (settings.users[i].port == port) {
                error = "OSCam Newcamd port " + std::to_string(port) +
                        " is already owned by another account";
                return false;
            }
            if (settings.users[i].user == username) {
                error = "OSCam Newcamd user " + username + " is already duplicated";
                return false;
            }
        }

        NewcamdUser desired;
        desired.user = username;
        desired.password = password;
        desired.port = port;
        desired.caid = caid;
        desired.provider = provider;
        desired.groups = groups;
        desired.au = true;

        if (existingIndex < 0) {
            settings.users.push_back(desired);
            changed = true;
        } else {
            auto& current = settings.users[static_cast<std::size_t>(existingIndex)];
            if (current.user != desired.user || current.password != desired.password ||
                current.port != desired.port || current.caid != desired.caid ||
                current.provider != desired.provider || current.groups != desired.groups ||
                current.au != desired.au) {
                current = desired;
                changed = true;
            }
        }
    }

    if (!commonDes.empty() && settings.key != commonDes) {
        settings.key = commonDes;
        changed = true;
    }
    return true;
}

bool OscamMiniManager::synchronizeCamClients(const std::vector<CamClientConfig>& clients,
                                              std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    Settings settings = loadLocked();
    bool changed = false;
    if (!mergeCamClientsLocked(settings, clients, error, changed)) return false;
    if (!changed) return true;
    if (!saveLocked(settings, error)) return false;

    int activeRc = 0;
    const std::string active = trim(run("systemctl is-active " + std::string(kService), &activeRc));
    if (active == "active") {
        int restartRc = 0;
        const std::string restartOutput = run("systemctl restart " + std::string(kService), &restartRc);
        if (restartRc != 0) {
            error = "OSCam-mini listeners were synchronized but service restart failed: " + trim(restartOutput);
            return false;
        }
    }
    return true;
}

void OscamMiniManager::appendRuntimeCamClients(std::vector<CamClientConfig>& clients) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Settings settings = loadLocked();

    const bool hasEnabledReader = std::any_of(settings.readers.begin(), settings.readers.end(),
                                              [](const Reader& reader) { return reader.enabled; });
    if (!hasEnabledReader || !isHex(settings.key, 28)) return;

    std::string host = trimLocal(settings.bindIp);
    if (host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]") host = "127.0.0.1";

    auto equivalentClientExists = [&](const NewcamdUser& user) {
        return std::any_of(clients.begin(), clients.end(), [&](const CamClientConfig& client) {
            const std::string backend = client.backendId.empty() ? "newcamd" : client.backendId;
            if (backend != "newcamd") return false;
            const Json::Value cfg = parseCamBackendConfigText(client.backendConfig);
            const int clientPort = cfg.get("port", 0).asInt();
            const std::string clientUser = cfg.get("user", "").asString();
            const std::string clientHost = trimLocal(cfg.get("host", "").asString());
            const bool sameHost = clientHost == host ||
                                  (isLoopbackNewcamdHost(clientHost) && isLoopbackNewcamdHost(host));
            return sameHost && clientPort == user.port && clientUser == user.user;
        });
    };

    std::set<std::string> usedIds;
    for (const auto& client : clients) {
        if (!client.id.empty()) usedIds.insert(client.id);
    }

    for (const auto& user : settings.users) {
        if (!isSafeName(user.user) || user.password.empty() ||
            user.port < 1 || user.port > 65535 || equivalentClientExists(user)) {
            continue;
        }

        std::string token = normalizedCamToken(user.user);
        if (token.empty()) token = std::to_string(user.port);
        std::string id = "oscam-mini-" + token;
        if (usedIds.count(id)) id += "-" + std::to_string(user.port);
        unsigned duplicateIndex = 2;
        const std::string idBase = id;
        while (usedIds.count(id)) id = idBase + "-" + std::to_string(duplicateIndex++);

        Json::Value backendConfig(Json::objectValue);
        backendConfig["host"] = host;
        backendConfig["port"] = user.port;
        backendConfig["user"] = user.user;
        backendConfig["pass"] = user.password;
        backendConfig["des"] = settings.key;
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";

        CamClientConfig client;
        client.id = id;
        client.name = "OSCam-mini " + user.user;
        if (isHex(user.caid, 4)) client.name += " / CAID " + user.caid;
        client.maxServices = 10;
        client.backendId = "newcamd";
        client.backendConfig = Json::writeString(writer, backendConfig);
        clients.push_back(std::move(client));
        usedIds.insert(id);
    }
}

bool OscamMiniManager::saveLocked(const Settings& settings, std::string& error) {
    if (!isHex(settings.key, 28)) {
        error = "DES key должен содержать ровно 28 HEX-символов";
        return false;
    }
    if (settings.users.empty()) {
        error = "Добавьте хотя бы одного пользователя Newcamd";
        return false;
    }

    std::set<int> ports;
    std::set<std::string> usernames;
    for (const auto& user : settings.users) {
        if (!isSafeName(user.user)) {
            error = "Некорректное имя пользователя: " + user.user;
            return false;
        }
        if (user.password.empty() || user.password.size() > 64 || user.password.find_first_of("\r\n;#") != std::string::npos) {
            error = "Некорректный пароль пользователя " + user.user;
            return false;
        }
        if (user.port < 1 || user.port > 65535) {
            error = "Некорректный порт пользователя " + user.user;
            return false;
        }
        if (!ports.insert(user.port).second) {
            error = "Порт " + std::to_string(user.port) + " используется более одного раза";
            return false;
        }
        if (!usernames.insert(user.user).second) {
            error = "Пользователь " + user.user + " указан более одного раза";
            return false;
        }
        if (!isHex(user.caid, 4)) {
            error = "CAID пользователя " + user.user + " должен содержать 4 HEX-символа";
            return false;
        }
        if (!isHex(user.provider, 6)) {
            error = "Provider пользователя " + user.user + " должен содержать 6 HEX-символов";
            return false;
        }
        if (!validGroups(user.groups)) {
            error = "Некорректные группы пользователя " + user.user;
            return false;
        }
    }

    if (settings.streamRelayEnabled) {
        const bool sourceHostOk = !settings.streamSourceHost.empty() && settings.streamSourceHost.size() <= 255 &&
            settings.streamSourceHost.find_first_of("\r\n,;# \t") == std::string::npos;
        if (!sourceHostOk || settings.streamSourcePort < 1 || settings.streamSourcePort > 65535 ||
            settings.streamRelayPort < 1 || settings.streamRelayPort > 65535 ||
            settings.streamRadegastPort < 1 || settings.streamRadegastPort > 65535) {
            error = "StreamRelay: проверьте Source Host/Port, Relay Port и Radegast Port";
            return false;
        }
        if (!validCaidTable(settings.streamRelayCaid)) {
            error = "StreamRelay CAID filter должен содержать CAID/маски OSCam, например 098C,09C4";
            return false;
        }
        if (settings.streamRelayBufferTime < 0 || settings.streamRelayBufferTime > 30000) {
            error = "StreamRelay buffer time должен быть 0..30000 мс";
            return false;
        }
    }

    std::ostringstream conf;
    conf << "[global]\n"
         << "logfile = stdout\n"
         << "clienttimeout = 5000\n"
         << "clientmaxidle = 120\n"
         << "waitforcards = 1\n\n"
         << "[newcamd]\n"
         << "serverip = " << safeIni(settings.bindIp) << "\n"
         << "port = ";
    for (std::size_t i = 0; i < settings.users.size(); ++i) {
        if (i) {
            conf << ';';
        }
        const auto& user = settings.users[i];
        conf << user.port << '@' << safeIni(user.caid) << ':' << safeIni(user.provider);
    }
    conf << "\nkey = " << safeIni(settings.key) << "\n"
         << "keepalive = " << (settings.keepalive ? 1 : 0) << "\n";

    if (settings.streamRelayEnabled) {
        conf << "\n[radegast]\n"
             << "serverip = 127.0.0.1\n"
             << "port = " << settings.streamRadegastPort << "\n"
             << "allowed = 127.0.0.1\n"
             << "\n[streamrelay]\n"
             << "stream_relay_enabled = 1\n"
             << "stream_relay_port = " << settings.streamRelayPort << "\n"
             << "stream_source_host = " << safeIni(settings.streamSourceHost) << "\n"
             << "stream_source_port = " << settings.streamSourcePort << "\n"
             << "stream_client_source_host = " << (settings.streamClientSourceHost ? 1 : 0) << "\n"
             << "stream_relay_buffer_time = " << settings.streamRelayBufferTime << "\n"
             << "stream_relay_ctab = " << safeIni(settings.streamRelayCaid) << "\n";
    }

    std::ostringstream users;
    for (const auto& user : settings.users) {
        users << "[account]\n"
              << "user = " << safeIni(user.user) << "\n"
              << "pwd = " << safeIni(user.password) << "\n"
              << "group = " << safeIni(user.groups) << "\n"
              << "au = " << (user.au ? 1 : 0) << "\n"
              << "caid = " << safeIni(user.caid) << "\n"
              << "ident = " << safeIni(user.caid) << ':' << safeIni(user.provider) << "\n"
              << "allowedprotocols = newcamd\n\n";
    }

    std::set<int> localNewcamdPorts;
    for (const auto& user : settings.users) {
        if (user.port >= 1 && user.port <= 65535) localNewcamdPorts.insert(user.port);
    }

    std::ostringstream server;
    for (const auto& reader : settings.readers) {
        if (reader.label.empty()) {
            error = "У ридера отсутствует имя";
            return false;
        }
        const bool pcsc = reader.protocol == "pcsc";
        const bool remoteReader = isRemoteReaderProtocol(reader.protocol);
        const bool serialDevice = reader.device.rfind("/dev/", 0) == 0 &&
            reader.device.size() > 5 && reader.device.size() <= 256 &&
            reader.device.find_first_of("\r\n;#") == std::string::npos;
        const bool pcscIndex = !reader.device.empty() && reader.device.size() <= 3 &&
            std::all_of(reader.device.begin(), reader.device.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            }) && std::stoul(reader.device) <= 999;
        if (!remoteReader && ((pcsc && !pcscIndex) || (!pcsc && !serialDevice))) {
            error = pcsc ? "Для PC/SC укажите индекс ридера (0, 1, ...): " + reader.label
                         : "Некорректное устройство ридера " + reader.label;
            return false;
        }
        if (remoteReader) {
            const bool hostOk = !reader.remoteHost.empty() && reader.remoteHost.size() <= 255 &&
                reader.remoteHost.find_first_of("\r\n,;# \t") == std::string::npos;
            if (!hostOk || reader.remotePort < 1 || reader.remotePort > 65535) {
                error = "Для Remote CA укажите корректные Host и Port: " + reader.label;
                return false;
            }
            if (isLoopbackNewcamdHost(reader.remoteHost) &&
                localNewcamdPorts.count(reader.remotePort) != 0) {
                error = "Remote CA ридер " + reader.label +
                    " указывает на локальный Newcamd listener OSCam-mini и создаёт петлю";
                return false;
            }
            if (reader.remoteLocalPort < 0 || reader.remoteLocalPort > 65535) {
                error = "Некорректный Local Port Remote CA: " + reader.label;
                return false;
            }
            const bool userOk = reader.remoteUser.size() <= 128 &&
                reader.remoteUser.find_first_of("\r\n;#") == std::string::npos;
            const bool passOk = reader.remotePassword.size() <= 256 &&
                reader.remotePassword.find_first_of("\r\n;#") == std::string::npos;
            if (!userOk || !passOk || (remoteReaderNeedsCredentials(reader.protocol) &&
                (reader.remoteUser.empty() || reader.remotePassword.empty()))) {
                error = "Для " + reader.protocol + " укажите корректные User и Password: " + reader.label;
                return false;
            }
            if (remoteReaderNeedsDesKey(reader.protocol) && !isHex(reader.remoteKey, 28)) {
                error = "DES key " + reader.protocol + " ридера " + reader.label + " должен содержать 28 HEX-символов";
                return false;
            }
            if (!reader.remoteKey.empty() && !isHex(reader.remoteKey, 28)) {
                error = "Remote key ридера " + reader.label + " должен содержать 28 HEX-символов";
                return false;
            }
            if ((reader.protocol == "cccam" || reader.protocol == "cccam_ext") &&
                (reader.cccMaxHops < -1 || reader.cccMaxHops > 20)) {
                error = "CCcam max hops должен быть от -1 до 20: " + reader.label;
                return false;
            }
        }
        if (!isHex(reader.caid, 4)) {
            error = "CAID ридера " + reader.label + " должен содержать 4 HEX-символа";
            return false;
        }
        if (reader.group < 1 || reader.group > 64) {
            error = "Некорректная группа ридера " + reader.label;
            return false;
        }
        if (!remoteReader && (reader.mhz < 100 || reader.mhz > 2000 || reader.cardmhz < 100 || reader.cardmhz > 2000)) {
            error = "Некорректная частота ридера " + reader.label;
            return false;
        }
        if (reader.protocol != "mouse" && reader.protocol != "phoenix" &&
            reader.protocol != "pcsc" && !remoteReader) {
            error = "Неподдерживаемый протокол OSCam reader: " + reader.protocol;
            return false;
        }
        if (!reader.boxkey.empty() && !isHex(reader.boxkey, 16)) {
            error = "BoxKey ридера " + reader.label + " должен содержать 16 HEX-символов";
            return false;
        }
        if (!reader.rsakey.empty() && (reader.rsakey.size() < 32 || (reader.rsakey.size() % 2) != 0 || !std::all_of(reader.rsakey.begin(), reader.rsakey.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))) {
            error = "RSAKey ридера " + reader.label + " должен содержать HEX-строку чётной длины";
            return false;
        }
        if (!reader.auprovid.empty() && !isHex(reader.auprovid, 6)) {
            error = "AU Provider ридера " + reader.label + " должен содержать 6 HEX-символов";
            return false;
        }

        std::string outputDevice = reader.device;
        if (remoteReader) {
            outputDevice = reader.remoteHost + "," + std::to_string(reader.remotePort);
            if (reader.remoteLocalPort > 0) outputDevice += "," + std::to_string(reader.remoteLocalPort);
        }
        server << "[reader]\n"
               << "label = " << safeIni(reader.label) << "\n"
               << "protocol = " << safeIni(reader.protocol) << "\n"
               << "device = " << safeIni(outputDevice) << "\n"
               << "caid = " << safeIni(reader.caid) << "\n";
        if (remoteReader) {
            if (!reader.remoteUser.empty()) server << "user = " << safeIni(reader.remoteUser) << "\n";
            if (!reader.remotePassword.empty()) server << "password = " << safeIni(reader.remotePassword) << "\n";
            if (!reader.remoteKey.empty()) server << "key = " << safeIni(reader.remoteKey) << "\n";
            if (remoteReaderNeedsDesKey(reader.protocol)) server << "connectoninit = 1\n";
            if (reader.protocol == "cs378x") server << "keepalive = " << (reader.remoteKeepalive ? 1 : 0) << "\n";
            if (reader.protocol == "cccam" || reader.protocol == "cccam_ext") {
                if (!reader.cccVersion.empty()) server << "cccversion = " << safeIni(reader.cccVersion) << "\n";
                server << "cccmaxhops = " << reader.cccMaxHops << "\n"
                       << "ccckeepalive = " << (reader.cccKeepalive ? 1 : 0) << "\n";
            }
            if (reader.protocol == "ghttp") server << "use_ssl = " << (reader.remoteUseSsl ? 1 : 0) << "\n";
        }
        if (!reader.boxkey.empty()) {
            server << "boxkey = " << safeIni(reader.boxkey) << "\n";
        }
        if (!reader.rsakey.empty()) {
            server << "rsakey = " << safeIni(reader.rsakey) << "\n";
        }
        if (!remoteReader) server << "detect = " << (pcsc ? "none" : safeIni(reader.detect)) << "\n";
        // PC/SC negotiates clock and card parameters through pcscd.  Phoenix
        // settings must not be imposed on USB OMNIKEY readers.
        if (!pcsc && !remoteReader) {
            server << "mhz = " << reader.mhz << "\n"
                   << "cardmhz = " << reader.cardmhz << "\n";
        }
        server << "group = " << reader.group << "\n";
        if (!reader.ident.empty()) {
            server << "ident = " << safeIni(reader.ident) << "\n";
        }
        if (!reader.auprovid.empty()) {
            server << "auprovid = " << safeIni(reader.auprovid) << "\n";
        }
        if (!reader.emmcache.empty()) {
            server << "emmcache = " << safeIni(reader.emmcache) << "\n";
        }
        if (!reader.enabled) {
            server << "disable = 1\n";
        }
        server << '\n';
    }

    return writeAtomic(configDir() + "/oscam.conf", conf.str(), error)
        && writeAtomic(configDir() + "/oscam.user", users.str(), error)
        && writeAtomic(configDir() + "/oscam.server", server.str(), error);
}

Json::Value OscamMiniManager::statusLocked() {
    Json::Value result;
    result["config_dir"] = configDir();
    result["binary_exists"] = fs::exists(binaryPath());

    int rc = 0;
    const std::string active = trim(run("systemctl is-active " + std::string(kService), &rc));
    const bool serviceActive = active == "active";
    result["service_active"] = serviceActive;
    result["service_state"] = active.empty() ? "unknown" : active;

    Json::Value devices(Json::arrayValue);
    for (const auto& device : ttyDevices()) {
        devices.append(device);
    }
    result["devices"] = devices;

    // In the portable bundle systemd starts OSCam through the bundled ELF
    // loader, so the kernel comm/name is ld-linux-x86-64 rather than
    // oscam-mini.  Resolve the process through systemd MainPID instead of
    // relying on "ps -C oscam-mini".
    std::string pid;
    if (serviceActive) {
        pid = trim(run("systemctl show -p MainPID --value " + std::string(kService)));
        if (pid == "0") {
            pid.clear();
        }
    }
    const std::string process = pid.empty()
        ? std::string()
        : trim(run("ps -p " + pid + " -o pid=,rss=,vsz=,%cpu=,cmd="));
    result["process"] = process;

    std::string logCommand = "journalctl -u " + std::string(kService) + " -n 120 --no-pager -o cat";
    if (!pid.empty()) {
        logCommand = "journalctl -u " + std::string(kService) + " _PID=" + pid + " -n 240 --no-pager -o cat";
    }
    std::string log = run(logCommand);
    if (log.size() > 32000) {
        log = log.substr(log.size() - 32000);
    }
    result["log"] = log;

    // Startup's "ready for requests" eventually falls out of the last 240
    // journal lines on busy readers. Recent successful ECM replies are much
    // stronger live evidence than a missing old startup message. Read only
    // a short, time-bounded journal slice and never expose CW bytes.
    std::string recentReaderLog;
    if (serviceActive && !pid.empty()) {
        recentReaderLog = run(
            "journalctl -u " + std::string(kService) + " _PID=" + pid +
            " --since '-45 seconds' --no-pager -o cat");
        if (recentReaderLog.size() > 32000) {
            recentReaderLog.erase(0, recentReaderLog.size() - 32000);
        }
    }
    const auto readerRecentlyFoundEcm = [&](const std::string& label) {
        if (label.empty()) return false;
        std::istringstream lines(recentReaderLog);
        std::string line;
        const std::string marker = " by " + label;
        while (std::getline(lines, line)) {
            if (line.find(" (ecm) ") == std::string::npos ||
                line.find("): found (") == std::string::npos) continue;
            const auto pos = line.rfind(marker);
            if (pos != std::string::npos &&
                (pos + marker.size() == line.size() ||
                 std::isspace(static_cast<unsigned char>(
                     line[pos + marker.size()])))) {
                return true;
            }
        }
        return false;
    };

    const Settings settings = loadLocked();
    // Include socket ownership so Remote Newcamd reader state is based on the
    // OSCam-mini process itself instead of an unrelated TCP connection that
    // happens to use the same port.
    const std::string established = serviceActive ? run("ss -Htanp state established 2>/dev/null") : std::string();
    const auto oscamSocketOnPort = [&](int port) {
        if (pid.empty() || port < 1 || port > 65535) return false;
        const std::string portToken = ":" + std::to_string(port);
        const std::string pidToken = "pid=" + pid + ",";
        std::istringstream sockets(established);
        std::string socketLine;
        while (std::getline(sockets, socketLine)) {
            if (socketLine.find(portToken) != std::string::npos &&
                socketLine.find(pidToken) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    Json::Value userActivity(Json::objectValue);
    for (const auto& user : settings.users) {
        Json::Value item;
        std::string state = "down";
        std::string detail = "OSCam-mini остановлен";

        if (serviceActive) {
            if (oscamSocketOnPort(user.port)) {
                state = "active";
                detail = "Newcamd клиент подключён";
            } else {
                state = "idle";
                detail = "Ожидание подключения";
            }
        }

        item["state"] = state;
        item["detail"] = detail;
        item["port"] = user.port;
        userActivity[user.user] = item;
    }
    result["user_activity"] = userActivity;

    Json::Value readerActivity(Json::objectValue);
    for (const auto& reader : settings.readers) {
        Json::Value item;
        std::string state = "down";
        std::string detail = "OSCam-mini остановлен";

        if (!reader.enabled) {
            state = "disabled";
            detail = "Ридер отключён";
        } else if (serviceActive) {
            const bool remoteReader = isRemoteReaderProtocol(reader.protocol);
            const std::string remoteEndpoint = reader.remoteHost + ":" + std::to_string(reader.remotePort);
            const std::string protocolTag = "[" + reader.protocol + "]";
            const std::string readyNeedle = reader.label + " [irdeto] ready for requests";
            const std::string readyMouseNeedle = reader.label + " [mouse] ready for requests";
            const bool remoteSocketReady = remoteReader && oscamSocketOnPort(reader.remotePort);
            const bool remoteLoggedReady = remoteReader &&
                (log.find(reader.label + " " + protocolTag + " proxy initialized") != std::string::npos ||
                 log.find(reader.label + " [newcamd] Newcamd Server: " + remoteEndpoint + " - UserID:") != std::string::npos);
            const bool recentlyFoundEcm =
                !remoteReader && readerRecentlyFoundEcm(reader.label);
            const bool ready = remoteReader
                ? (remoteSocketReady || remoteLoggedReady)
                : (recentlyFoundEcm ||
                   log.find(readyNeedle) != std::string::npos ||
                   log.find(readyMouseNeedle) != std::string::npos);
            const bool initializing = remoteReader
                ? log.find(reader.label + " " + protocolTag) != std::string::npos
                : (log.find(reader.label + " [mouse] card detected") != std::string::npos
                   || log.find(reader.label + " [mouse] detect irdeto card") != std::string::npos
                   || log.find(reader.label + " [mouse] found card system") != std::string::npos
                   || log.find(reader.label + " [pcsc] card detected") != std::string::npos
                   || log.find(reader.label + " [pcsc] found card system") != std::string::npos
                   || log.find(reader.label + " [irdeto] THIS WAS A SUCCESSFUL START ATTEMPT") != std::string::npos);
            const bool error = remoteReader
                ? (log.find(reader.label + " " + protocolTag + " ERROR") != std::string::npos ||
                   log.find(reader.label + " [newcamd] login failed for user") != std::string::npos ||
                   log.find(reader.label + " [newcamd] server does not return 14 bytes") != std::string::npos ||
                   log.find(reader.label + " [newcamd] expected MSG_CLIENT_2_SERVER_LOGIN_ACK") != std::string::npos)
                : (log.find(reader.label + " [mouse] Error activating card") != std::string::npos
                   || log.find(reader.label + " [mouse] ERROR") != std::string::npos
                   || log.find(reader.label + " [pcsc] ERROR") != std::string::npos
                   || log.find(reader.label + " [irdeto] ERROR") != std::string::npos
                   || log.find(reader.label + " [mouse] card initializing error") != std::string::npos);

            if (ready) {
                state = "active";
                detail = remoteReader ? ("Remote CA " + reader.protocol + " подключён")
                    : (recentlyFoundEcm ? "ECM успешно обработан — ридер активен" : "Карта готова");
            } else if (error) {
                state = "down";
                detail = remoteReader ? ("Ошибка Remote CA " + reader.protocol) : "Ошибка ридера";
            } else if (initializing) {
                state = "idle";
                detail = remoteReader ? ("Remote CA " + reader.protocol + " активируется") : "Инициализация карты";
            } else {
                state = "idle";
                detail = remoteReader ? ("Ожидание Remote CA " + reader.protocol) : "Ожидание карты";
            }
        }

        item["state"] = state;
        item["detail"] = detail;
        item["device"] = reader.device;
        readerActivity[reader.label] = item;
    }
    result["reader_activity"] = readerActivity;
    return result;
}

std::string OscamMiniManager::statusJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    return json(statusLocked());
}

std::string OscamMiniManager::settingsJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    const Settings settings = loadLocked();

    Json::Value result;
    result["bind_ip"] = settings.bindIp;
    result["key"] = settings.key;
    result["keepalive"] = settings.keepalive;
    result["stream_relay_enabled"] = settings.streamRelayEnabled;
    result["stream_relay_port"] = settings.streamRelayPort;
    result["stream_source_host"] = settings.streamSourceHost;
    result["stream_source_port"] = settings.streamSourcePort;
    result["stream_client_source_host"] = settings.streamClientSourceHost;
    result["stream_relay_buffer_time"] = settings.streamRelayBufferTime;
    result["stream_relay_caid"] = settings.streamRelayCaid;
    result["stream_radegast_port"] = settings.streamRadegastPort;

    Json::Value users(Json::arrayValue);
    for (const auto& user : settings.users) {
        Json::Value value;
        value["user"] = user.user;
        value["password"] = user.password;
        value["port"] = user.port;
        value["caid"] = user.caid;
        value["provider"] = user.provider;
        value["groups"] = user.groups;
        value["au"] = user.au;
        users.append(value);
    }
    result["users"] = users;

    Json::Value readersJson(Json::arrayValue);
    for (const auto& reader : settings.readers) {
        Json::Value value;
        value["label"] = reader.label;
        value["protocol"] = reader.protocol;
        value["device"] = reader.device;
        value["remote_host"] = reader.remoteHost;
        value["remote_port"] = reader.remotePort;
        value["remote_local_port"] = reader.remoteLocalPort;
        value["remote_user"] = reader.remoteUser;
        value["remote_password"] = reader.remotePassword;
        value["remote_key"] = reader.remoteKey;
        value["ccc_version"] = reader.cccVersion;
        value["ccc_max_hops"] = reader.cccMaxHops;
        value["ccc_keepalive"] = reader.cccKeepalive;
        value["remote_keepalive"] = reader.remoteKeepalive;
        value["remote_use_ssl"] = reader.remoteUseSsl;
        value["caid"] = reader.caid;
        value["detect"] = reader.detect;
        value["mhz"] = reader.mhz;
        value["cardmhz"] = reader.cardmhz;
        value["group"] = reader.group;
        value["ident"] = reader.ident;
        value["boxkey"] = reader.boxkey;
        value["rsakey"] = reader.rsakey;
        value["auprovid"] = reader.auprovid;
        value["emmcache"] = reader.emmcache;
        value["enabled"] = reader.enabled;
        readersJson.append(value);
    }
    result["readers"] = readersJson;

    Json::Value detected(Json::arrayValue);
    for (const auto& reader : detectedReaders()) {
        Json::Value value;
        value["kind"] = reader.kind;
        value["name"] = reader.name;
        value["protocol"] = reader.protocol;
        value["device"] = reader.device;
        value["detail"] = reader.detail;
        value["stable"] = reader.stable;
        detected.append(value);
    }
    result["detected_readers"] = detected;
    return json(result);
}

std::string OscamMiniManager::saveSettingsJson(const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    Json::Value input = parse(body, error);
    Json::Value output;

    if (!error.empty()) {
        output["ok"] = false;
        output["error"] = error;
        return json(output);
    }

    Settings settings;
    settings.bindIp = input.get("bind_ip", "127.0.0.1").asString();
    settings.key = input.get("key", "0102030405060708091011121314").asString();
    settings.keepalive = input.get("keepalive", true).asBool();
    settings.streamRelayEnabled = input.get("stream_relay_enabled", false).asBool();
    settings.streamRelayPort = input.get("stream_relay_port", 17999).asInt();
    settings.streamSourceHost = input.get("stream_source_host", "127.0.0.1").asString();
    settings.streamSourcePort = input.get("stream_source_port", 8001).asInt();
    settings.streamClientSourceHost = input.get("stream_client_source_host", false).asBool();
    settings.streamRelayBufferTime = input.get("stream_relay_buffer_time", 0).asInt();
    settings.streamRelayCaid = input.get("stream_relay_caid", "").asString();
    settings.streamRadegastPort = input.get("stream_radegast_port", 9090).asInt();

    if (input["users"].isArray()) {
        for (const auto& item : input["users"]) {
            NewcamdUser user;
            user.user = item.get("user", "tvstreamer").asString();
            user.password = item.get("password", "tvstreamer").asString();
            user.port = item.get("port", 11220).asInt();
            user.caid = item.get("caid", "0652").asString();
            user.provider = item.get("provider", "000000").asString();
            user.groups = item.get("groups", "1").asString();
            user.au = item.get("au", true).asBool();
            settings.users.push_back(user);
        }
    }

    if (input["readers"].isArray()) {
        for (const auto& item : input["readers"]) {
            Reader reader;
            reader.label = item.get("label", "Reader").asString();
            reader.protocol = item.get("protocol", "mouse").asString();
            reader.device = item.get("device", "").asString();
            reader.remoteHost = item.get("remote_host", "").asString();
            reader.remotePort = item.get("remote_port", 0).asInt();
            reader.remoteLocalPort = item.get("remote_local_port", 0).asInt();
            reader.remoteUser = item.get("remote_user", "").asString();
            reader.remotePassword = item.get("remote_password", "").asString();
            reader.remoteKey = item.get("remote_key", "").asString();
            reader.cccVersion = item.get("ccc_version", "").asString();
            reader.cccMaxHops = item.get("ccc_max_hops", 10).asInt();
            reader.cccKeepalive = item.get("ccc_keepalive", false).asBool();
            reader.remoteKeepalive = item.get("remote_keepalive", false).asBool();
            reader.remoteUseSsl = item.get("remote_use_ssl", false).asBool();
            reader.caid = item.get("caid", "0652").asString();
            reader.detect = item.get("detect", "cd").asString();
            reader.mhz = item.get("mhz", 600).asInt();
            reader.cardmhz = item.get("cardmhz", 600).asInt();
            reader.group = item.get("group", 1).asInt();
            reader.ident = item.get("ident", "").asString();
            reader.boxkey = item.get("boxkey", "").asString();
            reader.rsakey = item.get("rsakey", "").asString();
            reader.auprovid = item.get("auprovid", "").asString();
            reader.emmcache = item.get("emmcache", "1,3,2,0").asString();
            reader.enabled = item.get("enabled", true).asBool();
            settings.readers.push_back(reader);
        }
    }


    // V10.8.66: OSCam-mini and DVBStreamer5 CAM clients are one local chain.
    // Re-apply the persisted CAM endpoints before writing/restarting OSCam so a
    // Remote Newcamd reader cannot leave DVBStreamer5 pointing at a dead local port.
    std::vector<CamClientConfig> camClients;
    fs::path appConfigDir = fs::current_path();
    if (const char* configuredDir = std::getenv("DVBSTREAMER5_CONFIG_DIR"); configuredDir && *configuredDir) {
        const fs::path candidate(configuredDir);
        if (candidate.is_absolute()) appConfigDir = candidate;
    }
    const std::string appConfigText = readFile((appConfigDir / "dvbstreamer5-config.json").string());
    if (!appConfigText.empty()) {
        std::string appConfigError;
        const Json::Value appConfig = parse(appConfigText, appConfigError);
        if (!appConfigError.empty()) {
            output["ok"] = false;
            output["error"] = "DVBStreamer5 CAM configuration parse failed: " + appConfigError;
            return json(output);
        }
        if (appConfig["cam_clients"].isArray()) {
            for (const auto& item : appConfig["cam_clients"]) camClients.push_back(CamClientConfig::fromJson(item));
        }
    }
    bool camSyncChanged = false;
    if (!mergeCamClientsLocked(settings, camClients, error, camSyncChanged)) {
        output["ok"] = false;
        output["error"] = error;
        return json(output);
    }

    if (!saveLocked(settings, error)) {
        output["ok"] = false;
        output["error"] = error;
        return json(output);
    }

    int rc = 0;
    const std::string restartOutput = run("systemctl restart " + std::string(kService), &rc);
    output["ok"] = rc == 0;
    if (rc != 0) {
        output["error"] = "Конфигурация сохранена, но OSCam-mini не перезапущен: " + trim(restartOutput);
    }
    output["status"] = statusLocked();
    return json(output);
}

std::string OscamMiniManager::serviceActionJson(const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    const Json::Value input = parse(body, error);
    Json::Value output;

    const std::string action = input.get("action", "").asString();
    if (action != "start" && action != "stop" && action != "restart") {
        output["ok"] = false;
        output["error"] = "unsupported action";
        return json(output);
    }

    int rc = 0;
    const std::string commandOutput = run("systemctl " + action + " " + std::string(kService), &rc);
    output["ok"] = rc == 0;
    if (rc != 0) {
        output["error"] = trim(commandOutput);
    }
    output["status"] = statusLocked();
    return json(output);
}

std::string OscamMiniManager::renderPage() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OSCam-mini</title>
<style>
body{font-family:Arial,sans-serif;background:#0f1218;color:#eee;margin:0}.w{max-width:1320px;margin:auto;padding:16px}.c{background:#161b25;border:1px solid #2a3241;border-radius:18px;padding:20px;margin:14px 0}.g{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px}label{display:flex;flex-direction:column;gap:6px;font-size:14px;color:#c6d1e3}input,select{background:#0e131c;border:1px solid #334058;color:#fff;border-radius:10px;padding:11px;font-size:15px}button,a.btn{border:0;border-radius:10px;padding:11px 16px;background:#218cff;color:white;cursor:pointer;text-decoration:none;font-size:15px}.alt{background:#30394a!important}.danger{background:#56313a!important}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}.grow{flex:1}.status-ok{color:#77e79d}.status-bad{color:#ff8b8b}.msg{margin-left:10px}.error{color:#ff8b8b}.success{color:#77e79d}pre{white-space:pre-wrap;background:#0b0f16;padding:12px;border-radius:10px;max-height:280px;overflow:auto}h2,h3{margin-top:0}.hint{color:#9ba9bd;font-size:13px;margin-top:8px}
.compact-item{border:1px solid #2c3546;border-radius:12px;margin-top:10px;overflow:hidden;background:#111722}.compact-head{display:grid;grid-template-columns:18px minmax(130px,1.2fr) minmax(120px,1fr) minmax(135px,1fr) minmax(90px,.7fr) 24px;gap:10px;align-items:center;width:100%;box-sizing:border-box;padding:11px 13px;background:#171e2a;color:#eef4ff;border:0;border-radius:0;text-align:left;cursor:pointer}.compact-head:hover{background:#1b2432}.compact-main{font-weight:700;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.compact-sub{color:#aebbd0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:13px}.compact-status{font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.chev{font-size:18px;color:#91a0b7;transition:transform .15s ease}.compact-item.open .chev{transform:rotate(90deg)}.compact-body{display:none;padding:14px;border-top:1px solid #2c3546}.compact-item.open .compact-body{display:block}.dot{width:10px;height:10px;border-radius:50%;display:inline-block;box-shadow:0 0 0 2px rgba(255,255,255,.08)}.dot-active{background:#49d17d}.dot-idle{background:#e8b844}.dot-down{background:#e25d66}.dot-disabled{background:#798394}.activity-active{color:#65df92}.activity-idle{color:#e8c45d}.activity-down{color:#ef7b83}.activity-disabled{color:#929cac}@media(max-width:760px){.compact-head{grid-template-columns:18px 1fr 24px}.compact-head .hide-small{display:none}}
</style>
</head>
<body><div class="w">
<div class="row"><a class="btn alt" href="/">← DVBStreamer5</a><h2 style="margin:0">OSCam-mini</h2><span id="state"></span></div>
<div class="c"><div class="row"><button onclick="act('start')">Старт</button><button class="alt" onclick="act('restart')">Перезапуск</button><button class="alt" onclick="act('stop')">Стоп</button><button class="alt" onclick="loadAll()">Обновить</button></div><p>Конфиги: /etc/dvbstreamer5/oscam-mini</p></div>

<div class="c">
  <h3>Newcamd</h3>
  <div class="g"><label>Bind IP<input id="bind_ip"></label><label>DES key<input id="key"></label></div>
  <div class="row" style="margin-top:16px"><h3 style="margin:0">Пользователи / порты</h3><button onclick="addUser()">+ Пользователь</button></div>
  <div class="hint">Строка пользователя свернута по умолчанию. Нажмите на неё, чтобы открыть настройки.</div>
  <div id="users"></div>
</div>

<div class="c"><div class="row"><h3 style="margin:0">Ридеры / Remote CA OSCam-mini</h3><button onclick="addReader()">+ Ридер</button><button class="alt" onclick="rescanReaders()">Найти локальные ридеры</button><span id="readerScan" class="hint" style="margin:0"></span></div><div class="hint">Локальные: mouse/phoenix/PCSC. Remote CA: Newcamd, Camd35/CS357x, CS378x, CCcam, Radegast, Gbox, Scam, GHTTP и Camd33. Выберите протокол — показываются только относящиеся к нему поля.</div><div id="readers"></div></div>
<div class="c"><div class="row"><h3 style="margin:0">StreamRelay / iCAM</h3><label style="flex-direction:row;align-items:center"><input id="stream_relay_enabled" type="checkbox" onchange="streamRelayChanged()"> Включить</label></div><div class="hint">StreamRelay использует выбранные выше Remote CA readers. Встроенный Radegast loopback передаёт ECM в OSCam; CA credentials здесь не хранятся.</div><div id="streamRelayFields" class="g" style="margin-top:12px"><label>Relay Port<input id="stream_relay_port" type="number" min="1" max="65535" value="17999"></label><label>Source Host<input id="stream_source_host" value="127.0.0.1"></label><label>Source Port<input id="stream_source_port" type="number" min="1" max="65535" value="8001"></label><label>CAID filter<input id="stream_relay_caid" placeholder="098C,09C4"></label><label>Radegast loopback Port<input id="stream_radegast_port" type="number" min="1" max="65535" value="9090"></label><label>Buffer time, ms<input id="stream_relay_buffer_time" type="number" min="0" max="30000" value="0"></label><label style="flex-direction:row;align-items:center"><input id="stream_client_source_host" type="checkbox"> Source host from client</label></div></div>
<div class="c"><button onclick="save()">Сохранить и перезапустить</button><span id="msg" class="msg"></span></div>
<div class="c"><h3>Процесс / журнал</h3><pre id="proc"></pre><pre id="log"></pre></div>
</div>
<script>
function normalizeLanguage(value){return value==='ru'?'ru':'en'}
let language=normalizeLanguage(localStorage.getItem('dvbstreamer5-language')||'en');

const oscamRuToEn=new Map([
['Старт','Start'],
['Перезапуск','Restart'],
['Стоп','Stop'],
['Обновить','Refresh'],
['Конфиги:','Configs:'],
['Пользователи / порты','Users / ports'],
['+ Пользователь','+ User'],
['Строка пользователя свернута по умолчанию. Нажмите на неё, чтобы открыть настройки.','The user row is collapsed by default. Click it to open the settings.'],
['Ридеры / Remote CA OSCam-mini','Readers / Remote CA OSCam-mini'],
['+ Ридер','+ Reader'],
['Найти локальные ридеры','Detect local readers'],
['Локальные: mouse/phoenix/PCSC. Remote CA: Newcamd, Camd35/CS357x, CS378x, CCcam, Radegast, Gbox, Scam, GHTTP и Camd33. Выберите протокол — показываются только относящиеся к нему поля.','Local: mouse/phoenix/PCSC. Remote CA: Newcamd, Camd35/CS357x, CS378x, CCcam, Radegast, Gbox, Scam, GHTTP and Camd33. Select a protocol to show only its relevant fields.'],
['Включить','Enable'],
['StreamRelay использует выбранные выше Remote CA readers. Встроенный Radegast loopback передаёт ECM в OSCam; CA credentials здесь не хранятся.','StreamRelay uses the Remote CA readers selected above. The built-in Radegast loopback forwards ECM to OSCam; CA credentials are not stored here.'],
['Сохранить и перезапустить','Save and restart'],
['Процесс / журнал','Process / log'],
['Пользователь','User'],
['Пароль','Password'],
['Порт','Port'],
['Группы','Groups'],
['Удалить пользователя','Delete user'],
['— ручной ввод —','— manual input —'],
['Найдено ридеров:','Readers found:'],
['Поиск…','Searching…'],
['Имя','Name'],
['Найденный ридер','Detected reader'],
['Устройство / индекс PC/SC','Device / PC/SC index'],
['Протокол','Protocol'],
['Удалить ридер','Delete reader'],
['неактивен','inactive'],
['работает','running'],
['остановлен','stopped'],
['процесс не запущен','process not running'],
['Сохранение…','Saving…'],
['Сохранено и перезапущено','Saved and restarted'],
['Ошибка','Error'],
['OSCam-mini остановлен','OSCam-mini stopped'],
['Newcamd клиент подключён','Newcamd client connected'],
['Ожидание подключения','Waiting for connection'],
['Ридер отключён','Reader disabled'],
['Карта готова','Card ready'],
['Ошибка ридера','Reader error'],
['Инициализация карты','Initializing card'],
['Ожидание карты','Waiting for card'],
['DES key должен содержать ровно 28 HEX-символов','DES key must contain exactly 28 HEX characters'],
['Добавьте хотя бы одного пользователя Newcamd','Add at least one Newcamd user'],
['StreamRelay: проверьте Source Host/Port, Relay Port и Radegast Port','StreamRelay: check Source Host/Port, Relay Port and Radegast Port'],
['StreamRelay CAID filter должен содержать CAID/маски OSCam, например 098C,09C4','StreamRelay CAID filter must contain OSCam CAIDs/masks, for example 098C,09C4'],
['StreamRelay buffer time должен быть 0..30000 мс','StreamRelay buffer time must be 0..30000 ms'],
['У ридера отсутствует имя','Reader name is missing']
]);

function translateOscamText(value){
  const source=String(value??'');
  if(language!=='en'||!/[А-Яа-яЁё]/.test(source))return source;
  const match=source.match(/^(\s*)([\s\S]*?)(\s*)$/);
  const lead=match?match[1]:'', core=match?match[2]:source, tail=match?match[3]:'';
  if(oscamRuToEn.has(core))return lead+oscamRuToEn.get(core)+tail;
  let text=core;
  const replacements=[
    [/^Найдено ридеров: (\d+)$/,'Readers found: $1'],
    [/^Ошибка поиска: (.+)$/,'Search error: $1'],
    [/^⚠ текущий: (.+) — не найден$/,'⚠ current: $1 — not found'],
    [/^Remote CA (.+) подключён$/,'Remote CA $1 connected'],
    [/^Ошибка Remote CA (.+)$/,'Remote CA $1 error'],
    [/^Remote CA (.+) активируется$/,'Remote CA $1 is starting'],
    [/^Ожидание Remote CA (.+)$/,'Waiting for Remote CA $1'],
    [/^Некорректное имя пользователя: (.+)$/,'Invalid username: $1'],
    [/^Некорректный пароль пользователя (.+)$/,'Invalid password for user $1'],
    [/^Некорректный порт пользователя (.+)$/,'Invalid port for user $1'],
    [/^Порт (\d+) используется более одного раза$/,'Port $1 is used more than once'],
    [/^Пользователь (.+) указан более одного раза$/,'User $1 is specified more than once'],
    [/^CAID пользователя (.+) должен содержать 4 HEX-символа$/,'CAID for user $1 must contain 4 HEX characters'],
    [/^Provider пользователя (.+) должен содержать 6 HEX-символов$/,'Provider for user $1 must contain 6 HEX characters'],
    [/^Некорректные группы пользователя (.+)$/,'Invalid groups for user $1'],
    [/^Не удалось создать временный файл (.+)$/,'Failed to create temporary file $1'],
    [/^Ошибка записи (.+)$/,'Write error $1'],
    [/^Для PC\/SC укажите индекс ридера \(0, 1, \.\.\.\): (.+)$/,'Specify the PC/SC reader index (0, 1, ...): $1'],
    [/^Некорректное устройство ридера (.+)$/,'Invalid reader device $1'],
    [/^Для Remote CA укажите корректные Host и Port: (.+)$/,'Specify a valid Host and Port for Remote CA: $1'],
    [/^Некорректный Local Port Remote CA: (.+)$/,'Invalid Remote CA Local Port: $1'],
    [/^Remote CA ридер (.+) указывает на локальный Newcamd listener OSCam-mini и создаёт петлю$/,'Remote CA reader $1 points to the local OSCam-mini Newcamd listener and creates a loop'],
    [/^Для (.+) укажите корректные User и Password: (.+)$/,'Specify valid User and Password for $1: $2'],
    [/^DES key (.+) ридера (.+) должен содержать 28 HEX-символов$/,'DES key for $1 reader $2 must contain 28 HEX characters'],
    [/^Remote key ридера (.+) должен содержать 28 HEX-символов$/,'Remote key for reader $1 must contain 28 HEX characters'],
    [/^CCcam max hops должен быть от -1 до 20: (.+)$/,'CCcam max hops must be between -1 and 20: $1'],
    [/^CAID ридера (.+) должен содержать 4 HEX-символа$/,'Reader $1 CAID must contain 4 HEX characters'],
    [/^Некорректная группа ридера (.+)$/,'Invalid group for reader $1'],
    [/^Некорректная частота ридера (.+)$/,'Invalid frequency for reader $1'],
    [/^Неподдерживаемый протокол OSCam reader: (.+)$/,'Unsupported OSCam reader protocol: $1'],
    [/^BoxKey ридера (.+) должен содержать 16 HEX-символов$/,'Reader $1 BoxKey must contain 16 HEX characters'],
    [/^RSAKey ридера (.+) должен содержать HEX-строку чётной длины$/,'Reader $1 RSAKey must contain an even-length HEX string'],
    [/^AU Provider ридера (.+) должен содержать 6 HEX-символов$/,'Reader $1 AU Provider must contain 6 HEX characters'],
    [/^Конфигурация сохранена, но OSCam-mini не перезапущен: (.+)$/,'Configuration was saved, but OSCam-mini was not restarted: $1']
  ];
  for(const [pattern,replacement] of replacements)text=text.replace(pattern,replacement);
  text=text
    .replaceAll('Конфиги:','Configs:')
    .replaceAll('устройство не выбрано','no device selected')
    .replaceAll('обязателен для Newcamd','required for Newcamd');
  return lead+text+tail;
}

const oscamOriginalText=new WeakMap();
const oscamOriginalAttrs=new WeakMap();
const oscamLocalizedAttrs=['title','placeholder','aria-label'];

function localizeOscamTextNode(node){
  if(!node||node.nodeType!==Node.TEXT_NODE||['SCRIPT','STYLE','PRE','CODE'].includes(node.parentElement?.tagName))return;
  const current=node.nodeValue||'';
  if(language==='en'){
    if(/[А-Яа-яЁё]/.test(current))oscamOriginalText.set(node,current);
    const source=oscamOriginalText.get(node)??current;
    const translated=translateOscamText(source);
    if(translated!==current)node.nodeValue=translated;
  }else if(oscamOriginalText.has(node)){
    const original=oscamOriginalText.get(node);
    if(current!==original)node.nodeValue=original;
  }
}

function localizeOscamAttributes(element){
  if(!element||element.nodeType!==Node.ELEMENT_NODE||['SCRIPT','STYLE','PRE','CODE'].includes(element.tagName))return;
  let originals=oscamOriginalAttrs.get(element);
  if(!originals){originals=new Map();oscamOriginalAttrs.set(element,originals)}
  for(const name of oscamLocalizedAttrs){
    if(!element.hasAttribute(name))continue;
    const current=element.getAttribute(name)||'';
    if(language==='en'){
      if(/[А-Яа-яЁё]/.test(current))originals.set(name,current);
      const source=originals.get(name)??current;
      const translated=translateOscamText(source);
      if(translated!==current)element.setAttribute(name,translated);
    }else if(originals.has(name)){
      const original=originals.get(name);
      if(current!==original)element.setAttribute(name,original);
    }
  }
}

function applyOscamLanguage(root=document.body){
  document.documentElement.lang=language;
  if(!root)return;
  if(root.nodeType===Node.TEXT_NODE)localizeOscamTextNode(root);
  if(root.nodeType===Node.ELEMENT_NODE)localizeOscamAttributes(root);
  const walker=document.createTreeWalker(root,NodeFilter.SHOW_ELEMENT|NodeFilter.SHOW_TEXT);
  let node;
  while((node=walker.nextNode())){
    if(node.nodeType===Node.TEXT_NODE)localizeOscamTextNode(node);
    else localizeOscamAttributes(node);
  }
}

const oscamLocaleObserver=new MutationObserver(mutations=>{
  for(const mutation of mutations){
    if(mutation.type==='characterData')localizeOscamTextNode(mutation.target);
    else if(mutation.type==='attributes')localizeOscamAttributes(mutation.target);
    else for(const node of mutation.addedNodes)applyOscamLanguage(node);
  }
});
oscamLocaleObserver.observe(document.documentElement,{subtree:true,childList:true,characterData:true,attributes:true,attributeFilter:oscamLocalizedAttrs});

function syncOscamLanguage(){
  const selected=normalizeLanguage(localStorage.getItem('dvbstreamer5-language')||language);
  if(selected!==language){language=selected;applyOscamLanguage(document.body)}
}
window.addEventListener('storage',event=>{if(event.key==='dvbstreamer5-language')syncOscamLanguage()});
document.addEventListener('visibilitychange',()=>{if(!document.hidden)syncOscamLanguage()});

let devices=[];
let detectedReaders=[];
let lastStatus={};
function esc(v){return String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
async function api(url,opt){const r=await fetch(url,opt);let data;try{data=await r.json()}catch(e){throw new Error(await r.text()||('HTTP '+r.status))}return data}
function stateClass(s){return ['active','idle','down','disabled'].includes(s)?s:'down'}
function activityDot(s){s=stateClass(s);return `<span class="dot dot-${s}"></span>`}
function toggleItem(head){head.closest('.compact-item').classList.toggle('open')}
function streamRelayChanged(){const on=stream_relay_enabled.checked;streamRelayFields.style.opacity=on?'1':'.45';streamRelayFields.querySelectorAll('input').forEach(e=>e.disabled=!on)}

function userHtml(u={},i=0){const name=u.user||('user'+(i+1));return `<div class="compact-item user" data-user="${esc(name)}">
<button type="button" class="compact-head" onclick="toggleItem(this)">
<span class="activity-dot">${activityDot('down')}</span>
<span class="compact-main user-summary-name">${esc(name)}</span>
<span class="compact-sub hide-small user-summary-endpoint">${esc(u.port||11220+i)} · ${esc(u.caid||'0652')}:${esc(u.provider||'000000')}</span>
<span class="compact-sub hide-small user-summary-groups">group ${esc(u.groups||'1')}</span>
<span class="compact-status hide-small activity-text">неактивен</span>
<span class="chev">›</span>
</button>
<div class="compact-body"><div class="g">
<label>Пользователь<input class="user-name" value="${esc(name)}"></label>
<label>Пароль<input class="user-pass" type="password" value="${esc(u.password||'')}"></label>
<label>Порт<input class="user-port" type="number" min="1" max="65535" value="${esc(u.port||11220+i)}"></label>
<label>CAID<input class="user-caid" maxlength="4" value="${esc(u.caid||'0652')}"></label>
<label>Provider<input class="user-provider" maxlength="6" value="${esc(u.provider||'000000')}"></label>
<label>Группы<input class="user-groups" value="${esc(u.groups||'1')}" placeholder="6,12"></label>
</div><div class="row" style="margin-top:10px"><label style="flex-direction:row;align-items:center"><input class="user-au" type="checkbox" ${u.au!==false?'checked':''}> AU/EMM</label><button class="danger" onclick="this.closest('.user').remove()">Удалить пользователя</button></div></div></div>`}
function addUser(u={}){const box=document.createElement('div');box.innerHTML=userHtml(u,document.querySelectorAll('.user').length);users.append(...box.childNodes);applyActivity(lastStatus)}

function detectedReaderOptions(r={}){let match=-1;const out=['<option value="">— ручной ввод —</option>'];detectedReaders.forEach((d,i)=>{const selected=(d.protocol===r.protocol&&String(d.device)===String(r.device));if(selected)match=i;const stable=d.stable?' · stable':'';out.push(`<option value="${i}" ${selected?'selected':''}>${esc(d.name||d.device)} · ${esc((d.protocol||'').toUpperCase())} · ${esc(d.device)}${stable}</option>`)});if(r.device&&match<0)out.push(`<option value="_current" selected>⚠ текущий: ${esc(r.device)} (${esc(r.protocol||'mouse')}) — не найден</option>`);return out.join('')}
function chooseDetectedReader(sel){if(sel.value===''||sel.value==='_current')return;const d=detectedReaders[Number(sel.value)];if(!d)return;const row=sel.closest('.reader');row.querySelector('.reader-device').value=d.device;row.querySelector('.reader-protocol').value=d.protocol;row.querySelector('.reader-summary-device').textContent=(d.name||d.device)+' · '+d.device}
function updateReaderSelectors(){document.querySelectorAll('.reader').forEach(row=>{const r={protocol:row.querySelector('.reader-protocol').value,device:row.querySelector('.reader-device').value};row.querySelector('.reader-detected').innerHTML=detectedReaderOptions(r)});readerScan.textContent='Найдено ридеров: '+detectedReaders.length}
async function rescanReaders(){readerScan.textContent='Поиск…';try{const s=await api('/api/oscam-mini/settings');detectedReaders=s.detected_readers||[];updateReaderSelectors()}catch(e){readerScan.textContent='Ошибка поиска: '+e.message}}

function readerHtml(r={},i=0){const label=r.label||('Reader'+(i+1));return `<div class="compact-item reader" data-reader="${esc(label)}">
<button type="button" class="compact-head" onclick="toggleItem(this)">
<span class="activity-dot">${activityDot('down')}</span>
<span class="compact-main reader-summary-label">${esc(label)}</span>
<span class="compact-sub hide-small reader-summary-device">${esc(r.device||'устройство не выбрано')}</span>
<span class="compact-sub hide-small reader-summary-ca">CAID ${esc(r.caid||'0652')} · group ${esc(r.group||1)}</span>
<span class="compact-status hide-small activity-text">неактивен</span>
<span class="chev">›</span>
</button>
<div class="compact-body"><div class="g">
<label>Имя<input class="reader-label" value="${esc(label)}"></label>
<label class="reader-local">Найденный ридер<select class="reader-detected" onchange="chooseDetectedReader(this)">${detectedReaderOptions(r)}</select></label>
<label class="reader-local">Устройство / индекс PC/SC<input class="reader-device" value="${esc(r.device||'')}" placeholder="/dev/serial/by-id/... или 0"></label>
<label>CAID<input class="reader-caid" maxlength="4" value="${esc(r.caid||'0652')}"></label>
<label>Протокол<select class="reader-protocol" onchange="readerProtocolChanged(this)"><option value="mouse" ${r.protocol==='mouse'?'selected':''}>mouse</option><option value="phoenix" ${r.protocol==='phoenix'?'selected':''}>phoenix</option><option value="pcsc" ${r.protocol==='pcsc'?'selected':''}>PC/SC (OMNIKEY)</option><option value="newcamd" ${r.protocol==='newcamd'?'selected':''}>Newcamd 525</option><option value="newcamd524" ${r.protocol==='newcamd524'?'selected':''}>Newcamd 524</option><option value="camd35" ${r.protocol==='camd35'?'selected':''}>Camd35 / CS357x</option><option value="cs378x" ${r.protocol==='cs378x'?'selected':''}>CS378x TCP</option><option value="cccam" ${r.protocol==='cccam'?'selected':''}>CCcam</option><option value="cccam_ext" ${r.protocol==='cccam_ext'?'selected':''}>CCcam Extended</option><option value="radegast" ${r.protocol==='radegast'?'selected':''}>Radegast</option><option value="gbox" ${r.protocol==='gbox'?'selected':''}>Gbox</option><option value="scam" ${r.protocol==='scam'?'selected':''}>Scam</option><option value="ghttp" ${r.protocol==='ghttp'?'selected':''}>GHTTP</option><option value="camd33" ${r.protocol==='camd33'?'selected':''}>Camd33</option></select></label>
<label class="reader-remote">Remote Host<input class="reader-remote-host" value="${esc(r.remote_host||'')}" placeholder="10.110.15.252"></label>
<label class="reader-remote">Remote Port<input class="reader-remote-port" type="number" min="1" max="65535" value="${esc(r.remote_port||15000)}"></label>
<label class="reader-remote reader-local-port">Local Port (optional)<input class="reader-remote-local-port" type="number" min="0" max="65535" value="${esc(r.remote_local_port||0)}"></label>
<label class="reader-remote">Remote User<input class="reader-remote-user" value="${esc(r.remote_user||'')}" autocomplete="off"></label>
<label class="reader-remote">Remote Password<input class="reader-remote-password" type="password" value="${esc(r.remote_password||'')}" autocomplete="new-password"></label>
<label class="reader-remote reader-des-key" style="grid-column:1/-1">Remote DES key<input class="reader-remote-key" maxlength="28" value="${esc(r.remote_key||'')}" autocomplete="off" placeholder="28 HEX; обязателен для Newcamd"></label>
<label class="reader-remote reader-cccam">CCcam version<input class="reader-ccc-version" value="${esc(r.ccc_version||'')}" placeholder="2.3.0"></label>
<label class="reader-remote reader-cccam">CCcam max hops<input class="reader-ccc-max-hops" type="number" min="-1" max="20" value="${esc(r.ccc_max_hops??10)}"></label>
<label class="reader-remote reader-cccam" style="flex-direction:row;align-items:center"><input class="reader-ccc-keepalive" type="checkbox" ${r.ccc_keepalive?'checked':''}> CCcam keepalive</label>
<label class="reader-remote reader-cs378x" style="flex-direction:row;align-items:center"><input class="reader-remote-keepalive" type="checkbox" ${r.remote_keepalive?'checked':''}> CS378x keepalive</label>
<label class="reader-remote reader-ghttp" style="flex-direction:row;align-items:center"><input class="reader-remote-use-ssl" type="checkbox" ${r.remote_use_ssl?'checked':''}> GHTTP SSL</label>
<label class="reader-local">MHz (Phoenix)<input class="reader-mhz" type="number" value="${esc(r.mhz||600)}"></label>
<label class="reader-local">Card MHz<input class="reader-cardmhz" type="number" value="${esc(r.cardmhz||600)}"></label>
<label>Group<input class="reader-group" type="number" min="1" max="64" value="${esc(r.group||1)}"></label>
<label>Ident<input class="reader-ident" value="${esc(r.ident||'')}"></label>
<label class="reader-local">BoxKey<input class="reader-boxkey" autocomplete="off" value="${esc(r.boxkey||'')}" placeholder="16 HEX"></label>
<label class="reader-local">AU Provider<input class="reader-auprovid" maxlength="6" value="${esc(r.auprovid||'')}" placeholder="000652"></label>
<label class="reader-local">EMM cache<input class="reader-emmcache" value="${esc(r.emmcache||'1,3,2,0')}" placeholder="1,3,2,0"></label>
<label class="reader-local" style="grid-column:1/-1">RSAKey<input class="reader-rsakey" autocomplete="off" value="${esc(r.rsakey||'')}" placeholder="HEX RSA key"></label>
</div><div class="row" style="margin-top:10px"><button class="danger" onclick="this.closest('.reader').remove()">Удалить ридер</button></div></div></div>`}
function readerProtocolChanged(sel){const row=sel.closest('.reader');if(!row)return;const remote=['newcamd','newcamd524','newcamd525','camd35','cs357x','cs378x','cccam','cccam_ext','radegast','gbox','scam','ghttp','camd33'].includes(sel.value);row.querySelectorAll('.reader-remote').forEach(e=>e.style.display=remote?'flex':'none');row.querySelectorAll('.reader-local').forEach(e=>e.style.display=remote?'none':'flex');row.querySelectorAll('.reader-des-key').forEach(e=>e.style.display=(remote&&['newcamd','newcamd524','newcamd525'].includes(sel.value))?'flex':'none');row.querySelectorAll('.reader-cccam').forEach(e=>e.style.display=(remote&&['cccam','cccam_ext'].includes(sel.value))?'flex':'none');row.querySelectorAll('.reader-cs378x').forEach(e=>e.style.display=(remote&&sel.value==='cs378x')?'flex':'none');row.querySelectorAll('.reader-ghttp').forEach(e=>e.style.display=(remote&&sel.value==='ghttp')?'flex':'none');row.querySelectorAll('.reader-local-port').forEach(e=>e.style.display=(remote&&sel.value==='gbox')?'flex':'none');const summary=row.querySelector('.reader-summary-device');if(summary)summary.textContent=remote?((row.querySelector('.reader-remote-host')?.value||'remote')+':'+(row.querySelector('.reader-remote-port')?.value||'')):(row.querySelector('.reader-device')?.value||'устройство не выбрано')}
function addReader(r={}){const box=document.createElement('div');box.innerHTML=readerHtml(r,document.querySelectorAll('.reader').length);const node=box.firstElementChild;if(node){readers.append(node);readerProtocolChanged(node.querySelector('.reader-protocol'))}applyActivity(lastStatus)}

function setActivity(el,info){if(!el)return;const s=stateClass(info?.state||'down');const dot=el.querySelector('.activity-dot');const txt=el.querySelector('.activity-text');if(dot)dot.innerHTML=activityDot(s);if(txt){txt.textContent=info?.detail||'неактивен';txt.className=`compact-status hide-small activity-text activity-${s}`}}
function applyActivity(st){lastStatus=st||{};document.querySelectorAll('.user').forEach(el=>setActivity(el,(st.user_activity||{})[el.dataset.user]));document.querySelectorAll('.reader').forEach(el=>setActivity(el,(st.reader_activity||{})[el.dataset.reader]))}
async function refreshStatus(){try{const st=await api('/api/oscam-mini/status');lastStatus=st;state.textContent=st.service_active?'● работает':'● '+(st.service_state||'остановлен');state.className=st.service_active?'status-ok':'status-bad';proc.textContent=st.process||'процесс не запущен';log.textContent=st.log||'';applyActivity(st)}catch(e){}}

async function loadAll(){try{const st=await api('/api/oscam-mini/status');lastStatus=st;devices=st.devices||[];state.textContent=st.service_active?'● работает':'● '+(st.service_state||'остановлен');state.className=st.service_active?'status-ok':'status-bad';proc.textContent=st.process||'процесс не запущен';log.textContent=st.log||'';const s=await api('/api/oscam-mini/settings');detectedReaders=s.detected_readers||[];bind_ip.value=s.bind_ip||'127.0.0.1';key.value=s.key||'';stream_relay_enabled.checked=!!s.stream_relay_enabled;stream_relay_port.value=s.stream_relay_port||17999;stream_source_host.value=s.stream_source_host||'127.0.0.1';stream_source_port.value=s.stream_source_port||8001;stream_client_source_host.checked=!!s.stream_client_source_host;stream_relay_buffer_time.value=s.stream_relay_buffer_time??0;stream_relay_caid.value=s.stream_relay_caid||'';stream_radegast_port.value=s.stream_radegast_port||9090;streamRelayChanged();users.innerHTML='';(s.users||[]).forEach(addUser);readers.innerHTML='';(s.readers||[]).forEach(addReader);readerScan.textContent='Найдено ридеров: '+detectedReaders.length;applyActivity(st)}catch(e){msg.textContent=e.message;msg.className='msg error'}}

async function save(){const userRows=[...document.querySelectorAll('.user')].map(e=>({user:e.querySelector('.user-name').value,password:e.querySelector('.user-pass').value,port:+e.querySelector('.user-port').value,caid:e.querySelector('.user-caid').value.trim(),provider:e.querySelector('.user-provider').value.trim(),groups:e.querySelector('.user-groups').value.trim(),au:e.querySelector('.user-au').checked}));const readerRows=[...document.querySelectorAll('.reader')].map(e=>({label:e.querySelector('.reader-label').value,device:e.querySelector('.reader-device').value,caid:e.querySelector('.reader-caid').value.trim(),protocol:e.querySelector('.reader-protocol').value,remote_host:e.querySelector('.reader-remote-host').value.trim(),remote_port:+e.querySelector('.reader-remote-port').value,remote_local_port:+e.querySelector('.reader-remote-local-port').value,remote_user:e.querySelector('.reader-remote-user').value.trim(),remote_password:e.querySelector('.reader-remote-password').value,remote_key:e.querySelector('.reader-remote-key').value.trim(),ccc_version:e.querySelector('.reader-ccc-version').value.trim(),ccc_max_hops:+e.querySelector('.reader-ccc-max-hops').value,ccc_keepalive:e.querySelector('.reader-ccc-keepalive').checked,remote_keepalive:e.querySelector('.reader-remote-keepalive').checked,remote_use_ssl:e.querySelector('.reader-remote-use-ssl').checked,detect:'cd',mhz:+e.querySelector('.reader-mhz').value,cardmhz:+e.querySelector('.reader-cardmhz').value,group:+e.querySelector('.reader-group').value,ident:e.querySelector('.reader-ident').value.trim(),boxkey:e.querySelector('.reader-boxkey').value.trim(),rsakey:e.querySelector('.reader-rsakey').value.trim(),auprovid:e.querySelector('.reader-auprovid').value.trim(),emmcache:e.querySelector('.reader-emmcache').value.trim(),enabled:true}));const body={bind_ip:bind_ip.value.trim(),key:key.value.trim(),keepalive:true,stream_relay_enabled:stream_relay_enabled.checked,stream_relay_port:+stream_relay_port.value,stream_source_host:stream_source_host.value.trim(),stream_source_port:+stream_source_port.value,stream_client_source_host:stream_client_source_host.checked,stream_relay_buffer_time:+stream_relay_buffer_time.value,stream_relay_caid:stream_relay_caid.value.trim(),stream_radegast_port:+stream_radegast_port.value,users:userRows,readers:readerRows};msg.textContent='Сохранение…';msg.className='msg';try{const x=await api('/api/oscam-mini/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});msg.textContent=x.ok?'Сохранено и перезапущено':(x.error||'Ошибка');msg.className=x.ok?'msg success':'msg error';await loadAll()}catch(e){msg.textContent=e.message;msg.className='msg error'}}

async function act(action){try{const x=await api('/api/oscam-mini/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action})});if(!x.ok){msg.textContent=x.error||'Ошибка';msg.className='msg error'}await loadAll()}catch(e){msg.textContent=e.message;msg.className='msg error'}}
applyOscamLanguage(document.body);
loadAll();
setInterval(()=>{syncOscamLanguage();refreshStatus()},3000);
</script>
</body></html>)HTML";
}