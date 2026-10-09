std::string HttpServer::handleConfigExport() {
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

std::string HttpServer::handleSaveConfig(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
        const std::string message = "Invalid config payload: " + errs;
        std::cerr << message << std::endl;
        Json::Value response; response["result"] = "error"; response["error"] = message;
        Json::StreamWriterBuilder writer; return Json::writeString(writer, response);
    }
    const AppConfig previousConfig = configManager.config;
    const auto previousStreams = streamConfigById(previousConfig.streams);
    const auto managedSnapshot = streamManager.snapshot();
    AppConfig nextConfig = AppConfig::fromJson(root);
    if (!root.isMember("login") || root.get("login", "").asString().empty()) {
        nextConfig.login = configManager.config.login;
    }
    if (!root.isMember("password") || root.get("password", "").asString().empty()) {
        nextConfig.password = configManager.config.password;
    }
    if (!root.isMember("server_name") || root.get("server_name", "").asString().empty()) {
        nextConfig.serverName = configManager.config.serverName;
    }
    if (!root.isMember("http_port") || nextConfig.httpPort <= 0 || nextConfig.httpPort > 65535) {
        nextConfig.httpPort = configManager.config.httpPort;
    }
    if (!root.isMember("language")) {
        nextConfig.language = configManager.config.language;
    }
    if (!root.isMember("srt_vps_vds_optimization")) {
        nextConfig.srtVpsVdsOptimization = configManager.config.srtVpsVdsOptimization;
    }
    if (!root.isMember("cam_clients")) {
        nextConfig.camClients = configManager.config.camClients;
    }
    if (!root.isMember("mpts_outputs")) {
        nextConfig.mptsOutputs = configManager.config.mptsOutputs;
    }
    const bool hasStreamArray = root.isMember("streams") && root["streams"].isArray();
    const bool explicitlyAllowEmptyStreams = root.get("allow_empty_streams", false).asBool();
    if ((!hasStreamArray || nextConfig.streams.empty()) &&
        !previousConfig.streams.empty() && !explicitlyAllowEmptyStreams) {
        nextConfig.streams = previousConfig.streams;
        std::cerr << "Config save preserved " << previousConfig.streams.size()
                  << " existing stream(s): request contained no streams or an empty stream list"
                  << std::endl;
    }
    {
        std::set<std::string> validStreamIds;
        for (const auto& stream : nextConfig.streams) validStreamIds.insert(stream.id);
        for (auto& output : nextConfig.mptsOutputs) {
            output.services.erase(std::remove_if(output.services.begin(), output.services.end(),
                [&](const MptsServiceConfig& service) {
                    return !validStreamIds.count(service.streamId);
                }), output.services.end());
        }
        nextConfig.mptsOutputs.erase(std::remove_if(nextConfig.mptsOutputs.begin(), nextConfig.mptsOutputs.end(),
            [](const MptsOutputConfig& output) { return output.services.empty(); }),
            nextConfig.mptsOutputs.end());
    }
    // 203.67: the VPS/VDS SRT profile is a server-wide switch. Propagate the
    // runtime flag into every stream before comparison so active SRT pipelines
    // are hard-restarted and recreate their sockets when the switch changes.
    for (auto& stream : nextConfig.streams) {
        stream.srtVpsVdsOptimization = nextConfig.srtVpsVdsOptimization;
    }
    for (const auto& stream : nextConfig.streams) {
        if (stream.activationMode == "ondemand" &&
            !hasOutputType(stream, "http") && !hasOutputType(stream, "hls")) {
            Json::Value response; response["result"] = "error";
            response["error"] = "OnDemand stream requires at least one HTTP or HLS output: " + stream.id;
            Json::StreamWriterBuilder writer; return Json::writeString(writer, response);
        }
    }

    std::string listenerError;
    if (!validateHttpPortsForConfig(nextConfig, listenerError)) {
        std::cerr << "Config save rejected: " << listenerError << std::endl;
        Json::Value response; response["result"] = "error"; response["error"] = listenerError;
        Json::StreamWriterBuilder writer; return Json::writeString(writer, response);
    }
    const auto nextStreams = streamConfigById(nextConfig.streams);
    std::vector<std::string> streamsToStop;
    std::vector<StreamConfig> streamsToRestart;
    for (const auto& [id, state] : managedSnapshot) {
        (void)state;
        const auto next = nextStreams.find(id);
        if (next == nextStreams.end()) {
            streamsToStop.push_back(id);
            continue;
        }
        const auto previous = previousStreams.find(id);
        if (previous == previousStreams.end() || !sameStreamConfig(previous->second, next->second)) {
            if (next->second.activationMode == "ondemand") streamsToStop.push_back(id);
            else streamsToRestart.push_back(next->second);
        }
    }

    bool camClientsChanged = previousConfig.camClients.size() != nextConfig.camClients.size();
    if (!camClientsChanged) {
        for (std::size_t index = 0; index < previousConfig.camClients.size(); ++index) {
            if (previousConfig.camClients[index].toJson() != nextConfig.camClients[index].toJson()) {
                camClientsChanged = true;
                break;
            }
        }
    }

    configManager.config = nextConfig;
    configManager.save();
    // V10.8.100: CardManager::configure() synchronizes OSCam-mini and may
    // restart its systemd service when listener configuration changes. A
    // stream-only/partial config save must not restart OSCam or reload the CA
    // plugin when the CAM client configuration is byte-for-byte unchanged.
    if (camClientsChanged) {
        CardManager::instance().configure(configManager.config.camClients);
    }
    refreshHttpPorts();

    for (const auto& id : streamsToStop) {
        std::cerr << "Stopping stream after config save: " << id << std::endl;
        streamManager.stopStream(id);
    }
    for (const auto& stream : streamsToRestart) {
        std::cerr << "Hard restarting stream after config change: " << stream.id << std::endl;
        if (!streamManager.restartStream(stream)) {
            std::cerr << "Hard restart failed for stream: " << stream.id << std::endl;
        }
    }
    streamManager.configureMptsOutputs();
    Json::Value response; response["result"] = "ok";
    Json::StreamWriterBuilder writer; return Json::writeString(writer, response);
}

