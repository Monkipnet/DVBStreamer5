from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, found {count}")
    return text.replace(old, new, 1)


# Version
version_path = Path("src/AppVersion.h")
version = version_path.read_text()
version = replace_once(
    version,
    'inline constexpr const char* kProgramVersion = "10.8.107";',
    'inline constexpr const char* kProgramVersion = "10.8.108";',
    "version",
)
version_path.write_text(version)


# Header declarations
header_path = Path("src/HttpServer.h")
header = header_path.read_text()
header = replace_once(
    header,
    '    std::string handleSaveConfig(const std::string& body);\n',
    '    std::string handleConfigExport();\n'
    '    std::string handleConfigImport(const std::string& body);\n'
    '    std::string handleSaveConfig(const std::string& body);\n',
    "HttpServer declarations",
)
header_path.write_text(header)


cpp_path = Path("src/HttpServer.cpp")
cpp = cpp_path.read_text()

# API routes. Both endpoints stay behind the existing authenticated /api session.
route_anchor = '''            } else if (target == "/api/save-config") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleSaveConfig(req.body());
'''
route_replacement = '''            } else if (target == "/api/config/export") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleConfigExport();
            } else if (target == "/api/config/import") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleConfigImport(req.body());
            } else if (target == "/api/save-config") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleSaveConfig(req.body());
'''
cpp = replace_once(cpp, route_anchor, route_replacement, "config transfer API routes")

