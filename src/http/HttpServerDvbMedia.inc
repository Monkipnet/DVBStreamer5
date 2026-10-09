std::string HttpServer::dvbAdapters() {
    Json::Value root = DvbSatellite::adapters();
    std::map<std::pair<int, int>, Json::Value> usage;

    // A DVB frontend is assigned as soon as a stream is configured for it.
    // OnDemand streams are intentionally included even while their runtime
    // state is stopped, so the scanner does not advertise an assigned
    // frontend as free merely because no subscriber is connected yet.
    for (const auto& config : configManager.config.streams) {
        DvbSatelliteParams params;
        std::string parseError;
        if (!DvbSatellite::parseUri(config.inputUri, params, parseError)) continue;
        const auto key = std::make_pair(params.adapter, params.frontend);
        auto& item = usage[key];
        item["configured_streams"] = item.get("configured_streams", 0).asUInt() + 1;
        if (toLower(config.activationMode) == "ondemand") {
            item["ondemand_streams"] = item.get("ondemand_streams", 0).asUInt() + 1;
        }
        // Preserve one configured tune even while an OnDemand frontend is idle.
        // An active runtime consumer below overrides these values with the tune
        // that is physically in use right now.
        if (!item.isMember("frequency_khz")) {
            item["frequency_khz"] = params.frequencyKHz;
            item["symbol_rate"] = params.symbolRateK;
            item["polarity"] = params.polarity;
            item["delivery_system"] = params.deliverySystem;
            item["modulation"] = params.modulation;
            item["fec"] = params.fec;
            item["stream_id"] = params.streamId;
            item["diseqc_source"] = params.diseqcSource;
            item["lnb_lof1_khz"] = params.lnbLof1KHz;
            item["lnb_lof2_khz"] = params.lnbLof2KHz;
            item["lnb_slof_khz"] = params.lnbSlofKHz;
        }
        if (!item.isMember("streams")) item["streams"] = Json::Value(Json::arrayValue);
        Json::Value stream;
        stream["id"] = config.id;
        stream["name"] = config.name;
        stream["sid"] = config.inputServiceId;
        stream["activation_mode"] = config.activationMode;
        item["streams"].append(stream);
    }

    // Keep the old active-consumer accounting separately. This preserves
    // SHARED N as a runtime indicator instead of conflating configured
    // OnDemand assignments with currently tuned consumers.
    const auto snap = streamManager.snapshot();
    for (const auto& [id, state] : snap) {
        if (!state || !state->running.load()) continue;
        DvbSatelliteParams params;
        std::string parseError;
        if (!DvbSatellite::parseUri(state->config.inputUri, params, parseError)) continue;
        const auto key = std::make_pair(params.adapter, params.frontend);
        auto& item = usage[key];
        item["consumers"] = item.get("consumers", 0).asUInt() + 1;
        item["frequency_khz"] = params.frequencyKHz;
        item["symbol_rate"] = params.symbolRateK;
        item["polarity"] = params.polarity;
        item["delivery_system"] = params.deliverySystem;
        item["modulation"] = params.modulation;
        item["fec"] = params.fec;
        item["stream_id"] = params.streamId;
        item["diseqc_source"] = params.diseqcSource;
        item["lnb_lof1_khz"] = params.lnbLof1KHz;
        item["lnb_lof2_khz"] = params.lnbLof2KHz;
        item["lnb_slof_khz"] = params.lnbSlofKHz;
    }

    if (root.isMember("adapters") && root["adapters"].isArray()) {
        for (Json::ArrayIndex i = 0; i < root["adapters"].size(); ++i) {
            auto& adapter = root["adapters"][i];
            const auto key = std::make_pair(adapter.get("adapter", 0).asInt(), adapter.get("frontend", 0).asInt());
            const auto found = usage.find(key);
            if (found == usage.end()) {
                adapter["in_use"] = false;
                adapter["consumers"] = 0;
                adapter["configured_streams"] = 0;
                adapter["ondemand_streams"] = 0;
            } else {
                const unsigned configured = found->second.get("configured_streams", 0).asUInt();
                const unsigned consumers = found->second.get("consumers", 0).asUInt();
                adapter["in_use"] = configured > 0 || consumers > 0;
                adapter["consumers"] = consumers;
                adapter["configured_streams"] = configured;
                adapter["ondemand_streams"] = found->second.get("ondemand_streams", 0);
                if (found->second.isMember("frequency_khz")) {
                    adapter["frequency_khz"] = found->second.get("frequency_khz", 0);
                    adapter["symbol_rate"] = found->second.get("symbol_rate", 0);
                    adapter["polarity"] = found->second.get("polarity", "");
                    adapter["delivery_system"] = found->second.get("delivery_system", "");
                    adapter["modulation"] = found->second.get("modulation", "auto");
                    adapter["fec"] = found->second.get("fec", "auto");
                    adapter["stream_id"] = found->second.get("stream_id", -1);
                    adapter["diseqc_source"] = found->second.get("diseqc_source", -1);
                    adapter["lnb_lof1_khz"] = found->second.get("lnb_lof1_khz", 9750000);
                    adapter["lnb_lof2_khz"] = found->second.get("lnb_lof2_khz", 10600000);
                    adapter["lnb_slof_khz"] = found->second.get("lnb_slof_khz", 11700000);
                }
                if (found->second.isMember("streams")) {
                    adapter["streams"] = found->second["streams"];
                }
            }
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string HttpServer::caManagerStatus() {
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, CardManager::instance().snapshot());
}

std::string HttpServer::handleCamClientSettings(const std::string& body) {
    Json::Value response;
    Json::Value request;
    Json::CharReaderBuilder readerBuilder;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["result"] = "error";
        response["error"] = "invalid CAM client settings: " + errors;
    } else {
        auto configToString = [](const Json::Value& value) {
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            return Json::writeString(writer, value.isNull() ? Json::Value(Json::objectValue) : value);
        };
        auto parseClient = [&](const Json::Value& item) {
            CamClientConfig updated;
            updated.id = item.get("id", item.get("client_id", "").asString()).asString();
            updated.name = item.get("name", updated.id).asString();
            updated.maxServices = std::clamp(item.get("max_services", 10).asUInt(), 1u,
                                             CardManager::kMaxConfigurableServices);
            updated.backendId = item.get("backend_id", "newcamd").asString();
            if (updated.backendId.empty() || updated.backendId == "passthrough") updated.backendId = "newcamd";
            if (item.isMember("backend_config")) {
                updated.backendConfig = item["backend_config"].isString()
                    ? item["backend_config"].asString()
                    : configToString(item["backend_config"]);
            } else {
                Json::Value backendConfig;
                backendConfig["host"] = item.get("host", "127.0.0.1").asString();
                backendConfig["port"] = item.get("port", 15000).asInt();
                backendConfig["user"] = item.get("user", "user").asString();
                backendConfig["pass"] = item.get("pass", "pass").asString();
                backendConfig["des"] = item.get("des", "0102030405060708091011121314").asString();
                updated.backendConfig = configToString(backendConfig);
            }
            if (updated.backendConfig.empty()) updated.backendConfig = "{}";
            return updated;
        };

        std::vector<CamClientConfig> nextClients;
        if (request["clients"].isArray()) {
            for (const auto& item : request["clients"]) {
                CamClientConfig client = parseClient(item);
                if (!client.id.empty()) nextClients.push_back(client);
            }
            configManager.config.camClients = nextClients;
        } else {
            CamClientConfig updated = parseClient(request);
            if (updated.id.empty()) {
                response["result"] = "error";
                response["error"] = "client id is required";
                Json::StreamWriterBuilder writer;
                return Json::writeString(writer, response);
            }
            bool replaced = false;
            for (auto& existing : configManager.config.camClients) {
                if (existing.id == updated.id) {
                    existing = updated;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) configManager.config.camClients.push_back(updated);
        }

        if (!configManager.save()) {
            response["result"] = "error";
            response["error"] = "failed to save CAM client settings";
        } else {
            CardManager::instance().configure(configManager.config.camClients);
            response = CardManager::instance().snapshot();
            response["result"] = "ok";
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::handleDvbTune(const std::string& body, bool scan) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value request;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["error"] = "invalid DVB request: " + errors;
    } else {
        response = scan ? DvbSatellite::scan(request) : DvbSatellite::signal(request);
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::handleDvbAddChannels(const std::string& body) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value request;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["error"] = "invalid channel selection: " + errors;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    if (!request["channels"].isArray() || request["channels"].size() == 0) {
        response["error"] = "No satellite channels selected";
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    std::string outputType = toLower(request.get("output_type", "udp-vbr").asString());
    static const std::set<std::string> kSatelliteOutputTypes = {
        "udp-vbr", "udp-cbr", "rtp", "srt", "http", "hls", "rtsp", "rtmp", "youtube"
    };
    if (!kSatelliteOutputTypes.count(outputType)) {
        response["error"] = "Unsupported satellite output protocol: " + outputType;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    std::string outputMode = toLower(request.get("output_mode", "listener").asString());
    if (outputMode != "listener" && outputMode != "caller") outputMode = "listener";
    if (outputType != "srt") outputMode = "listener";

    std::string outputHost = request.get("output_host", "").asString();
    if (outputHost.empty()) {
        if (outputType == "udp-vbr" || outputType == "udp-cbr" || outputType == "rtp") {
            outputHost = "239.255.10.1";
        } else if (outputType == "srt" && outputMode == "listener") {
            outputHost = "0.0.0.0";
        } else {
            outputHost = "127.0.0.1";
        }
    }

    const int defaultPort =
        (outputType == "srt" ? 7001 :
         (outputType == "rtsp" ? 8554 :
          ((outputType == "rtmp" || outputType == "youtube") ? 1935 :
           ((outputType == "http" || outputType == "hls") ? configManager.config.httpPort : 5000))));
    int basePort = std::clamp(request.get("base_port", defaultPort).asInt(), 1, 65535);
    const std::string interfaceAddress = request.get("interface_address", "").asString();
    std::string activationMode = toLower(request.get("activation_mode", "online").asString());
    if (activationMode != "ondemand") activationMode = "online";
    const bool autoStart = request.get("auto_start", false).asBool();

    bool cbr = request.get("cbr", outputType == "udp-cbr").asBool();
    if (outputType == "udp-cbr") cbr = true;
    if (outputType == "udp-vbr" || outputType == "rtp" || outputType == "rtsp" ||
        outputType == "rtmp" || outputType == "youtube") {
        cbr = false;
    }

    const int srtLatencyMs = std::clamp(request.get("srt_latency_ms", 120).asInt(), 20, 60000);
    const std::string srtPassphrase = request.get("srt_passphrase", "").asString();
    const std::string srtStreamId = request.get("srt_streamid", "").asString();
    int srtPbKeyLen = request.get("srt_pbkeylen", 16).asInt();
    if (srtPbKeyLen != 16 && srtPbKeyLen != 24 && srtPbKeyLen != 32) srtPbKeyLen = 16;

    const bool sharedHttpPort = outputType == "http" || outputType == "hls";
    if (activationMode == "ondemand" && !sharedHttpPort) {
        response["error"] = "OnDemand requires HTTP or HLS output";
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }
    const std::string conditionalAccessClient = request.get("conditional_access_client", "").asString();
    bool hasScrambledSelection = false;
    for (const auto& selected : request["channels"]) {
        if (selected.get("scrambled", false).asBool()) { hasScrambledSelection = true; break; }
    }
    if (hasScrambledSelection && (conditionalAccessClient.empty() || conditionalAccessClient == "auto")) {
        response["error"] = "Select a CAM client for scrambled channels";
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }
    const uint64_t targetBitrate = std::clamp<uint64_t>(
        request.get("target_bitrate_kbps", Json::UInt64(12000)).asUInt64(), 500, 100000) * 1000ULL;

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::set<int> occupiedPorts;
    for (const auto& existing : configManager.config.streams) {
        if (existing.outputHost == outputHost) occupiedPorts.insert(existing.outputPort);
        for (const auto& extra : existing.additionalOutputs) {
            if (extra.outputHost == outputHost) occupiedPorts.insert(extra.outputPort);
        }
    }
    int nextPort = basePort;
    size_t created = 0;
    size_t skipped = 0;
    Json::Value createdItems(Json::arrayValue);

    for (Json::ArrayIndex index = 0; index < request["channels"].size(); ++index) {
        const Json::Value& item = request["channels"][index];
        const uint32_t sid = item.get("service_id", 0).asUInt();
        const std::string inputUri = item.get("input_uri", "").asString();
        if (sid == 0 || inputUri.empty()) {
            ++skipped;
            continue;
        }

        DvbSatelliteParams params;
        std::string dvbError;
        if (!DvbSatellite::parseUri(inputUri, params, dvbError)) {
            ++skipped;
            continue;
        }

        const bool duplicate = std::any_of(
            configManager.config.streams.begin(), configManager.config.streams.end(),
            [&](const StreamConfig& existing) {
                return existing.inputUri == inputUri && existing.inputServiceId == sid;
            });
        if (duplicate) {
            ++skipped;
            continue;
        }

        StreamConfig config;
        config.id = "sat-" + std::to_string(now) + "-" + std::to_string(sid) + "-" + std::to_string(index);
        config.name = item.get("name", "").asString();
        if (config.name.empty()) config.name = "Satellite " + std::to_string(sid);
        config.inputUri = inputUri;
        config.inputMode = "auto";
        config.testPattern = false;
        config.activationMode = activationMode;
        config.autoStart = autoStart && activationMode == "online";
        config.outputType = outputType;
        config.outputMode = outputMode;
        config.outputHost = outputHost;
        if (sharedHttpPort) {
            // HTTP/HLS channels are separated by URL path, so every selected
            // service may share the same listener port.
            config.outputPort = basePort;
        } else {
            while (nextPort <= 65535 && occupiedPorts.count(nextPort)) ++nextPort;
            if (nextPort > 65535) {
                ++skipped;
                continue;
            }
            config.outputPort = nextPort;
            occupiedPorts.insert(nextPort);
            ++nextPort;
        }
        config.interfaceAddress = interfaceAddress;
        config.cbr = cbr;
        config.targetBitrate = targetBitrate;
        config.srtOutputLatencyMs = srtLatencyMs;
        config.srtOutputPassphrase = srtPassphrase;
        config.srtOutputStreamId = srtStreamId;
        config.srtOutputPbKeyLen = srtPbKeyLen;
        config.remapEnabled = false;
        config.transcodeEnabled = false;
        config.inputServiceId = sid;
        config.serviceId = sid;
        config.serviceName = config.name;
        config.serviceProvider = item.get("provider", "").asString();
        config.conditionalAccessClient = item.get("scrambled", false).asBool()
            ? conditionalAccessClient
            : "";
        config.videoPid = 0;
        config.audioPid = 0;

        configManager.config.streams.push_back(config);
        Json::Value createdItem = config.toJson();
        createdItems.append(createdItem);
        ++created;
    }

    if (created > 0) configManager.save();
    response["result"] = "ok";
    response["created"] = Json::UInt64(created);
    response["skipped"] = Json::UInt64(skipped);
    response["channels"] = createdItems;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

bool HttpServer::handleHttpStream(tcp::socket& socket, const std::string& target) {
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
        "HTTP/1.1 200 OK\r\n"
        "Server: DVBStreamer5\r\n"
        "Content-Type: video/MP2T\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    boost::asio::write(socket, boost::asio::buffer(header));
    boost::system::error_code endpointError;
    const std::string clientIp = socket.remote_endpoint(endpointError).address().to_string();
    int fd = socket.release();
    std::string privateId, previewToken;
    if (resolvePrivatePreviewTarget(target, privateId)) {
        // Validated by the GET handler before the response header is written.
        if (!privatePreviewTokenFromTarget(target, previewToken)) {
            ::close(fd);
            return false;
        }
    }
    streamManager.addHttpClient(id, fd, endpointError ? "" : clientIp, previewToken);
    return true;
}

bool HttpServer::serveHlsFile(const tcp::socket& socket, const std::string& target, http::response<http::string_body>& res) {
    std::string id;
    std::string fileName;
    if (!resolveHlsTarget(configManager.config.streams, target, id, fileName) ||
        fileName.find("..") != std::string::npos) {
        return false;
    }

    const StreamConfig* cfg = findStreamConfigById(configManager.config.streams, id);
    if (!cfg) return false;
    std::string demandError;
    if (!streamManager.ensureOnDemandStream(id, "hls", &demandError)) {
        res.result(http::status::service_unavailable);
        res.set(http::field::content_type, "text/plain");
        res.set("Retry-After", "1");
        res.body() = "On-demand stream unavailable";
        return true;
    }
    const std::filesystem::path filePath = hlsStorageDirectory(*cfg) / fileName;
    const std::string archivePlaylist = buildHlsArchivePlaylist(*cfg, fileName);
    if (archivePlaylist.empty() && cfg->activationMode == "ondemand" && filePath.extension() == ".m3u8") {
        // V10.8.91: an old OnDemand playlist can survive the previous generation
        // until the new segmenter clears its directory.  Waiting only for
        // video.m3u8 to exist can therefore return that stale manifest while its
        // referenced segment0000000000.ts is already being deleted by the new
        // generation.  Treat a live playlist as ready only when the first media
        // URI it advertises exists alongside it.
        const auto livePlaylistReady = [&filePath]() {
            std::error_code ec;
            if (!std::filesystem::exists(filePath, ec) || ec ||
                !std::filesystem::is_regular_file(filePath, ec) || ec) {
                return false;
            }

            std::ifstream probe(filePath, std::ios::binary);
            if (!probe.is_open()) return false;
            std::string line;
            while (std::getline(probe, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line.front() == '#') continue;
                const std::string relative = cleanHlsRelativePath(line);
                if (relative.empty()) return false;
                const auto mediaPath = filePath.parent_path() / relative;
                ec.clear();
                return std::filesystem::exists(mediaPath, ec) && !ec &&
                       std::filesystem::is_regular_file(mediaPath, ec) && !ec;
            }
            return false;
        };

        // Keep the V10.8.90 12-second startup budget, but spend it waiting for a
        // coherent playlist+first-media pair rather than for the manifest alone.
        for (int attempt = 0; attempt < 120 && !livePlaylistReady(); ++attempt) {
            // V10.8.92: this HTTP request itself is an active OnDemand viewer.
            // The decoder-safe first H.264/H.265 segment can legitimately take
            // longer than the normal 10 s idle window. Refresh OnDemand activity
            // while the request is blocked here so the monitor cannot stop the
            // stream underneath the pending playlist request. Once the request
            // completes, normal HLS playlist/segment requests keep activity alive
            // and the historical 10 s post-view idle shutdown remains unchanged.
            if ((attempt % 10) == 0) {
                std::string startupKeepaliveError;
                if (!streamManager.ensureOnDemandStream(
                        id, "hls-startup-wait", &startupKeepaliveError)) {
                    demandError = startupKeepaliveError;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (!archivePlaylist.empty()) {
        res.body() = archivePlaylist;
    } else {
        if (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath)) {
            res.result(http::status::not_found);
            res.set(http::field::content_type, "text/plain");
            res.body() = "Not Found";
            return true;
        }
        std::ifstream input(filePath, std::ios::binary);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        res.body() = buffer.str();
    }
    boost::system::error_code endpointError;
    const std::string clientIp = socket.remote_endpoint(endpointError).address().to_string();
    if (!endpointError && !clientIp.empty()) {
        streamManager.addStreamSession(id, clientIp, "hls");
    }
    if (filePath.extension() == ".m3u8") res.set(http::field::content_type, "application/vnd.apple.mpegurl");
    else if (filePath.extension() == ".m4s") res.set(http::field::content_type, "video/iso.segment");
    else if (filePath.extension() == ".mp4") res.set(http::field::content_type, "video/mp4");
    else if (filePath.extension() == ".bin") res.set(http::field::content_type, "application/octet-stream");
    else res.set(http::field::content_type, "video/MP2T");
    res.set(http::field::cache_control, "no-cache, no-store, must-revalidate");
    res.set("Access-Control-Allow-Origin", "*");
    res.keep_alive(false);
    return true;
}


