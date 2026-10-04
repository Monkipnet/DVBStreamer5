#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one anchor, found {count}: {old[:160]!r}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.70";',
    'inline constexpr const char* kProgramVersion = "10.8.71";')

replace_once(
    "src/ConfigManager.h",
    "    bool testPattern = false;\n    bool autoStart = false;\n",
    "    bool testPattern = false;\n"
    "    // V10.8.71: channel activation policy. Online keeps the full media/CA\n"
    "    // pipeline running; OnDemand starts it only for an observable client.\n"
    "    std::string activationMode = \"online\"; // online | ondemand\n"
    "    bool autoStart = false;\n")

replace_once(
    "src/ConfigManager.cpp",
    '    config.testPattern = root.get("test_pattern", false).asBool();\n'
    '    config.autoStart = root.get("auto_start", false).asBool();\n',
    '    config.testPattern = root.get("test_pattern", false).asBool();\n'
    '    config.activationMode = toLower(root.get("activation_mode", "online").asString());\n'
    '    if (config.activationMode != "ondemand") config.activationMode = "online";\n'
    '    config.autoStart = root.get("auto_start", false).asBool();\n')

replace_once(
    "src/ConfigManager.cpp",
    '    root["test_pattern"] = testPattern;\n'
    '    root["auto_start"] = autoStart;\n',
    '    root["test_pattern"] = testPattern;\n'
    '    root["activation_mode"] = activationMode == "ondemand" ? "ondemand" : "online";\n'
    '    root["auto_start"] = autoStart;\n')

replace_once(
    "src/main.cpp",
    '    for (const auto& stream : configManager.config.streams) {\n'
    '        if (!stream.autoStart) continue;\n'
    '        std::cerr << "Auto-starting stream: " << stream.id << std::endl;\n',
    '    for (const auto& stream : configManager.config.streams) {\n'
    '        if (!stream.autoStart || stream.activationMode == "ondemand") {\n'
    '            if (stream.autoStart && stream.activationMode == "ondemand") {\n'
    '                std::cerr << "On-demand stream kept idle at startup: " << stream.id << std::endl;\n'
    '            }\n'
    '            continue;\n'
    '        }\n'
    '        std::cerr << "Auto-starting stream: " << stream.id << std::endl;\n')

replace_once(
    "src/StreamManager.h",
    '#include <mutex>\n#include <string>\n',
    '#include <mutex>\n#include <set>\n#include <string>\n')

replace_once(
    "src/StreamManager.h",
    '    bool isStreamActive(const std::string& id);\n'
    '    std::vector<std::string> activeStreams();\n',
    '    bool isStreamActive(const std::string& id);\n'
    '    bool ensureOnDemandStream(const std::string& id, const std::string& source,\n'
    '                              std::string* error = nullptr);\n'
    '    std::vector<std::string> activeStreams();\n')

replace_once(
    "src/StreamManager.h",
    '    void monitorNativeStream(StreamState* state);\n\n'
    '    ConfigManager& configManager;\n',
    '    void monitorNativeStream(StreamState* state);\n'
    '    void monitorOnDemandStreams();\n\n'
    '    ConfigManager& configManager;\n')

replace_once(
    "src/StreamManager.h",
    '    std::map<std::string, HttpClientSession> adHocSessions;\n'
    '    mutable std::mutex managerMutex;\n'
    '    std::atomic<uint64_t> nextSessionId{0};\n',
    '    std::map<std::string, HttpClientSession> adHocSessions;\n'
    '    mutable std::mutex managerMutex;\n'
    '    std::mutex onDemandMutex;\n'
    '    std::set<std::string> onDemandStartedStreams;\n'
    '    std::map<std::string, std::chrono::steady_clock::time_point> onDemandLastActivity;\n'
    '    std::atomic<bool> onDemandMonitorStop{false};\n'
    '    std::thread onDemandMonitorThread;\n'
    '    std::atomic<uint64_t> nextSessionId{0};\n')

