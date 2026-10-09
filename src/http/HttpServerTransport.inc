HttpServer::HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm)
    : ioContext(ioc), sessionPool(kHttpSessionWorkers), qualityHistoryTimer(ioc),
      configManager(cfg), streamManager(sm) {
    std::cerr << "HTTP worker pool 202.47: workers=" << kHttpSessionWorkers
              << " max_queued=" << kMaxQueuedHttpSessions
              << " thread_per_request=off" << std::endl;
}

bool HttpServer::start() {
    if (!bindHttpPorts(configuredHttpPorts())) return false;

    // V10.8.115: quality history belongs to the server, not to the browser.
    // Restore persisted samples, record immediately, then continue every
    // 30 seconds even when no Web UI client is connected.
    if (!qualitySamplerStarted) {
        loadQualityHistoryFromDisk();
        recordAllQualitySamples();
        qualityLastPersist = 0;
        qualitySamplerStarted = true;
        scheduleQualitySampler();
    }
    return true;
}

void HttpServer::doAccept(std::shared_ptr<tcp::acceptor> listener, int port, uint64_t generation) {
    if (!listener || !listener->is_open()) {
        return;
    }

    listener->async_accept([this, listener, port, generation](boost::system::error_code ec, tcp::socket socket) {
        if (generation != acceptGeneration.load()) {
            return;
        }
        if (!ec) {
            const uint32_t queued = queuedHttpSessions.fetch_add(1, std::memory_order_relaxed) + 1;
            if (queued > kMaxQueuedHttpSessions) {
                queuedHttpSessions.fetch_sub(1, std::memory_order_relaxed);
                std::cerr << "HTTP worker pool overload: queued=" << queued
                          << " limit=" << kMaxQueuedHttpSessions << std::endl;
                boost::system::error_code closeEc;
                socket.close(closeEc);
            } else {
                try {
                    boost::asio::post(sessionPool,
                        [this, socket = std::move(socket)]() mutable {
                            struct PendingGuard {
                                std::atomic<uint32_t>& counter;
                                ~PendingGuard() { counter.fetch_sub(1, std::memory_order_relaxed); }
                            } guard{queuedHttpSessions};
                            handleSession(std::move(socket));
                        });
                } catch (const std::exception& ex) {
                    queuedHttpSessions.fetch_sub(1, std::memory_order_relaxed);
                    std::cerr << "HTTP session enqueue failed: " << ex.what() << std::endl;
                    boost::system::error_code closeEc;
                    socket.close(closeEc);
                }
            }
        } else if (ec != boost::asio::error::operation_aborted) {
            std::cerr << "HTTP accept failed on port " << port << ": " << ec.message() << std::endl;
        }
        if (listener->is_open()) {
            doAccept(listener, port, generation);
        }
    });
}

