from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}\n--- OLD ---\n{old}")
    p.write_text(text.replace(old, new, 1))

replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.114";',
    'inline constexpr const char* kProgramVersion = "10.8.115";',
)

replace_once(
    "src/HttpServer.h",
    '    void recordQualitySample(const StreamConfig& cfg, const Json::Value& state);\n',
    '    void recordQualitySample(const StreamConfig& cfg, const Json::Value& state);\n'
    '    void recordAllQualitySamples();\n'
    '    void scheduleQualitySampler();\n'
    '    void loadQualityHistoryFromDisk();\n'
    '    void saveQualityHistoryToDisk();\n',
)

replace_once(
    "src/HttpServer.h",
    '    boost::asio::io_context& ioContext;\n    boost::asio::thread_pool sessionPool;\n',
    '    boost::asio::io_context& ioContext;\n'
    '    boost::asio::thread_pool sessionPool;\n'
    '    boost::asio::steady_timer qualityHistoryTimer;\n',
)

replace_once(
    "src/HttpServer.h",
    '    std::unordered_map<std::string, std::deque<QualitySample>> qualitySamples;\n'
    '    std::unordered_map<std::string, int64_t> qualityLastCompaction;\n',
    '    std::unordered_map<std::string, std::deque<QualitySample>> qualitySamples;\n'
    '    std::unordered_map<std::string, int64_t> qualityLastCompaction;\n'
    '    bool qualitySamplerStarted = false;\n'
    '    int64_t qualityLastPersist = 0;\n',
)

replace_once(
    "src/HttpServer.cpp",
    'constexpr int64_t kQualityCompactionIntervalSeconds = 60LL;\n'
    'constexpr std::size_t kQualityMaxSamplesPerStream = 10000;\n',
    'constexpr int64_t kQualityCompactionIntervalSeconds = 60LL;\n'
    'constexpr int64_t kQualityPersistIntervalSeconds = 5LL * 60LL;\n'
    'constexpr std::size_t kQualityMaxSamplesPerStream = 10000;\n'
    'constexpr const char* kQualityHistoryPath = "/var/lib/dvbstreamer5/quality-history.json";\n',
)

replace_once(
    "src/HttpServer.cpp",
    'HttpServer::HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm)\n'
    '    : ioContext(ioc), sessionPool(kHttpSessionWorkers),\n'
    '      configManager(cfg), streamManager(sm) {',
    'HttpServer::HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm)\n'
    '    : ioContext(ioc), sessionPool(kHttpSessionWorkers), qualityHistoryTimer(ioc),\n'
    '      configManager(cfg), streamManager(sm) {',
)

replace_once(
    "src/HttpServer.cpp",
    'bool HttpServer::start() {\n'
    '    return bindHttpPorts(configuredHttpPorts());\n'
    '}\n',
    'bool HttpServer::start() {\n'
    '    if (!bindHttpPorts(configuredHttpPorts())) return false;\n'
    '\n'
    '    // V10.8.115: quality history belongs to the server, not to the browser.\n'
    '    // Restore persisted samples, record immediately, then continue every\n'
    '    // 30 seconds even when no Web UI client is connected.\n'
    '    if (!qualitySamplerStarted) {\n'
    '        loadQualityHistoryFromDisk();\n'
    '        recordAllQualitySamples();\n'
    '        qualityLastPersist = 0;\n'
    '        qualitySamplerStarted = true;\n'
    '        scheduleQualitySampler();\n'
    '    }\n'
    '    return true;\n'
    '}\n',
)