std::string HttpServer::handleMptsSave(const std::string& body) {
    Json::Value response;
    Json::Value request;
    Json::CharReaderBuilder readerBuilder;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors) ||
        !request.isMember("outputs") || !request["outputs"].isArray()) {
        response["result"] = "error";
        response["error"] = errors.empty() ? "MPTS request must contain outputs[]" : errors;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    std::set<std::string> availableStreams;
    for (const auto& stream : configManager.config.streams) availableStreams.insert(stream.id);
    std::set<std::string> ids;
    std::vector<MptsOutputConfig> outputs;
    for (const auto& item : request["outputs"]) {
        auto output = MptsOutputConfig::fromJson(item);
        if (output.id.empty() || cleanPathToken(output.id) != output.id) {
            response["result"] = "error";
            response["error"] = "MPTS output id must contain only letters, digits, '-' or '_'";
            Json::StreamWriterBuilder writer;
            return Json::writeString(writer, response);
        }
        if (!ids.insert(output.id).second) {
            response["result"] = "error";
            response["error"] = "Duplicate MPTS output id: " + output.id;
            Json::StreamWriterBuilder writer;
            return Json::writeString(writer, response);
        }
        if (output.services.empty()) {
            response["result"] = "error";
            response["error"] = "MPTS output has no services: " + output.id;
            Json::StreamWriterBuilder writer;
            return Json::writeString(writer, response);
        }
        for (const auto& service : output.services) {
            if (!availableStreams.count(service.streamId)) {
                response["result"] = "error";
                response["error"] = "MPTS references unknown stream: " + service.streamId;
                Json::StreamWriterBuilder writer;
                return Json::writeString(writer, response);
            }
        }
        in_addr address{};
        if (::inet_pton(AF_INET, output.outputHost.c_str(), &address) != 1) {
            response["result"] = "error";
            response["error"] = "Invalid MPTS IPv4 output address: " + output.outputHost;
            Json::StreamWriterBuilder writer;
            return Json::writeString(writer, response);
        }
        outputs.push_back(std::move(output));
    }

    const auto previous = configManager.config.mptsOutputs;
    configManager.config.mptsOutputs = std::move(outputs);
    if (!configManager.save()) {
        configManager.config.mptsOutputs = previous;
        response["result"] = "error";
        response["error"] = "Failed to save MPTS configuration";
    } else {
        streamManager.configureMptsOutputs();
        response = streamManager.mptsSnapshot();
        response["result"] = "ok";
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::handleMptsAction(const std::string& body) {
    Json::Value response;
    Json::Value request;
    Json::CharReaderBuilder readerBuilder;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["result"] = "error";
        response["error"] = "Invalid MPTS action: " + errors;
    } else {
        const std::string id = request.get("id", "").asString();
        const std::string action = toLower(request.get("action", "").asString());
        std::string error;
        bool ok = false;
        if (action == "start") {
            ok = streamManager.startMptsOutput(id, &error);
        } else if (action == "stop") {
            ok = streamManager.stopMptsOutput(id);
            if (!ok) error = "MPTS output not found: " + id;
        } else if (action == "restart") {
            streamManager.stopMptsOutput(id);
            ok = streamManager.startMptsOutput(id, &error);
        } else {
            error = "Unknown MPTS action: " + action;
        }
        response = streamManager.mptsSnapshot();
        response["result"] = ok ? "ok" : "error";
        if (!ok) response["error"] = error;
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::listBackupFiles() {
    Json::Value root;
    Json::Value files(Json::arrayValue);
    const auto directory = std::filesystem::current_path() / "backup-files";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        root["error"] = "failed to create backup-files directory";
    } else {
        struct BackupFileEntry {
            std::filesystem::path path;
            std::filesystem::file_time_type modified;
            uintmax_t size = 0;
        };
        std::vector<BackupFileEntry> entries;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }
            BackupFileEntry item;
            item.path = entry.path();
            item.modified = entry.last_write_time(ec);
            if (ec) {
                ec.clear();
                item.modified = std::filesystem::file_time_type::min();
            }
            item.size = entry.file_size(ec);
            if (ec) {
                ec.clear();
                item.size = 0;
            }
            entries.push_back(std::move(item));
        }
        std::sort(entries.begin(), entries.end(), [](const BackupFileEntry& left, const BackupFileEntry& right) {
            return left.modified > right.modified;
        });
        for (const auto& entry : entries) {
            Json::Value item;
            item["name"] = entry.path.filename().string();
            item["path"] = entry.path.string();
            item["size"] = Json::UInt64(entry.size);
            files.append(item);
        }
    }
    root["files"] = files;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string HttpServer::handleUploadBackupFile(const std::string& target, const std::string& body) {
    Json::Value root;
    if (body.empty()) {
        root["error"] = "empty file";
    } else {
        const std::string streamId = cleanPathToken(urlDecode(queryValue(target, "stream_id")));
        const std::string requestedName = cleanFileName(urlDecode(queryValue(target, "filename")));
        const std::string prefix = streamId.empty() ? "stream" : streamId;
        const auto directory = std::filesystem::current_path() / "backup-files";
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            root["error"] = "failed to create backup-files directory";
        } else {
            const auto destination = directory /
                (prefix + "-" + std::to_string(unixNowSeconds()) + "-" + requestedName);
            std::ofstream output(destination, std::ios::binary);
            if (!output.is_open()) {
                root["error"] = "failed to open destination file";
            } else {
                output.write(body.data(), static_cast<std::streamsize>(body.size()));
                output.close();
                root["path"] = destination.string();
                root["filename"] = requestedName;
                root["size"] = Json::UInt64(body.size());
            }
        }
    }

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string HttpServer::handleDeleteBackupFile(const std::string& body) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value request;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["error"] = "invalid request";
    } else {
        const std::string requestedName = request.get("name", "").asString();
        const std::string safeName = cleanFileName(requestedName);
        if (requestedName.empty() || safeName != requestedName) {
            response["error"] = "invalid filename";
        } else {
            const auto directory = std::filesystem::current_path() / "backup-files";
            const auto filePath = directory / safeName;
            std::error_code ec;
            if (!std::filesystem::exists(filePath, ec) || ec || !std::filesystem::is_regular_file(filePath, ec)) {
                response["error"] = "file not found";
            } else if (!std::filesystem::remove(filePath, ec) || ec) {
                response["error"] = "failed to delete file";
            } else {
                response["result"] = "ok";
                response["name"] = safeName;
                response["path"] = filePath.string();
            }
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::handleStartStream(const std::string& body) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
        const std::string message = "Invalid start-stream payload: " + errs;
        std::cerr << message << std::endl;
        response["result"] = "error";
        response["error"] = message;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    const std::string streamId = root.get("id", "").asString();
    const auto configuredStream = std::find_if(
        configManager.config.streams.begin(), configManager.config.streams.end(),
        [&streamId](const StreamConfig& stream) { return stream.id == streamId; });
    if (streamId.empty() || configuredStream == configManager.config.streams.end()) {
        const std::string message = streamId.empty()
            ? "missing stream id"
            : ("stream is not present in saved configuration: " + streamId);
        std::cerr << "Start stream rejected: " << message << std::endl;
        response["result"] = "error";
        response["error"] = message;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    const StreamConfig cfg = *configuredStream;
    std::string startError;
    bool started = streamManager.startStream(cfg, &startError);
    if (!started && streamManager.isStreamActive(cfg.id)) started = true;

    response["stream_id"] = cfg.id;
    if (started) {
        response["result"] = "ok";
        if (!cfg.conditionalAccessClient.empty()) {
            response["ca"] = CardManager::instance().streamState(cfg.id);
        }
    } else {
        const std::string message = startError.empty()
            ? ("Failed to start stream: " + (cfg.name.empty() ? cfg.id : cfg.name))
            : startError;
        std::cerr << "Start stream failed: id=" << cfg.id
                  << " name=" << (cfg.name.empty() ? cfg.id : cfg.name)
                  << " error=" << message << std::endl;
        response["result"] = "error";
        response["error"] = message;
    }

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::handleStopStream(const std::string& body) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
        const std::string message = "Invalid stop-stream payload: " + errs;
        std::cerr << message << std::endl;
        response["result"] = "error";
        response["error"] = message;
    } else {
        const std::string id = root.get("id", "").asString();
        if (id.empty()) {
            response["result"] = "error";
            response["error"] = "missing stream id";
        } else {
            const bool stopped = streamManager.stopStreamAsync(id);
            const bool configured = std::any_of(
                configManager.config.streams.begin(), configManager.config.streams.end(),
                [&id](const StreamConfig& stream) { return stream.id == id; });
            if (stopped || configured) {
                response["result"] = "ok";
                response["already_stopped"] = !stopped;
                response["status"] = "stopped";
            } else {
                response["result"] = "error";
                response["error"] = "stream is not configured: " + id;
            }
            response["stream_id"] = id;
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

void HttpServer::handleRestartProgram() {
    try {
        std::thread([]() {
            // Let the HTTP response leave the socket before systemd stops this
            // process. --no-block is essential: a synchronous `systemctl restart`
            // launched from the service can wait for the very process that is
            // executing it. Queue the restart job and let PID 1 perform a full
            // stop/start with a new process, new allocator and new media-engine state.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::cerr << "PROGRAM RESTART 202.60: action=systemd-full-restart service=dvbstreamer5.service"
                      << std::endl;
            const int rc = std::system(
                "/usr/bin/systemctl --no-block restart dvbstreamer5.service >/dev/null 2>&1");
            if (rc != 0) {
                std::cerr << "PROGRAM RESTART 202.60: systemctl failed rc=" << rc << std::endl;
            }
        }).detach();
    } catch (const std::exception& ex) {
        std::cerr << "Restart worker thread creation failed: " << ex.what() << std::endl;
    }
}

  void HttpServer::handleDeleteStream(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
      std::cerr << "Invalid delete-stream payload: " << errs << std::endl;
      return;
    }
    const std::string id = root.get("id", "").asString();
    if (id.empty()) return;
    streamManager.stopStream(id);
    {
      std::lock_guard<std::mutex> lock(qualityMutex);
      qualitySamples.erase(id);
      qualityLastCompaction.erase(id);
    }
    {
      std::lock_guard<std::mutex> lock(caDecodeUiMutex());
      caDecodeUiMemory().erase(id);
    }
    auto& streams = configManager.config.streams;
    streams.erase(std::remove_if(streams.begin(), streams.end(), [&id](const StreamConfig& stream) {
      return stream.id == id;
    }), streams.end());
    auto& mptsOutputs = configManager.config.mptsOutputs;
    for (auto& output : mptsOutputs) {
      output.services.erase(std::remove_if(output.services.begin(), output.services.end(),
        [&id](const MptsServiceConfig& service) { return service.streamId == id; }),
        output.services.end());
    }
    mptsOutputs.erase(std::remove_if(mptsOutputs.begin(), mptsOutputs.end(),
      [](const MptsOutputConfig& output) { return output.services.empty(); }), mptsOutputs.end());
    configManager.save();
    streamManager.configureMptsOutputs();
  }

  void HttpServer::handleSaveSubscribers(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
      std::cerr << "Invalid subscribers payload: " << errs << std::endl;
      return;
    }
    SubscriberListConfig next;
    next.filteringEnabled = root.get("filtering_enabled", false).asBool();
    if (root["blocked_ips"].isArray()) {
      for (const auto& item : root["blocked_ips"]) {
        const std::string ip = normalizeIpAddress(item.asString());
        if (!ip.empty() && std::find(next.blockedIps.begin(), next.blockedIps.end(), ip) == next.blockedIps.end()) {
          next.blockedIps.push_back(ip);
        }
      }
    } else {
      next.blockedIps = configManager.subscribers.blockedIps;
    }
    if (root["subscribers"].isArray()) {
      for (const auto& item : root["subscribers"]) {
        auto subscriber = SubscriberConfig::fromJson(item);
        if (!subscriber.name.empty() && (!subscriber.primaryIp.empty() || !subscriber.backupIp.empty())) {
          next.subscribers.push_back(std::move(subscriber));
        }
      }
    }
    const bool filteringModeChanged = configManager.subscribers.filteringEnabled != next.filteringEnabled;
    if (configManager.subscribers.toJson() == next.toJson()) {
      std::cerr << "Subscriber update unchanged; skipping session reset" << std::endl;
      return;
    }

    configManager.subscribers = std::move(next);
    configManager.saveSubscribers();
    const size_t reset = streamManager.enforceSubscriberAccess();
    if (reset > 0) {
      std::cerr << "Reset unauthorized stream sessions after subscriber update: " << reset << std::endl;
    }
    if (filteringModeChanged) {
      const size_t restarted = streamManager.restartAllSrtOutputs();
      std::cerr << "Restarted SRT outputs after filtering mode change: " << restarted << std::endl;
    }
  }

  void HttpServer::handleResetSubscriber(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) return;
    const std::string name = root.get("name", "").asString();
    for (const auto& subscriber : configManager.subscribers.subscribers) {
      if (subscriber.name != name) continue;
      size_t reset = 0;
      if (!subscriber.primaryIp.empty()) reset += streamManager.resetHttpSessions(subscriber.primaryIp);
      if (!subscriber.backupIp.empty() && subscriber.backupIp != subscriber.primaryIp) {
        reset += streamManager.resetHttpSessions(subscriber.backupIp);
      }
      const size_t restarted = streamManager.restartSrtOutputsForStreams(subscriber.streamIds);
      std::cerr << "Reset subscriber sessions: " << name << " (" << reset
                << "), restarted_srt_outputs=" << restarted << std::endl;
      return;
    }
  }

  void HttpServer::handleBlockStreamClient(const std::string& body, bool blocked) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
      std::cerr << "Invalid stream client block payload: " << errs << std::endl;
      return;
    }

    const std::string ip = normalizeIpAddress(root.get("ip", "").asString());
    boost::system::error_code addressError;
    boost::asio::ip::make_address(ip, addressError);
    if (ip.empty() || addressError) {
      std::cerr << "Invalid stream client IP: " << ip << std::endl;
      return;
    }

    auto& blockedIps = configManager.subscribers.blockedIps;
    const auto found = std::find(blockedIps.begin(), blockedIps.end(), ip);
    if (blocked && found == blockedIps.end()) {
      blockedIps.push_back(ip);
    } else if (!blocked && found != blockedIps.end()) {
      blockedIps.erase(found);
    } else {
      return;
    }
    configManager.saveSubscribers();

    if (blocked) {
      const size_t reset = streamManager.enforceSubscriberAccess();
      std::cerr << "Blocked stream client " << ip << ", reset_sessions=" << reset << std::endl;
    } else {
      std::cerr << "Unblocked stream client " << ip << std::endl;
    }
  }