void HttpServer::handleSession(tcp::socket socket) {
    try {
        boost::beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        parser.body_limit(512ULL * 1024ULL * 1024ULL);
        http::read(socket, buffer, parser);
        http::request<http::string_body> req = parser.release();
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "DVBStreamer5");
        res.set(http::field::content_type, "text/html; charset=UTF-8");
        res.set(http::field::cache_control, "no-store");
        res.set(http::field::pragma, "no-cache");
        // One accepted socket is handled for exactly one HTTP request in this
        // implementation. Advertising keep-alive while closing the socket after
        // the response makes HLS clients reuse a connection that no longer has a
        // request loop and can stall playlist/segment fetching after a few seconds.
        res.keep_alive(false);

        const std::string target(req.target());
        if (requiresAuthentication(target) && !isAuthorized(req)) {
            writeUnauthorized(res);
            res.content_length(res.body().size());
            http::write(socket, res);
            return;
        }

        if (req.method() == http::verb::head) {
            // Strict URL receivers (including NetUP Stream Processor) may probe
            // HTTP Progressive/HLS URLs with HEAD before opening the media GET.
            // Answer those probes without creating a playback session.
            std::string previewId;
            if (resolvePrivatePreviewTarget(target, previewId)) {
                if (!findStreamConfigById(configManager.config.streams, previewId) ||
                    !streamManager.isStreamActive(previewId)) {
                    res.result(http::status::not_found);
                    res.set(http::field::content_type, "text/plain");
                    res.body() = "Preview stream not running";
                } else {
                    res.result(http::status::ok);
                    res.set(http::field::content_type, "video/MP2T");
                    res.set(http::field::cache_control, "no-store");
                }
                res.keep_alive(false);
                http::write(socket, res);
                return;
            }
            std::string httpMpegTsId;
            if (resolveHttpMpegTsTarget(configManager.config.streams, target, httpMpegTsId)) {
                if (!isStreamClientAllowed(socket, target)) {
                    res.result(http::status::forbidden);
                    res.set(http::field::content_type, "text/plain");
                } else {
                    res.result(http::status::ok);
                    res.set(http::field::content_type, "video/MP2T");
                    res.set(http::field::cache_control, "no-cache, no-store, must-revalidate");
                }
                res.keep_alive(false);
                http::write(socket, res);
                return;
            }

            std::string hlsId;
            std::string hlsFile;
            if (resolveHlsTarget(configManager.config.streams, target, hlsId, hlsFile)) {
                if (!isStreamClientAllowed(socket, target)) {
                    res.result(http::status::forbidden);
                    res.set(http::field::content_type, "text/plain");
                    res.keep_alive(false);
                    http::write(socket, res);
                    return;
                }
                const StreamConfig* hlsCfg = findStreamConfigById(configManager.config.streams, hlsId);
                const std::filesystem::path filePath = hlsCfg
                    ? hlsStorageDirectory(*hlsCfg) / hlsFile
                    : std::filesystem::path();
                int64_t archiveFrom = 0, archiveTo = 0; bool archiveEvent = false;
                const bool dynamicArchive = hlsCfg && hlsCfg->hlsArchiveEnabled &&
                    parseHlsArchivePlaylistRequest(hlsFile, archiveFrom, archiveTo, archiveEvent);
                if (!dynamicArchive && (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath))) {
                    res.result(http::status::not_found);
                    res.set(http::field::content_type, "text/plain");
                    res.keep_alive(false);
                    http::write(socket, res);
                    return;
                }
                res.result(http::status::ok);
                if (filePath.extension() == ".m3u8") res.set(http::field::content_type, "application/vnd.apple.mpegurl");
                else if (filePath.extension() == ".m4s") res.set(http::field::content_type, "video/iso.segment");
                else if (filePath.extension() == ".mp4") res.set(http::field::content_type, "video/mp4");
                else if (filePath.extension() == ".bin") res.set(http::field::content_type, "application/octet-stream");
                else res.set(http::field::content_type, "video/MP2T");
                res.set(http::field::cache_control, "no-cache, no-store, must-revalidate");
                res.set("Access-Control-Allow-Origin", "*");
                res.keep_alive(false);
                http::write(socket, res);
                return;
            }
        }

        if (req.method() == http::verb::get) {
          std::string previewId;
          if (resolvePrivatePreviewTarget(target, previewId)) {
            if (!findStreamConfigById(configManager.config.streams, previewId) ||
                !streamManager.isStreamActive(previewId)) {
              res.result(http::status::not_found);
              res.set(http::field::content_type, "text/plain; charset=UTF-8");
              res.body() = "Preview stream not running";
            } else {
              std::string previewToken;
              if (!privatePreviewTokenFromTarget(target, previewToken)) {
                res.result(http::status::bad_request);
                res.set(http::field::content_type, "text/plain; charset=UTF-8");
                res.body() = "Missing or invalid preview session";
              } else if (handleHttpStream(socket, target)) {
                return;
              } else {
                res.result(http::status::service_unavailable);
                res.body() = "Preview relay unavailable";
              }
            }
          } else {
          std::string httpMpegTsId;
          if (resolveHttpMpegTsTarget(configManager.config.streams, target, httpMpegTsId)) {
            if (!isStreamClientAllowed(socket, target)) {
              res.result(http::status::forbidden);
              res.set(http::field::content_type, "text/plain");
              res.body() = "Stream access denied";
            } else if (handleHttpStream(socket, target)) {
              return;
            }
            } else if ([&] { std::string sid, file; return resolveHlsTarget(configManager.config.streams, target, sid, file); }()) {
                if (!isStreamClientAllowed(socket, target)) {
                    res.result(http::status::forbidden);
                    res.set(http::field::content_type, "text/plain");
                    res.body() = "Stream access denied";
                } else if (serveHlsFile(socket, target, res)) {
                    // serveHlsFile filled the response.
                }
            } else if (target == "/" || target == "/index.html") {
                res.body() = renderIndexPage();
            } else if (target == "/oscam-mini") {
                res.set(http::field::content_type, "text/html; charset=UTF-8");
                res.body() = OscamMiniManager::instance().renderPage();
            } else if (target == "/api/oscam-mini/status") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().statusJson();
            } else if (target == "/api/oscam-mini/settings") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().settingsJson();
            } else if (target == "/preview/mpegts.min.js" ||
                       target == "/preview/hls.min.js" ||
                       target == "/licenses/mpegts.js.txt" ||
                       target == "/licenses/hls.js.txt") {
                const std::string_view asset = dvbstreamer5::web::embeddedWebAsset(target);
                if (asset.empty()) {
                    res.result(http::status::not_found);
                    res.set(http::field::content_type, "text/plain; charset=UTF-8");
                    res.body() = "Embedded web asset is unavailable.";
                } else {
                    const bool isJavaScript = target.rfind("/preview/", 0) == 0;
                    res.set(http::field::content_type, isJavaScript
                        ? "application/javascript; charset=UTF-8"
                        : "text/plain; charset=UTF-8");
                    res.set(http::field::cache_control, "private, max-age=3600");
                    res.body().assign(asset.data(), asset.size());
                }
            } else if (target.rfind("/api/streams/", 0) == 0 &&
                       target.size() > std::string("/api/streams//preview").size() &&
                       target.compare(target.size() - std::string("/preview").size(),
                                      std::string("/preview").size(), "/preview") == 0) {
                res.set(http::field::content_type, "application/json; charset=UTF-8");
                res.set(http::field::cache_control, "no-store");
                const std::string encodedId = target.substr(std::string("/api/streams/").size(),
                    target.size() - std::string("/api/streams/").size() - std::string("/preview").size());
                const std::string streamId = urlDecode(encodedId);
                const StreamConfig* cfg = findStreamConfigById(configManager.config.streams, streamId);
                if (!cfg || streamId.empty() || streamId.find('/') != std::string::npos) {
                    res.result(http::status::not_found);
                    res.body() = R"({"error":"stream_not_found"})";
                } else {
                    Json::Value manifest(Json::objectValue);
                    manifest["stream_id"] = cfg->id;
                    manifest["name"] = cfg->name;
                    const bool active = streamManager.isStreamActive(cfg->id);
                    manifest["active"] = active;
                    manifest["input_mode"] = cfg->inputMode;
                    manifest["input_is_hls"] = toLower(cfg->inputMode) == "hls" ||
                        toLower(cfg->inputUri).find(".m3u8") != std::string::npos;
                    Json::Value sources(Json::arrayValue);
                    // A browser preview uses the existing HTTP relay when present,
                    // otherwise an isolated private MPEG-TS relay sharing this
                    // channel's already-running media chain. The configured outputs
                    // are not altered and the private URL requires admin auth.
                    const std::string cleanId = cleanPathToken(cfg->id);
                    const bool safeId = !cleanId.empty() && cleanId == cfg->id;
                    if (safeId && active) {
                        const bool configuredHttp = hasOutputType(*cfg, "http");
                        Json::Value source(Json::objectValue);
                        source["id"] = configuredHttp ? "configured-http" : "temporary-http";
                        source["label"] = configuredHttp ? "HTTP MPEG-TS" : "Временный HTTP MPEG-TS";
                        source["kind"] = "http";
                        source["preview_kind"] = "mpegts";
                        source["preview_url"] = configuredHttp
                            ? "/stream/" + cleanId + ".ts"
                            : "/api/streams/" + cleanId + "/preview.ts";
                        sources.append(std::move(source));
                        // If this channel also publishes HLS, offer the already
                        // running playlist as an alternate browser transport.
                        // No upstream URL, access token or satellite URI leaks.
                        if (hasOutputType(*cfg, "hls")) {
                            Json::Value hls(Json::objectValue);
                            hls["id"] = "configured-hls";
                            hls["label"] = "HLS";
                            hls["kind"] = "hls";
                            hls["preview_kind"] = "hls";
                            hls["preview_url"] = "/" + hlsPublicName(*cfg) + "/video.m3u8";
                            sources.append(std::move(hls));
                        }
                    }
                    manifest["sources"] = std::move(sources);
                    Json::StreamWriterBuilder writer;
                    res.body() = Json::writeString(writer, manifest);
                }
            } else if (target == "/api/interfaces") {
                res.set(http::field::content_type, "application/json");
                res.body() = listInterfaces();
            } else if (target == "/api/system-metrics") {
              res.set(http::field::content_type, "application/json");
              httpMetricsRequestCount.fetch_add(1, std::memory_order_relaxed);
              res.body() = systemMetrics();
            } else if (target == "/api/mpts/status") {
                res.set(http::field::content_type, "application/json");
                Json::StreamWriterBuilder writer;
                res.body() = Json::writeString(writer, streamManager.mptsSnapshot());
            } else if (target == "/api/backup-files") {
                res.set(http::field::content_type, "application/json");
                res.body() = listBackupFiles();
            } else if (target == "/api/state") {
                res.set(http::field::content_type, "application/json");
                // 202.68: server-side A/B probe for the /api/state allocation path.
                // With DVBSTREAMER5_STATE_API_DIAG_BYPASS=1 the remote browser may remain
                // open and keep polling, but currentState() and its large JSON tree
                // are not built. Streaming/media paths are untouched.
                const char* stateBypassEnv = std::getenv("DVBSTREAMER5_STATE_API_DIAG_BYPASS");
                const bool stateBypass =
                    stateBypassEnv && *stateBypassEnv && std::strcmp(stateBypassEnv, "0") != 0;
                std::string stateBody;
                if (stateBypass) {
                    stateBody = std::string(R"({"diagnostic_state_bypass":true,"program_version":")") + kProgramVersion + R"("})";
                } else {
                    stateBody = currentState();
                }
                httpStateRequestCount.fetch_add(1, std::memory_order_relaxed);
                httpStateResponseBytes.fetch_add(
                    static_cast<uint64_t>(stateBody.size()), std::memory_order_relaxed);
                httpStateLastResponseBytes.store(
                    static_cast<uint64_t>(stateBody.size()), std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> diagLock(httpDiagMutex);
                    httpStateWorkerThreads.insert(std::this_thread::get_id());
                }
                res.body() = std::move(stateBody);
            } else if (target == "/api/dvb-adapters") {
                res.set(http::field::content_type, "application/json");
                res.body() = dvbAdapters();
            } else if (target == "/api/ca-manager") {
                res.set(http::field::content_type, "application/json");
                res.body() = caManagerStatus();
            } else if (target.rfind("/api/quality-history", 0) == 0) {
                res.set(http::field::content_type, "application/json");
                res.body() = qualityHistory(target);
            } else if (target == "/health") {
                res.set(http::field::content_type, "text/plain");
                res.body() = "Healthy";
            } else {
                res.result(http::status::not_found);
                res.body() = "Not Found";
            }
          } // end normal GET routing; private preview is handled above
        } else if (req.method() == http::verb::post) {
            std::string closingPreviewId;
            if (resolvePrivatePreviewCloseTarget(target, closingPreviewId)) {
                res.set(http::field::content_type, "application/json");
                Json::Value payload;
                Json::CharReaderBuilder builder;
                std::string error;
                std::istringstream input(req.body());
                if (req.body().size() > 256 ||
                    !Json::parseFromStream(builder, input, &payload, &error) ||
                    !payload.isObject() || !payload["session"].isString() ||
                    !validPreviewSessionToken(payload["session"].asString()) ||
                    !findStreamConfigById(configManager.config.streams, closingPreviewId)) {
                    res.result(http::status::bad_request);
                    res.body() = R"({"error":"Invalid preview close request"})";
                } else {
                    const bool closed = streamManager.closePreviewSession(
                        closingPreviewId, payload["session"].asString());
                    res.body() = closed ? R"({"closed":true})" : R"({"closed":false})";
                }
            } else if (target == "/api/network/udp-input-filter") {
                res.set(http::field::content_type, "application/json");
                Json::Value payload;
                Json::CharReaderBuilder builder;
                std::string parseError;
                std::istringstream input(req.body());
                Json::Value reply(Json::objectValue);
                if (req.body().size() > 4096 ||
                    !Json::parseFromStream(builder, input, &payload, &parseError) ||
                    !payload.isObject() || !payload["address"].isString() ||
                    !payload["enabled"].isBool()) {
                    res.result(http::status::bad_request);
                    reply["result"] = "error";
                    reply["error"] = "Invalid UDP input filter request";
                } else {
                    const std::string address = payload["address"].asString();
                    const bool enabled = payload["enabled"].asBool();
                    const auto interfaces = enumerateNetworkInterfaces();
                    const auto iface = std::find_if(interfaces.begin(), interfaces.end(),
                        [&address](const NetworkInterface& candidate) {
                            return candidate.address == address && candidate.name != "lo" &&
                                   candidate.address.rfind("127.", 0) != 0;
                        });
                    if (iface == interfaces.end()) {
                        res.result(http::status::bad_request);
                        reply["result"] = "error";
                        reply["error"] = "Network interface is unavailable";
                    } else {
                        const auto previous = configManager.config.udpInputFilterInterfaces;
                        auto next = previous;
                        next.erase(std::remove(next.begin(), next.end(), address), next.end());
                        if (enabled) next.push_back(address);
                        std::sort(next.begin(), next.end());
                        next.erase(std::unique(next.begin(), next.end()), next.end());
                        configManager.config.udpInputFilterInterfaces = next;

                        std::string firewallError;
                        if (!dvbstreamer5::network::UdpInputFirewall::apply(
                                configManager.config, firewallError)) {
                            configManager.config.udpInputFilterInterfaces = previous;
                            std::string rollbackError;
                            (void)dvbstreamer5::network::UdpInputFirewall::apply(
                                configManager.config, rollbackError);
                            res.result(http::status::bad_request);
                            reply["result"] = "error";
                            reply["error"] = firewallError;
                        } else if (!configManager.save()) {
                            configManager.config.udpInputFilterInterfaces = previous;
                            std::string rollbackError;
                            (void)dvbstreamer5::network::UdpInputFirewall::apply(
                                configManager.config, rollbackError);
                            res.result(http::status::internal_server_error);
                            reply["result"] = "error";
                            reply["error"] = "Unable to save configuration";
                        } else {
                            reply["result"] = "ok";
                            reply["address"] = address;
                            reply["enabled"] = enabled;
                        }
                    }
                }
                Json::StreamWriterBuilder writer;
                writer["indentation"] = "";
                res.body() = Json::writeString(writer, reply);
            } else if (target == "/api/oscam-mini/save") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().saveSettingsJson(req.body());
            } else if (target == "/api/oscam-mini/action") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().serviceActionJson(req.body());
            } else if (target == "/api/config/export") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleConfigExport();
            } else if (target == "/api/config/import") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleConfigImport(req.body());
            } else if (target == "/api/save-config") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleSaveConfig(req.body());
            } else if (target == "/api/mpts/save") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleMptsSave(req.body());
            } else if (target == "/api/mpts/action") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleMptsAction(req.body());
            } else if (target == "/api/start-stream") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleStartStream(req.body());
            } else if (target == "/api/stop-stream") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleStopStream(req.body());
            } else if (target == "/api/restart-program") {
                handleRestartProgram();
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"restarting\"}";
            } else if (target == "/api/delete-stream") {
              handleDeleteStream(req.body());
              res.set(http::field::content_type, "application/json");
              res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/save-subscribers") {
                handleSaveSubscribers(req.body());
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"ok\"}";
            } else if (target.rfind("/api/upload-backup-file", 0) == 0) {
                res.set(http::field::content_type, "application/json");
                res.body() = handleUploadBackupFile(target, req.body());
            } else if (target == "/api/delete-backup-file") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDeleteBackupFile(req.body());
            } else if (target == "/api/reset-subscriber") {
              handleResetSubscriber(req.body());
              res.set(http::field::content_type, "application/json");
              res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/block-stream-client") {
              handleBlockStreamClient(req.body(), true);
              res.set(http::field::content_type, "application/json");
              res.body() = R"json({"result":"ok"})json";
            } else if (target == "/api/unblock-stream-client") {
              handleBlockStreamClient(req.body(), false);
              res.set(http::field::content_type, "application/json");
              res.body() = R"json({"result":"ok"})json";
            } else if (target == "/api/cam-client-settings") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleCamClientSettings(req.body());
            } else if (target == "/api/dvb-signal") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDvbTune(req.body(), false);
            } else if (target == "/api/dvb-scan") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDvbTune(req.body(), true);
            } else if (target == "/api/dvb-add-channels") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDvbAddChannels(req.body());
            } else {
                res.result(http::status::not_found);
                res.body() = "Not Found";
            }
        } else {
            res.result(http::status::method_not_allowed);
            res.body() = "Method Not Allowed";
        }

        res.content_length(res.body().size());
        http::write(socket, res);
    } catch (const std::exception& ex) {
        const std::string message = ex.what();
        if (message.find("bad method") != std::string::npos) {
            std::cerr << "HTTP non-HTTP request ignored on web port: " << message << std::endl;
        } else {
            std::cerr << "HTTP session failed: " << message << std::endl;
        }
    }
}