replace_once(
    "src/StreamManager.cpp",
    'StreamManager::StreamManager(ConfigManager& cfg, TelegramNotifier& notifier)\n'
    '    : configManager(cfg), telegramNotifier(notifier),\n'
    '      mptsOutputManager(std::make_unique<MptsOutputManager>()) {\n'
    '    configureMptsOutputs();\n'
    '}\n\n'
    'StreamManager::~StreamManager() {\n'
    '    stopAll();\n'
    '}\n',
    'StreamManager::StreamManager(ConfigManager& cfg, TelegramNotifier& notifier)\n'
    '    : configManager(cfg), telegramNotifier(notifier),\n'
    '      mptsOutputManager(std::make_unique<MptsOutputManager>()) {\n'
    '    configureMptsOutputs();\n'
    '    onDemandMonitorThread = std::thread(&StreamManager::monitorOnDemandStreams, this);\n'
    '}\n\n'
    'StreamManager::~StreamManager() {\n'
    '    onDemandMonitorStop.store(true, std::memory_order_release);\n'
    '    if (onDemandMonitorThread.joinable()) onDemandMonitorThread.join();\n'
    '    stopAll();\n'
    '}\n')

replace_once(
    "src/StreamManager.cpp",
    'bool StreamManager::isStreamActive(const std::string& id) {\n'
    '    std::lock_guard<std::mutex> lock(managerMutex);\n'
    '    const auto it = streams.find(id);\n'
    '    return it != streams.end() && it->second->active.load();\n'
    '}\n\n'
    'std::vector<std::string> StreamManager::activeStreams() {\n',
    '''bool StreamManager::isStreamActive(const std::string& id) {
    std::lock_guard<std::mutex> lock(managerMutex);
    const auto it = streams.find(id);
    return it != streams.end() && it->second->active.load();
}

bool StreamManager::ensureOnDemandStream(const std::string& id, const std::string& source,
                                         std::string* error) {
    if (error) error->clear();
    StreamConfig cfg;
    bool found = false;
    for (const auto& candidate : configManager.config.streams) {
        if (candidate.id == id) {
            cfg = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        if (error) *error = "stream is not configured";
        return false;
    }
    if (toLower(cfg.activationMode) != "ondemand") return true;

    std::lock_guard<std::mutex> demandLock(onDemandMutex);
    onDemandLastActivity[id] = std::chrono::steady_clock::now();
    if (isStreamActive(id)) return true;

    std::string startError;
    if (!startStream(cfg, &startError)) {
        // A manual start may win the race between the pre-check and startStream().
        if (isStreamActive(id)) return true;
        if (error) *error = startError.empty() ? "failed to start on-demand stream" : startError;
        std::cerr << "ONDEMAND ACTIVATE FAILED stream=" << id
                  << " source=" << source << std::endl;
        return false;
    }
    onDemandStartedStreams.insert(id);
    std::cerr << "ONDEMAND ACTIVATE stream=" << id
              << " source=" << source
              << " ca=" << (cfg.conditionalAccessClient.empty() ? "fta" : "managed")
              << std::endl;
    return true;
}

void StreamManager::monitorOnDemandStreams() {
    constexpr auto kIdleGrace = std::chrono::seconds(10);
    while (!onDemandMonitorStop.load(std::memory_order_acquire)) {
        for (int i = 0; i < 4 && !onDemandMonitorStop.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (onDemandMonitorStop.load(std::memory_order_acquire)) break;

        std::lock_guard<std::mutex> demandLock(onDemandMutex);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = onDemandStartedStreams.begin(); it != onDemandStartedStreams.end();) {
            const std::string id = *it;
            bool stillOnDemand = false;
            for (const auto& cfg : configManager.config.streams) {
                if (cfg.id == id) {
                    stillOnDemand = toLower(cfg.activationMode) == "ondemand";
                    break;
                }
            }
            if (!stillOnDemand) {
                onDemandLastActivity.erase(id);
                it = onDemandStartedStreams.erase(it);
                continue;
            }

            bool hasHttpClient = false;
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                for (const auto& [fd, session] : httpClients) {
                    (void)fd;
                    if (session.streamId == id && session.previewSession.empty()) {
                        hasHttpClient = true;
                        break;
                    }
                }
            }
            const auto activity = onDemandLastActivity.find(id);
            const bool recentActivity = activity != onDemandLastActivity.end() &&
                now - activity->second < kIdleGrace;
            if (hasHttpClient || recentActivity) {
                ++it;
                continue;
            }

            it = onDemandStartedStreams.erase(it);
            onDemandLastActivity.erase(id);
            if (isStreamActive(id)) {
                std::cerr << "ONDEMAND DEACTIVATE stream=" << id
                          << " reason=no-clients idle_s=10" << std::endl;
                stopStream(id);
            }
        }
    }
}

std::vector<std::string> StreamManager::activeStreams() {
''')