anchor = 'std::string HttpServer::qualityHistory(const std::string& target) {'
insert = r'''void HttpServer::recordAllQualitySamples() {
    const auto snap = streamManager.snapshot();
    const auto configs = configManager.config.streams;

    for (const auto& cfg : configs) {
        Json::Value state;
        const auto it = snap.find(cfg.id);
        if (it != snap.end() && it->second) {
            auto* streamState = it->second;
            state["active"] = streamState->active.load();
            state["status"] = streamState->statusMessage;
            state["bitrate_in_kbps"] = Json::UInt64(streamState->inputBitrate.load() / 1000);
            state["bitrate_out_kbps"] = Json::UInt64(streamState->outputBitrate.load() / 1000);
            state["payload_out_kbps"] = Json::UInt64(streamState->outputPayloadBitrate.load() / 1000);
            state["input_cc_errors"] = Json::UInt64(streamState->inputCcErrorsDelta.load());
            state["output_cc_errors"] = Json::UInt64(streamState->outputCcErrorsDelta.load());
            state["input_cc_errors_total"] = Json::UInt64(streamState->inputCcErrors.load());
            state["output_cc_errors_total"] = Json::UInt64(streamState->outputCcErrors.load());
        } else {
            state["active"] = false;
            state["status"] = "stopped";
            state["bitrate_in_kbps"] = Json::UInt64(0);
            state["bitrate_out_kbps"] = Json::UInt64(0);
            state["payload_out_kbps"] = Json::UInt64(0);
            state["input_cc_errors"] = Json::UInt64(0);
            state["output_cc_errors"] = Json::UInt64(0);
            state["input_cc_errors_total"] = Json::UInt64(0);
            state["output_cc_errors_total"] = Json::UInt64(0);
        }
        recordQualitySample(cfg, state);
    }
}

void HttpServer::scheduleQualitySampler() {
    qualityHistoryTimer.expires_after(std::chrono::seconds(kQualityRecordIntervalSeconds));
    qualityHistoryTimer.async_wait([this](const boost::system::error_code& ec) {
        if (ec) return;

        recordAllQualitySamples();
        const int64_t now = unixNowSeconds();
        if (qualityLastPersist == 0 || now - qualityLastPersist >= kQualityPersistIntervalSeconds) {
            saveQualityHistoryToDisk();
            qualityLastPersist = now;
        }
        scheduleQualitySampler();
    });
}

void HttpServer::loadQualityHistoryFromDisk() {
    std::ifstream input(kQualityHistoryPath, std::ios::binary);
    if (!input.is_open()) return;

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errors;
    if (!Json::parseFromStream(reader, input, &root, &errors) ||
        root.get("format", "").asString() != "dvbstreamer5-quality-history" ||
        root.get("version", 0).asInt() != 1) {
        std::cerr << "QUALITY HISTORY load ignored invalid file: " << errors << std::endl;
        return;
    }

    std::set<std::string> configuredIds;
    for (const auto& cfg : configManager.config.streams) configuredIds.insert(cfg.id);
    const int64_t now = unixNowSeconds();
    const int64_t cutoff = now - kQualityRetentionSeconds;

    std::lock_guard<std::mutex> lock(qualityMutex);
    qualitySamples.clear();
    qualityLastCompaction.clear();

    const Json::Value& streams = root["streams"];
    if (!streams.isObject()) return;
    for (const auto& id : streams.getMemberNames()) {
        if (!configuredIds.count(id)) continue;
        const Json::Value& rows = streams[id];
        if (!rows.isArray()) continue;

        auto& dst = qualitySamples[id];
        for (const auto& row : rows) {
            QualitySample sample;
            sample.timestamp = row.get("ts", Json::Int64(0)).asInt64();
            if (sample.timestamp < cutoff || sample.timestamp > now + 300) continue;
            sample.active = row.get("active", false).asBool();
            sample.inputKbps = row.get("input_kbps", Json::UInt64(0)).asUInt64();
            sample.outputKbps = row.get("output_kbps", Json::UInt64(0)).asUInt64();
            sample.targetKbps = row.get("target_kbps", Json::UInt64(0)).asUInt64();
            sample.inputCcErrors = row.get("input_cc_errors", Json::UInt64(0)).asUInt64();
            sample.outputCcErrors = row.get("output_cc_errors", Json::UInt64(0)).asUInt64();
            sample.inputCcErrorsTotal = row.get("input_cc_errors_total", Json::UInt64(0)).asUInt64();
            sample.outputCcErrorsTotal = row.get("output_cc_errors_total", Json::UInt64(0)).asUInt64();
            sample.status = row.get("status", "").asString();
            sample.level = row.get("level", "offline").asString();
            sample.message = row.get("message", "").asString();
            dst.push_back(std::move(sample));
        }
        std::sort(dst.begin(), dst.end(), [](const QualitySample& a, const QualitySample& b) {
            return a.timestamp < b.timestamp;
        });
        while (dst.size() > kQualityMaxSamplesPerStream) dst.pop_front();
        if (dst.empty()) qualitySamples.erase(id);
        else qualityLastCompaction[id] = now;
    }

    std::size_t restored = 0;
    for (const auto& [id, rows] : qualitySamples) {
        (void)id;
        restored += rows.size();
    }
    std::cerr << "QUALITY HISTORY restored samples=" << restored
              << " path=" << kQualityHistoryPath << std::endl;
}

void HttpServer::saveQualityHistoryToDisk() {
    Json::Value root;
    root["format"] = "dvbstreamer5-quality-history";
    root["version"] = 1;
    root["saved_at"] = Json::Int64(unixNowSeconds());
    Json::Value streams(Json::objectValue);

    {
        std::lock_guard<std::mutex> lock(qualityMutex);
        for (const auto& [id, rows] : qualitySamples) {
            Json::Value array(Json::arrayValue);
            for (const auto& sample : rows) {
                Json::Value row;
                row["ts"] = Json::Int64(sample.timestamp);
                row["active"] = sample.active;
                row["input_kbps"] = Json::UInt64(sample.inputKbps);
                row["output_kbps"] = Json::UInt64(sample.outputKbps);
                row["target_kbps"] = Json::UInt64(sample.targetKbps);
                row["input_cc_errors"] = Json::UInt64(sample.inputCcErrors);
                row["output_cc_errors"] = Json::UInt64(sample.outputCcErrors);
                row["input_cc_errors_total"] = Json::UInt64(sample.inputCcErrorsTotal);
                row["output_cc_errors_total"] = Json::UInt64(sample.outputCcErrorsTotal);
                row["status"] = sample.status;
                row["level"] = sample.level;
                row["message"] = sample.message;
                array.append(std::move(row));
            }
            streams[id] = std::move(array);
        }
    }
    root["streams"] = std::move(streams);

    const std::filesystem::path path(kQualityHistoryPath);
    const std::filesystem::path temp = path.string() + ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        std::cerr << "QUALITY HISTORY persist mkdir failed: " << ec.message() << std::endl;
        return;
    }

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            std::cerr << "QUALITY HISTORY persist open failed: " << temp << std::endl;
            return;
        }
        output << Json::writeString(writer, root);
        output.flush();
        if (!output.good()) {
            std::cerr << "QUALITY HISTORY persist write failed: " << temp << std::endl;
            output.close();
            std::filesystem::remove(temp, ec);
            return;
        }
    }

    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::cerr << "QUALITY HISTORY persist rename failed: " << ec.message() << std::endl;
        std::filesystem::remove(temp, ec);
    }
}

'''
replace_once("src/HttpServer.cpp", anchor, insert + anchor)

print("V10.8.115 persistent quality history patch applied")