bool HttpServer::requiresAuthentication(const std::string& target) const {
    // Control-panel preview endpoints must never become public via a channel
    // whose HLS public name collides with the first URL segment ('api').
    if (target.rfind("/api/streams/", 0) == 0) return true;
    std::string hlsId, hlsFile;
    const bool hlsTarget = resolveHlsTarget(configManager.config.streams, target, hlsId, hlsFile);
    std::string httpMpegTsId;
    const bool httpMpegTsTarget = resolveHttpMpegTsTarget(configManager.config.streams, target, httpMpegTsId);
    return target != "/health" &&
           !httpMpegTsTarget &&
           !hlsTarget;
}

bool HttpServer::isAuthorized(const http::request<http::string_body>& req) const {
    const auto auth = req.find(http::field::authorization);
    if (auth == req.end()) {
        return false;
    }

    const std::string header(auth->value());
    const std::string prefix = "Basic ";
    if (header.size() <= prefix.size() || !boost::algorithm::istarts_with(header, prefix)) {
        return false;
    }

    const std::string decoded = base64Decode(header.substr(prefix.size()));
    const auto separator = decoded.find(':');
    if (separator == std::string::npos) {
        return false;
    }

    const std::string login = decoded.substr(0, separator);
    const std::string password = decoded.substr(separator + 1);
    return constantTimeEquals(login, configManager.config.login) &&
           constantTimeEquals(password, configManager.config.password);
}