# Backend: export one self-describing bundle; import supports the bundle, a raw
# DVBStreamer5 config, and legacy TVStreamer5/TVStreammer5 JSON. Raw/legacy
# imports intentionally preserve the current panel credentials and HTTP port.
method_anchor = '''std::string HttpServer::handleSaveConfig(const std::string& body) {
'''
methods = r'''std::string HttpServer::handleConfigExport() {
    Json::Value backup;
    backup["format"] = "dvbstreamer5-config-backup";
    backup["format_version"] = 1;
    backup["program_version"] = kProgramVersion;
    backup["contains_sensitive_data"] = true;
    backup["config"] = configManager.config.toJson();
    backup["subscribers"] = configManager.subscribers.toJson();
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, backup);
}

std::string HttpServer::handleConfigImport(const std::string& body) {
    auto jsonReply = [](const Json::Value& value) {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        return Json::writeString(writer, value);
    };
    auto errorReply = [&](const std::string& message) {
        Json::Value response;
        response["result"] = "error";
        response["error"] = message;
        return jsonReply(response);
    };
    auto parseJsonText = [](const std::string& text, Json::Value& root, std::string& error) {
        Json::CharReaderBuilder builder;
        std::istringstream input(text);
        return Json::parseFromStream(builder, input, &root, &error);
    };
    auto canonicalOutputType = [](std::string type, bool cbr) {
        type = toLower(type);
        if (type == "udp_cbr" || type == "udpcbr") type = "udp-cbr";
        if (type == "udp_vbr" || type == "udpvbr") type = "udp-vbr";
        if (type == "udp") type = cbr ? "udp-cbr" : "udp-vbr";
        return type;
    };
    auto supportedOutputType = [](const std::string& type) {
        static const std::set<std::string> supported = {
            "udp-cbr", "udp-vbr", "rtp", "http", "hls", "srt",
            "rtsp", "rtmp", "youtube"
        };
        return supported.find(type) != supported.end();
    };
    auto portRequired = [](const std::string& type) {
        return type == "udp-cbr" || type == "udp-vbr" || type == "rtp" ||
               type == "srt" || type == "rtsp" || type == "rtmp";
    };

    Json::Value request;
    std::string parseError;
    if (!parseJsonText(body, request, parseError) || !request.isObject()) {
        return errorReply("invalid import request: " + parseError);
    }
    const std::string action = toLower(request.get("action", "preview").asString());
    if (action != "preview" && action != "apply") {
        return errorReply("unsupported import action");
    }
    const std::string sourceKind = toLower(request.get("source_kind", "auto").asString());
    const std::string filename = request.get("filename", "").asString();
    const std::string content = request.get("content", "").asString();
    if (content.empty()) return errorReply("configuration file is empty");

    Json::Value importedRoot;
    parseError.clear();
    if (!parseJsonText(content, importedRoot, parseError) || !importedRoot.isObject()) {
        return errorReply("invalid configuration JSON: " + parseError);
    }

    const bool backupBundle =
        importedRoot.get("format", "").asString() == "dvbstreamer5-config-backup";
    Json::Value configRoot = backupBundle ? importedRoot["config"] : importedRoot;
    if (!configRoot.isObject()) return errorReply("backup does not contain a valid config object");
    if (!configRoot.isMember("streams") || !configRoot["streams"].isArray()) {
        return errorReply("configuration does not contain a streams array");
    }

    std::string lowerFilename = toLower(filename);
    const bool legacyFilename =
        lowerFilename == "tvstreammersat5-config.json" ||
        lowerFilename == "tvstreammer5-config.json" ||
        lowerFilename == "tvstreamer5-config.json";
    const bool legacyImport = sourceKind == "tvstreammer5" || sourceKind == "tvstreamer5" ||
                              (sourceKind == "auto" && legacyFilename);

    // A raw config copied from disk can contain an AES-GCM password bound to a
    // different local key. Preserve current access for every raw/legacy import.
    // Only our explicit backup bundle contains a portable runtime password and
    // is allowed to restore login/password/http_port.
    const bool preserveAccess = legacyImport || !backupBundle;
    if (preserveAccess) {
        configRoot["login"] = configManager.config.login;
        configRoot["password"] = configManager.config.password;
        configRoot["http_port"] = configManager.config.httpPort;
    }

    AppConfig candidate = AppConfig::fromJson(configRoot);
    std::set<std::string> streamIds;
    std::size_t outputCount = 0;
    for (std::size_t index = 0; index < candidate.streams.size(); ++index) {
        const auto& stream = candidate.streams[index];
        if (stream.id.empty()) {
            return errorReply("stream #" + std::to_string(index + 1) + " has an empty id");
        }
        if (!streamIds.insert(stream.id).second) {
            return errorReply("duplicate stream id: " + stream.id);
        }
        if (!stream.testPattern && stream.inputUri.empty()) {
            return errorReply("stream " + stream.id + " has an empty input_uri");
        }
        const std::string primaryType = canonicalOutputType(stream.outputType, stream.cbr);
        if (!supportedOutputType(primaryType)) {
            return errorReply("stream " + stream.id + " has unsupported output type: " + stream.outputType);
        }
        if (portRequired(primaryType) && (stream.outputPort <= 0 || stream.outputPort > 65535)) {
            return errorReply("stream " + stream.id + " has invalid primary output port");
        }
        ++outputCount;
        for (std::size_t outIndex = 0; outIndex < stream.additionalOutputs.size(); ++outIndex) {
            const auto& output = stream.additionalOutputs[outIndex];
            const std::string type = canonicalOutputType(output.outputType, false);
            if (!supportedOutputType(type)) {
                return errorReply("stream " + stream.id + " has unsupported additional output type: " + output.outputType);
            }
            if (portRequired(type) && (output.outputPort <= 0 || output.outputPort > 65535)) {
                return errorReply("stream " + stream.id + " has invalid additional output port");
            }
            ++outputCount;
        }
    }

    std::set<std::string> camIds;
    for (const auto& client : candidate.camClients) {
        if (client.id.empty()) return errorReply("CAM client with empty id");
        if (!camIds.insert(client.id).second) return errorReply("duplicate CAM client id: " + client.id);
    }

    Json::Value subscribersRoot;
    bool hasSubscribers = false;
    if (backupBundle && importedRoot.isMember("subscribers")) {
        subscribersRoot = importedRoot["subscribers"];
        hasSubscribers = subscribersRoot.isObject();
        if (!hasSubscribers) return errorReply("backup subscribers section is invalid");
    }
    const std::string subscribersContent = request.get("subscribers_content", "").asString();
    if (!subscribersContent.empty()) {
        parseError.clear();
        if (!parseJsonText(subscribersContent, subscribersRoot, parseError) || !subscribersRoot.isObject()) {
            return errorReply("invalid subscribers JSON: " + parseError);
        }
        hasSubscribers = true;
    }

    SubscriberListConfig candidateSubscribers;
    if (hasSubscribers) candidateSubscribers = SubscriberListConfig::fromJson(subscribersRoot);

    Json::Value response;
    response["result"] = "ok";
    response["preview"] = action == "preview";
    response["source"] = legacyImport ? "TVStreammer5" : (backupBundle ? "DVBStreamer5 backup" : "DVBStreamer5 raw config");
    response["streams"] = Json::UInt64(candidate.streams.size());
    response["outputs"] = Json::UInt64(outputCount);
    response["cam_clients"] = Json::UInt64(candidate.camClients.size());
    response["mpts_outputs"] = Json::UInt64(candidate.mptsOutputs.size());
    response["subscribers"] = Json::UInt64(hasSubscribers ? candidateSubscribers.subscribers.size() : 0);
    response["subscribers_present"] = hasSubscribers;
    response["access_preserved"] = preserveAccess;
    response["target_http_port"] = candidate.httpPort;
    response["format_version"] = backupBundle ? importedRoot.get("format_version", 0).asInt() : 0;

    if (action == "preview") return jsonReply(response);

    const SubscriberListConfig previousSubscribers = configManager.subscribers;
    const bool filteringChanged = hasSubscribers &&
        previousSubscribers.filteringEnabled != candidateSubscribers.filteringEnabled;
    bool subscriberFileChanged = false;
    if (hasSubscribers) {
        configManager.subscribers = candidateSubscribers;
        if (!configManager.saveSubscribers()) {
            configManager.subscribers = previousSubscribers;
            return errorReply("failed to save imported subscribers");
        }
        subscriberFileChanged = true;
    }

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const std::string configPayload = Json::writeString(writer, configRoot);
    const std::string saveResult = handleSaveConfig(configPayload);
    Json::Value saveReply;
    parseError.clear();
    const bool parsedSaveReply = parseJsonText(saveResult, saveReply, parseError);
    if ((parsedSaveReply && saveReply.get("result", "").asString() == "error") ||
        (!parsedSaveReply && saveResult.find("error") != std::string::npos)) {
        if (subscriberFileChanged) {
            configManager.subscribers = previousSubscribers;
            (void)configManager.saveSubscribers();
        }
        return saveResult;
    }

    if (hasSubscribers) {
        (void)streamManager.enforceSubscriberAccess();
        if (filteringChanged) (void)streamManager.restartAllSrtOutputs();
    }

    response["preview"] = false;
    response["applied"] = true;
    response["target_http_port"] = configManager.config.httpPort;
    return jsonReply(response);
}

'''
cpp = replace_once(cpp, method_anchor, methods + method_anchor, "config transfer methods")