# DVB scan: one Online/OnDemand choice is applied globally to all selected services.
replace_once(
    "src/HttpServer.cpp",
    '    const int defaultPort =\n'
    '        (outputType == "srt" ? 7001 :\n'
    '         (outputType == "rtsp" ? 8554 :\n'
    '          ((outputType == "rtmp" || outputType == "youtube") ? 1935 :\n'
    '           ((outputType == "http" || outputType == "hls") ? configManager.config.httpPort : 5000))));\n'
    '    int basePort = std::clamp(request.get("base_port", defaultPort).asInt(), 1, 65535);\n'
    '    const std::string interfaceAddress = request.get("interface_address", "").asString();\n'
    '    const bool autoStart = request.get("auto_start", false).asBool();\n',
    '    const int defaultPort =\n'
    '        (outputType == "srt" ? 7001 :\n'
    '         (outputType == "rtsp" ? 8554 :\n'
    '          ((outputType == "rtmp" || outputType == "youtube") ? 1935 :\n'
    '           ((outputType == "http" || outputType == "hls") ? configManager.config.httpPort : 5000))));\n'
    '    int basePort = std::clamp(request.get("base_port", defaultPort).asInt(), 1, 65535);\n'
    '    const std::string interfaceAddress = request.get("interface_address", "").asString();\n'
    '    std::string activationMode = toLower(request.get("activation_mode", "online").asString());\n'
    '    if (activationMode != "ondemand") activationMode = "online";\n'
    '    const bool autoStart = request.get("auto_start", false).asBool();\n')

replace_once(
    "src/HttpServer.cpp",
    '    const bool sharedHttpPort = outputType == "http" || outputType == "hls";\n'
    '    const std::string conditionalAccessClient = request.get("conditional_access_client", "").asString();\n',
    '    const bool sharedHttpPort = outputType == "http" || outputType == "hls";\n'
    '    if (activationMode == "ondemand" && !sharedHttpPort) {\n'
    '        response["error"] = "OnDemand requires HTTP or HLS output";\n'
    '        Json::StreamWriterBuilder writer;\n'
    '        return Json::writeString(writer, response);\n'
    '    }\n'
    '    const std::string conditionalAccessClient = request.get("conditional_access_client", "").asString();\n')

replace_once(
    "src/HttpServer.cpp",
    '        config.inputMode = "auto";\n'
    '        config.testPattern = false;\n'
    '        config.autoStart = autoStart;\n'
    '        config.outputType = outputType;\n',
    '        config.inputMode = "auto";\n'
    '        config.testPattern = false;\n'
    '        config.activationMode = activationMode;\n'
    '        config.autoStart = autoStart && activationMode == "online";\n'
    '        config.outputType = outputType;\n')