bool HttpServer::isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const {
  const std::string normalizedClientIp = normalizeIpAddress(clientIp);
  if (streamId.empty() || normalizedClientIp.empty()) {
    return false;
  }
  if (std::find(configManager.subscribers.blockedIps.begin(), configManager.subscribers.blockedIps.end(), normalizedClientIp) !=
      configManager.subscribers.blockedIps.end()) {
    return false;
  }
  if (!configManager.subscribers.filteringEnabled) {
    return true;
  }
  for (const auto& subscriber : configManager.subscribers.subscribers) {
    const std::string primaryIp = normalizeIpAddress(subscriber.primaryIp);
    const std::string backupIp = normalizeIpAddress(subscriber.backupIp);
    const bool ipMatches = subscriber.enabled &&
      (normalizedClientIp == primaryIp || (!backupIp.empty() && normalizedClientIp == backupIp));
    const bool streamMatches = std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), streamId) != subscriber.streamIds.end();
    if (ipMatches && streamMatches) {
      return true;
    }
  }
  return false;
}

bool HttpServer::isStreamClientAllowed(const tcp::socket& socket, const std::string& target) const {
  boost::system::error_code ec;
  const std::string clientIp = socket.remote_endpoint(ec).address().to_string();
  if (ec) {
    return false;
  }
  std::string streamId;
  resolveHttpMpegTsTarget(configManager.config.streams, target, streamId);
  if (streamId.empty()) {
    std::string fileName;
    resolveHlsTarget(configManager.config.streams, target, streamId, fileName);
  }
  return isClientAllowedForStream(streamId, clientIp);
}