# System menu entry.
menu_anchor = '''<button class="system-menu-item" onclick="openTelegramModal();closeSystemMenu()" data-i18n="telegram">Telegram API</button>
'''
menu_replacement = '''<button class="system-menu-item" onclick="openConfigTransferModal();closeSystemMenu()">Конфигурация</button>
<button class="system-menu-item" onclick="openTelegramModal();closeSystemMenu()" data-i18n="telegram">Telegram API</button>
'''
cpp = replace_once(cpp, menu_anchor, menu_replacement, "config transfer system menu")

# Runtime English localization for the new static menu label.
locale_anchor = "  ['Тест', 'Test'],\n"
locale_replacement = "  ['Конфигурация', 'Configuration'],\n  ['Тест', 'Test'],\n"
cpp = replace_once(cpp, locale_anchor, locale_replacement, "config transfer localization")

# Browser UI. File contents are read locally by the browser and sent only to the
# authenticated server endpoint for preview/apply. The old file is never modified.
js_anchor = '''function openTelegramModal() {
'''
js = r'''let pendingConfigImportPayload = null;
function configTransferText(ru, en) { return language === 'ru' ? ru : en; }
function openConfigTransferModal() {
  pendingConfigImportPayload = null;
  openModal(`
    <h2>${configTransferText('Конфигурация', 'Configuration')}</h2>
    <div style="display:grid;gap:12px">
      <div style="padding:12px;border:1px solid rgba(255,255,255,.10);border-radius:10px;background:rgba(255,255,255,.025)">
        <strong>${configTransferText('Экспорт DVBStreamer5', 'Export DVBStreamer5')}</strong>
        <div style="margin:6px 0 10px;color:#9aa3b1;font-size:.78rem">${configTransferText('Один JSON содержит основной конфиг и абонентов. Файл содержит чувствительные параметры (Telegram/CAM/пароль панели) — храните его безопасно.', 'One JSON contains the main config and subscribers. The file contains sensitive settings (Telegram/CAM/panel password) — store it securely.')}</div>
        <button class="button-primary" onclick="exportDvbStreamerConfig()">${configTransferText('Экспортировать backup', 'Export backup')}</button>
      </div>
      <div style="padding:12px;border:1px solid rgba(255,255,255,.10);border-radius:10px;background:rgba(255,255,255,.025)">
        <strong>${configTransferText('Импорт / миграция', 'Import / migration')}</strong>
        <div class="form-grid full" style="margin-top:10px">
          <div class="form-row"><label>${configTransferText('Основной JSON', 'Main JSON')}</label><input id="configImportFile" type="file" accept=".json,application/json" /></div>
          <div class="form-row"><label>${configTransferText('Абоненты (необязательно для старого конфига)', 'Subscribers (optional for legacy config)')}</label><input id="configSubscribersFile" type="file" accept=".json,application/json" /></div>
        </div>
        <div style="display:flex;gap:8px;flex-wrap:wrap;margin-top:10px">
          <button class="button-secondary" onclick="previewConfigImport('dvbstreamer5')">${configTransferText('Проверить DVBStreamer5', 'Preview DVBStreamer5')}</button>
          <button class="button-secondary" onclick="previewConfigImport('tvstreammer5')">${configTransferText('Проверить TVStreammer5', 'Preview TVStreammer5')}</button>
        </div>
        <div id="configImportPreview" style="margin-top:12px;color:#cbd5e1;font-size:.8rem"></div>
      </div>
    </div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">${t('cancel')}</button>
      <button id="configImportApply" class="button-primary" onclick="applyConfigImport()" disabled>${configTransferText('Применить импорт', 'Apply import')}</button>
    </div>`);
}
async function exportDvbStreamerConfig() {
  try {
    const response = await fetch('/api/config/export', {method:'POST'});
    const text = await response.text();
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const blob = new Blob([text], {type:'application/json;charset=utf-8'});
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = `dvbstreamer5-config-backup-${new Date().toISOString().slice(0,10)}.json`;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
  } catch (error) {
    uiError(error?.message || error);
  }
}
async function previewConfigImport(sourceKind) {
  const mainFile = document.getElementById('configImportFile')?.files?.[0];
  const subscriberFile = document.getElementById('configSubscribersFile')?.files?.[0];
  const preview = document.getElementById('configImportPreview');
  const applyButton = document.getElementById('configImportApply');
  pendingConfigImportPayload = null;
  if (applyButton) applyButton.disabled = true;
  if (!mainFile) {
    if (preview) preview.textContent = configTransferText('Выберите основной JSON.', 'Select the main JSON file.');
    return;
  }
  try {
    const payload = {
      action:'preview', source_kind:sourceKind,
      filename:mainFile.name, content:await mainFile.text()
    };
    if (subscriberFile) {
      payload.subscribers_filename = subscriberFile.name;
      payload.subscribers_content = await subscriberFile.text();
    }
    const response = await fetch('/api/config/import', {
      method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload)
    });
    const data = await response.json();
    if (!response.ok || data.result !== 'ok') throw new Error(data.error || `HTTP ${response.status}`);
    pendingConfigImportPayload = payload;
    const access = data.access_preserved
      ? configTransferText('Доступ к текущей панели будет сохранён.', 'Current panel access will be preserved.')
      : configTransferText('Backup может восстановить login/password/httpPort.', 'The backup may restore login/password/httpPort.');
    const subscribers = data.subscribers_present
      ? `${data.subscribers}`
      : configTransferText('не изменяются', 'unchanged');
    if (preview) preview.innerHTML = `
      <div><strong>${escapeHtmlValue(data.source || '')}</strong></div>
      <div>${configTransferText('Потоки', 'Streams')}: <b>${Number(data.streams||0)}</b> · OUT: <b>${Number(data.outputs||0)}</b> · CAM: <b>${Number(data.cam_clients||0)}</b> · MPTS: <b>${Number(data.mpts_outputs||0)}</b></div>
      <div>${configTransferText('Абоненты', 'Subscribers')}: <b>${escapeHtmlValue(subscribers)}</b></div>
      <div style="margin-top:6px;color:#9fd3ff">${escapeHtmlValue(access)}</div>`;
    if (applyButton) applyButton.disabled = false;
  } catch (error) {
    if (preview) preview.textContent = configTransferText('Ошибка: ', 'Error: ') + (error?.message || error);
  }
}
async function applyConfigImport() {
  if (!pendingConfigImportPayload) return;
  const message = configTransferText(
    'Применить импорт? Активные потоки с изменённой конфигурацией будут перезапущены.',
    'Apply import? Active streams with changed configuration will be restarted.'
  );
  if (!confirm(message)) return;
  const applyButton = document.getElementById('configImportApply');
  if (applyButton) applyButton.disabled = true;
  try {
    const payload = {...pendingConfigImportPayload, action:'apply'};
    const response = await fetch('/api/config/import', {
      method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload)
    });
    const data = await response.json();
    if (!response.ok || data.result !== 'ok' || !data.applied) throw new Error(data.error || `HTTP ${response.status}`);
    alert(configTransferText('Конфигурация импортирована.', 'Configuration imported.'));
    const targetPort = Number(data.target_http_port || 0);
    if (targetPort > 0 && targetPort !== Number(window.location.port || (window.location.protocol === 'https:' ? 443 : 80))) {
      const next = new URL(window.location.href);
      next.port = String(targetPort);
      window.location.href = next.toString();
      return;
    }
    closeModal();
    await fetchState();
  } catch (error) {
    uiError(error?.message || error);
    if (applyButton) applyButton.disabled = false;
  }
}

'''
cpp = replace_once(cpp, js_anchor, js + js_anchor, "config transfer web UI")

cpp_path.write_text(cpp)