# Public HTTP is the trigger. Private browser preview remains active-stream-only.
replace_once(
    "src/HttpServer.cpp",
    '''bool HttpServer::handleHttpStream(tcp::socket& socket, const std::string& target) {
    std::string id;
    if (!resolveHttpMpegTsTarget(configManager.config.streams, target, id)) {
        if (!resolvePrivatePreviewTarget(target, id) ||
            !findStreamConfigById(configManager.config.streams, id) ||
            !streamManager.isStreamActive(id)) return false;
    }
    if (id.empty()) return false;

    const std::string header =
''',
    '''bool HttpServer::handleHttpStream(tcp::socket& socket, const std::string& target) {
    std::string id;
    const bool publicHttp = resolveHttpMpegTsTarget(configManager.config.streams, target, id);
    if (!publicHttp) {
        if (!resolvePrivatePreviewTarget(target, id) ||
            !findStreamConfigById(configManager.config.streams, id) ||
            !streamManager.isStreamActive(id)) return false;
    }
    if (id.empty()) return false;
    if (publicHttp) {
        std::string demandError;
        if (!streamManager.ensureOnDemandStream(id, "http", &demandError)) {
            const std::string body = "On-demand stream unavailable\n";
            const std::string unavailable =
                "HTTP/1.1 503 Service Unavailable\\r\\n"
                "Server: DVBStreamer5\\r\\n"
                "Content-Type: text/plain; charset=utf-8\\r\\n"
                "Retry-After: 1\\r\\n"
                "Connection: close\\r\\n"
                "Content-Length: " + std::to_string(body.size()) + "\\r\\n\\r\\n" + body;
            boost::asio::write(socket, boost::asio::buffer(unavailable));
            return true;
        }
    }

    const std::string header =
''')

# Every live HLS request refreshes demand activity. The first manifest request may
# need a short wait while the segmenter publishes its first playlist.
replace_once(
    "src/HttpServer.cpp",
    '    const StreamConfig* cfg = findStreamConfigById(configManager.config.streams, id);\n'
    '    if (!cfg) return false;\n'
    '    const std::filesystem::path filePath = hlsStorageDirectory(*cfg) / fileName;\n'
    '    const std::string archivePlaylist = buildHlsArchivePlaylist(*cfg, fileName);\n',
    '    const StreamConfig* cfg = findStreamConfigById(configManager.config.streams, id);\n'
    '    if (!cfg) return false;\n'
    '    std::string demandError;\n'
    '    if (!streamManager.ensureOnDemandStream(id, "hls", &demandError)) {\n'
    '        res.result(http::status::service_unavailable);\n'
    '        res.set(http::field::content_type, "text/plain");\n'
    '        res.set("Retry-After", "1");\n'
    '        res.body() = "On-demand stream unavailable";\n'
    '        return true;\n'
    '    }\n'
    '    const std::filesystem::path filePath = hlsStorageDirectory(*cfg) / fileName;\n'
    '    const std::string archivePlaylist = buildHlsArchivePlaylist(*cfg, fileName);\n'
    '    if (archivePlaylist.empty() && cfg->activationMode == "ondemand" && filePath.extension() == ".m3u8") {\n'
    '        for (int attempt = 0; attempt < 30 && !std::filesystem::exists(filePath); ++attempt) {\n'
    '            std::this_thread::sleep_for(std::chrono::milliseconds(100));\n'
    '        }\n'
    '    }\n')

# API validation and active-stream transition rules.
replace_once(
    "src/HttpServer.cpp",
    '    for (auto& stream : nextConfig.streams) {\n'
    '        stream.srtVpsVdsOptimization = nextConfig.srtVpsVdsOptimization;\n'
    '    }\n\n'
    '    std::string listenerError;\n',
    '    for (auto& stream : nextConfig.streams) {\n'
    '        stream.srtVpsVdsOptimization = nextConfig.srtVpsVdsOptimization;\n'
    '    }\n'
    '    for (const auto& stream : nextConfig.streams) {\n'
    '        if (stream.activationMode == "ondemand" &&\n'
    '            !hasOutputType(stream, "http") && !hasOutputType(stream, "hls")) {\n'
    '            Json::Value response; response["result"] = "error";\n'
    '            response["error"] = "OnDemand stream requires at least one HTTP or HLS output: " + stream.id;\n'
    '            Json::StreamWriterBuilder writer; return Json::writeString(writer, response);\n'
    '        }\n'
    '    }\n\n'
    '    std::string listenerError;\n')