void HttpServer::writeUnauthorized(http::response<http::string_body>& res) const {
    res.result(http::status::unauthorized);
    res.set(http::field::www_authenticate, "Basic realm=\"DVBStreamer5\"");
    res.set(http::field::content_type, "text/plain; charset=UTF-8");
    res.body() = "Unauthorized";
}

std::set<int> HttpServer::configuredHttpPorts() const {
    std::set<int> ports;
    if (configManager.config.httpPort > 0 && configManager.config.httpPort <= 65535) {
        ports.insert(configManager.config.httpPort);
    }
    const int defaultPort = configManager.config.httpPort;
    for (const auto& stream : configManager.config.streams) {
        for (const auto& output : streamOutputs(stream)) {
            const std::string type = normalizedOutputType(output);
            if (type == "http" || type == "hls") {
                const int port = streamHttpPort(output, defaultPort);
                if (port > 0 && port <= 65535) ports.insert(port);
            }
        }
    }
    if (ports.empty()) {
        ports.insert(9000);
    }
    return ports;
}

bool HttpServer::validateHttpPortsForConfig(const AppConfig& config, std::string& error) const {
    error.clear();
    struct Owner { std::string label; std::string protocol; };
    std::map<int, Owner> requested;
    if (config.httpPort > 0 && config.httpPort <= 65535) {
        requested.emplace(config.httpPort, Owner{"Control Panel", "HTTP"});
    }
    for (const auto& stream : config.streams) {
        for (const auto& output : streamOutputs(stream)) {
            const std::string type = normalizedOutputType(output);
            if (type != "http" && type != "hls") continue;
            const int port = streamHttpPort(output, config.httpPort);
            if (port <= 0 || port > 65535) continue;
            const std::string label = stream.name.empty() ? stream.id : stream.name;
            requested[port] = Owner{label, type == "hls" ? "HLS" : "HTTP"};
        }
    }
    for (const auto& [port, owner] : requested) {
        {
            std::lock_guard<std::mutex> lock(acceptorsMutex);
            const auto current = acceptors.find(port);
            if (current != acceptors.end() && current->second && current->second->is_open()) continue;
        }
        boost::system::error_code ec;
        tcp::acceptor probe(ioContext);
        tcp::endpoint endpoint(tcp::v4(), static_cast<unsigned short>(port));
        probe.open(endpoint.protocol(), ec);
        if (!ec) probe.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (!ec) probe.bind(endpoint, ec);
        if (probe.is_open()) {
            boost::system::error_code closeEc;
            probe.close(closeEc);
        }
        if (ec) {
            error = owner.protocol + " port " + std::to_string(port) +
                " is unavailable for " + owner.label + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool HttpServer::bindHttpPorts(const std::set<int>& ports) {
    std::lock_guard<std::mutex> acceptorLock(acceptorsMutex);
    boost::system::error_code ec;
    const uint64_t generation = acceptGeneration.fetch_add(1) + 1;

    for (auto& [port, listener] : acceptors) {
        (void)port;
        if (listener && listener->is_open()) {
            listener->cancel(ec);
            listener->close(ec);
        }
    }

    std::unordered_map<int, std::shared_ptr<tcp::acceptor>> nextAcceptors;
    for (int port : ports) {
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid HTTP port: " << port << std::endl;
            continue;
        }

        auto listener = std::make_shared<tcp::acceptor>(ioContext);
        tcp::endpoint endpoint(tcp::v4(), static_cast<unsigned short>(port));
        listener->open(endpoint.protocol(), ec);
        if (ec) {
            std::cerr << "HTTP server failed to open port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) {
            std::cerr << "HTTP server failed to set reuse_address on port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->bind(endpoint, ec);
        if (ec) {
            std::cerr << "HTTP server failed to bind port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cerr << "HTTP server failed to listen on port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }

        doAccept(listener, port, generation);
        nextAcceptors[port] = std::move(listener);
        std::cerr << "HTTP server listening on port " << port << std::endl;
    }

    const bool primaryBound = nextAcceptors.count(configManager.config.httpPort) > 0;
    acceptors = std::move(nextAcceptors);
    if (!primaryBound) {
        std::cerr << "HTTP server primary port is not listening: " << configManager.config.httpPort << std::endl;
    }
    return primaryBound;
}

void HttpServer::refreshHttpPorts() {
    // V10.8.100: config saves are allowed to be partial and the web UI may
    // issue several of them in quick succession. Rebinding an unchanged
    // listener set closes healthy acceptors and can race the next save into
    // EADDRINUSE. Keep the existing listeners when every requested port is
    // already open; bindHttpPorts() remains the recovery/reconfiguration path
    // when the set really changed or a listener disappeared.
    const auto desiredPorts = configuredHttpPorts();
    boost::asio::post(ioContext, [this, desiredPorts]() {
        {
            std::lock_guard<std::mutex> acceptorLock(acceptorsMutex);
            bool unchanged = acceptors.size() == desiredPorts.size();
            if (unchanged) {
                for (int port : desiredPorts) {
                    const auto found = acceptors.find(port);
                    if (found == acceptors.end() || !found->second || !found->second->is_open()) {
                        unchanged = false;
                        break;
                    }
                }
            }
            if (unchanged) return;
        }
        bindHttpPorts(desiredPorts);
    });
}

