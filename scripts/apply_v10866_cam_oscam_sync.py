from pathlib import Path


def rep(text, old, new, label, count=1):
    n = text.count(old)
    if n != count:
        raise SystemExit(f"{label}: expected {count} match(es), got {n}")
    return text.replace(old, new, count)

# Public synchronization hook used by CardManager on startup/config updates.
p = Path("src/OscamMiniManager.h")
s = p.read_text()
s = rep(s, '#include <vector>\n\nclass OscamMiniManager {', '#include <vector>\n\nstruct CamClientConfig;\n\nclass OscamMiniManager {', 'CamClientConfig forward declaration')
s = rep(
    s,
    '    std::string serviceActionJson(const std::string& body);\n    std::string renderPage();\n',
    '    std::string serviceActionJson(const std::string& body);\n    std::string renderPage();\n    bool synchronizeCamClients(const std::vector<CamClientConfig>& clients, std::string& error);\n',
    'public CAM synchronization hook',
)
s = rep(
    s,
    '    Settings loadLocked();\n    bool saveLocked(const Settings& settings, std::string& error);\n',
    '    Settings loadLocked();\n    bool mergeCamClientsLocked(Settings& settings, const std::vector<CamClientConfig>& clients,\n                               std::string& error, bool& changed);\n    bool saveLocked(const Settings& settings, std::string& error);\n',
    'private CAM merge hook',
)
p.write_text(s)

# OSCam-mini owns the generated downstream listener/account configuration.
p = Path("src/OscamMiniManager.cpp")
s = p.read_text()
s = rep(s, '#include "OscamMiniManager.h"\n', '#include "OscamMiniManager.h"\n#include "ConfigManager.h"\n', 'ConfigManager include')

helpers = r'''
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
'''
s = rep(
    s,
    '}\n\nOscamMiniManager& OscamMiniManager::instance() {',
    helpers + '\n}\n\nOscamMiniManager& OscamMiniManager::instance() {',
    'anonymous namespace CAM helpers',
)

merge_impl = r'''
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
            password.find_first_of("\\r\\n;#") != std::string::npos) {
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

'''
s = rep(
    s,
    'bool OscamMiniManager::saveLocked(const Settings& settings, std::string& error) {',
    merge_impl + 'bool OscamMiniManager::saveLocked(const Settings& settings, std::string& error) {',
    'CAM merge implementation',
)

sync_on_oscam_save = r'''
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

'''
s = rep(
    s,
    '    if (!saveLocked(settings, error)) {\n',
    sync_on_oscam_save + '    if (!saveLocked(settings, error)) {\n',
    'OSCam save CAM synchronization',
)
p.write_text(s)

# Run synchronization on startup and every CAM/config update before backend sessions start.
p = Path("src/CardManager.cpp")
s = p.read_text()
s = rep(s, '#include "CaBackend.h"\n', '#include "CaBackend.h"\n#include "OscamMiniManager.h"\n', 'OscamMiniManager include in CardManager')
s = rep(
    s,
    '    CaBackendManager::instance().configure(configured);\n',
    '    std::string oscamSyncError;\n    if (!OscamMiniManager::instance().synchronizeCamClients(configured, oscamSyncError)) {\n        std::cerr << "OSCam-mini CAM listener synchronization failed: " << oscamSyncError << std::endl;\n    }\n    CaBackendManager::instance().configure(configured);\n',
    'CardManager startup/config synchronization',
)
p.write_text(s)

# Version.
p = Path("src/AppVersion.h")
s = p.read_text()
s = rep(s, 'inline constexpr const char* kProgramVersion = "10.8.65";', 'inline constexpr const char* kProgramVersion = "10.8.66";', 'version bump')
p.write_text(s)

# Short operator documentation.
p = Path("OSCAM_MINI.md")
s = p.read_text()
section = '''## DVBStreamer5 CAM listener synchronization\n\nV10.8.66 synchronizes loopback Newcamd CAM clients with OSCam-mini downstream\nlisteners. A CAM client such as `127.0.0.1:4004` is matched to the enabled Remote\nNewcamd reader `remote_<cam-client-id>`; OSCam-mini then creates/updates the local\nlistener/account on that same port using the reader CAID/provider/group. The CAM\nclient DES key becomes the shared downstream OSCam Newcamd key. Multiple loopback\nCAM clients therefore must use the same downstream DES key. Direct remote CAM\nclients are left unchanged.\n\nSynchronization runs at DVBStreamer5 startup, after CAM-client configuration\nchanges, and again when OSCam-mini settings are saved. This prevents a saved CAM\nendpoint from silently pointing at a port that OSCam-mini does not listen on.\n\n'''
s = rep(s, '## Remote Newcamd readers\n', section + '## Remote Newcamd readers\n', 'documentation section')
p.write_text(s)