replace_once(
    "src/HttpServer.cpp",
    '        const auto previous = previousStreams.find(id);\n'
    '        if (previous == previousStreams.end() || !sameStreamConfig(previous->second, next->second)) {\n'
    '            streamsToRestart.push_back(next->second);\n'
    '        }\n',
    '        const auto previous = previousStreams.find(id);\n'
    '        if (previous == previousStreams.end() || !sameStreamConfig(previous->second, next->second)) {\n'
    '            if (next->second.activationMode == "ondemand") streamsToStop.push_back(id);\n'
    '            else streamsToRestart.push_back(next->second);\n'
    '        }\n')

replace_once(
    "src/HttpServer.cpp",
    '        std::cerr << "Stopping removed stream after config save: " << id << std::endl;\n',
    '        std::cerr << "Stopping stream after config save: " << id << std::endl;\n')

# Tile rerender/status follows activation mode.
replace_once(
    "src/HttpServer.cpp",
    '    conditional_access_client: stream.conditional_access_client,\n'
    '    cbr: stream.cbr,\n',
    '    conditional_access_client: stream.conditional_access_client,\n'
    "    activation_mode: stream.activation_mode || 'online',\n"
    '    cbr: stream.cbr,\n')

replace_once(
    "src/HttpServer.cpp",
    "    statusPill.textContent = stream.active ? (stream.using_backup ? 'Backup' : 'Online') : 'Offline';\n",
    "    statusPill.textContent = stream.active ? (stream.using_backup ? 'Backup' : 'Online') : ((stream.activation_mode || 'online') === 'ondemand' ? 'OnDemand' : 'Offline');\n")

# Global DVB scan selector and payload.
replace_once(
    "src/HttpServer.cpp",
    '  const saveButton = document.getElementById(\'satSaveButton\');\n'
    '  if (saveButton) saveButton.disabled = true;\n'
    '  const payload = {\n',
    '  const saveButton = document.getElementById(\'satSaveButton\');\n'
    '  if (saveButton) saveButton.disabled = true;\n'
    "  const activationMode = document.getElementById('satActivationMode')?.value === 'ondemand' ? 'ondemand' : 'online';\n"
    "  const satelliteOutputType = document.getElementById('satOutputType')?.value || 'udp-vbr';\n"
    "  if (activationMode === 'ondemand' && !['http','hls'].includes(String(satelliteOutputType).toLowerCase())) {\n"
    "    alert('OnDemand требует выход HTTP или HLS. Для UDP/RTP/SRT/RTSP/RTMP выберите Online.');\n"
    '    if (saveButton) saveButton.disabled = false;\n'
    '    return;\n'
    '  }\n'
    '  const payload = {\n')

replace_once(
    "src/HttpServer.cpp",
    "    conditional_access_client:document.getElementById('satCamClientSelect')?.value || '',\n"
    "    auto_start:document.getElementById('satAutoStart')?.checked === true\n",
    "    conditional_access_client:document.getElementById('satCamClientSelect')?.value || '',\n"
    '    activation_mode:activationMode,\n'
    "    auto_start:activationMode === 'online' && document.getElementById('satAutoStart')?.checked === true\n")

replace_once(
    "src/HttpServer.cpp",
    '''      <div class="sat-field"><label>Выходной интерфейс</label><select id="satOutputInterface"><option value="">Авто (системный маршрут)</option>${(state.interfaces||[]).map(i=>`<option value="${satEscape(i.address)}">${satEscape(i.name)} (${satEscape(i.address)})</option>`).join('')}</select></div>
      <div class="sat-field wide"><label>Автозапуск</label><div class="checkbox-inline"><input id="satAutoStart" type="checkbox" /><span>Запускать созданные каналы после перезапуска</span></div></div>
''',
    '''      <div class="sat-field"><label>Выходной интерфейс</label><select id="satOutputInterface"><option value="">Авто (системный маршрут)</option>${(state.interfaces||[]).map(i=>`<option value="${satEscape(i.address)}">${satEscape(i.name)} (${satEscape(i.address)})</option>`).join('')}</select></div>
      <div class="sat-field wide"><label>Режим каналов транспондера</label><select id="satActivationMode" onchange="const a=document.getElementById('satAutoStart');if(a){a.disabled=this.value==='ondemand';if(this.value==='ondemand')a.checked=false;}"><option value="online">Online</option><option value="ondemand">OnDemand</option></select><small>Глобально для всех выбранных каналов. OnDemand запускает поток и CAM только при HTTP/HLS клиенте; UDP/RTP/SRT/RTSP/RTMP не могут определить пассивного получателя.</small></div>
      <div class="sat-field wide"><label>Автозапуск</label><div class="checkbox-inline"><input id="satAutoStart" type="checkbox" /><span>Запускать созданные каналы после перезапуска</span></div></div>
''')

# New-stream default and per-tile edit selector.
replace_once(
    "src/HttpServer.cpp",
    "conditional_access_client:'', test_pattern:false, auto_start:false, remap_enabled:false",
    "conditional_access_client:'', test_pattern:false, activation_mode:'online', auto_start:false, remap_enabled:false")

replace_once(
    "src/HttpServer.cpp",
    '''        <div class="form-row full"><label>Автозапуск</label><div class="checkbox-inline"><input id="streamAutoStart" type="checkbox" ${stream.auto_start ? 'checked' : ''} /><span>Запускать после перезапуска программы</span></div></div>
''',
    '''        <div class="form-row full"><label>Online / OnDemand</label><select id="streamActivationMode" onchange="const a=document.getElementById('streamAutoStart');if(a){a.disabled=this.value==='ondemand';if(this.value==='ondemand')a.checked=false;}"><option value="online" ${(stream.activation_mode||'online')==='online'?'selected':''}>Online</option><option value="ondemand" ${stream.activation_mode==='ondemand'?'selected':''}>OnDemand</option></select><small>OnDemand: вход, декодирование/CAM и выход запускаются по первому HTTP/HLS клиенту и останавливаются при отсутствии клиентов. Для UDP/RTP/SRT/RTSP/RTMP используйте Online.</small></div>
        <div class="form-row full"><label>Автозапуск</label><div class="checkbox-inline"><input id="streamAutoStart" type="checkbox" ${stream.auto_start ? 'checked' : ''} ${stream.activation_mode==='ondemand' ? 'disabled' : ''} /><span>Запускать после перезапуска программы</span></div></div>
''')

replace_once(
    "src/HttpServer.cpp",
    "  const selectedInputMode = document.getElementById('streamInputMode').value;\n"
    '  const payload = {\n',
    "  const selectedInputMode = document.getElementById('streamInputMode').value;\n"
    "  const activationMode = document.getElementById('streamActivationMode')?.value === 'ondemand' ? 'ondemand' : 'online';\n"
    "  if (activationMode === 'ondemand' && !outputs.some(output => ['http','hls'].includes(String(output.output_type || '').toLowerCase()))) {\n"
    "    uiError('OnDemand требует хотя бы один выход HTTP или HLS, где сервер может определить подключение клиента.');\n"
    '    return;\n'
    '  }\n'
    '  const payload = {\n')

replace_once(
    "src/HttpServer.cpp",
    "    test_pattern: document.getElementById('streamTestPattern').checked,\n"
    "    auto_start: document.getElementById('streamAutoStart').checked,\n",
    "    test_pattern: document.getElementById('streamTestPattern').checked,\n"
    '    activation_mode: activationMode,\n'
    "    auto_start: activationMode === 'online' && document.getElementById('streamAutoStart').checked,\n")

checks = {
    "src/AppVersion.h": ['kProgramVersion = "10.8.71"'],
    "src/ConfigManager.h": ['activationMode = "online"'],
    "src/StreamManager.cpp": ["ONDEMAND ACTIVATE", "ONDEMAND DEACTIVATE", "monitorOnDemandStreams"],
    "src/HttpServer.cpp": [
        "satActivationMode", "streamActivationMode",
        'ensureOnDemandStream(id, "http"', 'ensureOnDemandStream(id, "hls"',
        'activation_mode:activationMode'
    ],
}
for path, needles in checks.items():
    text = Path(path).read_text()
    for needle in needles:
        if needle not in text:
            raise SystemExit(f"{path}: missing expected feature marker {needle!r}")

print("V10.8.71 OnDemand patch applied")
