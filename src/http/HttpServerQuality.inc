namespace {

constexpr const char* kQualityHistoryV2Dir = "/var/lib/dvbstreamer5/quality-history-v2";
constexpr std::size_t kQualityMemorySamplesPerStream = 240;
constexpr std::uintmax_t kQualityShardPruneBytes = 4ULL * 1024ULL * 1024ULL;

std::mutex& qualityDiskMutex() {
    static std::mutex mutex;
    return mutex;
}

std::filesystem::path qualityShardPath(const std::string& id) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(id.size() * 3U + 6U);
    for (unsigned char ch : id) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('_');
            encoded.push_back(hex[(ch >> 4U) & 0x0fU]);
            encoded.push_back(hex[ch & 0x0fU]);
        }
    }
    if (encoded.empty()) encoded = "stream";
    return std::filesystem::path(kQualityHistoryV2Dir) / (encoded + ".jsonl");
}

std::pair<std::string, std::string> classifyQualitySample(
    bool active,
    const std::string& status,
    uint64_t inputKbps,
    uint64_t outputKbps,
    uint64_t targetKbps,
    uint64_t inputCc,
    uint64_t outputCc) {
    const std::string statusLower = toLower(status);
    if (!active) {
        return {"offline", status == "stopped" ? "Поток остановлен" : "Поток не активен: " + status};
    }
    if (statusLower.find("error") != std::string::npos ||
        statusLower.find("failed") != std::string::npos ||
        statusLower.find("ended") != std::string::npos) {
        return {"error", "Ошибка media engine: " + status};
    }
    if (inputKbps == 0) {
        return {"warn", "Нет входного битрейта при активном потоке"};
    }
    if (inputCc > 0 || outputCc > 0) {
        return {"error", "CC-errors MPEG-TS: вход=" + std::to_string(inputCc) +
            ", выход=" + std::to_string(outputCc)};
    }
    if (targetKbps > 0 && outputKbps > 0) {
        const double diff = std::abs(static_cast<double>(outputKbps) -
                                     static_cast<double>(targetKbps));
        const double deviation = diff / static_cast<double>(targetKbps);
        if (deviation > 0.20) {
            return {"warn", "Выходной битрейт отклоняется от цели больше чем на 20%"};
        }
    }
    return {"ok", "Качество в норме"};
}

void pruneQualityShardIfNeeded(const std::filesystem::path& path, int64_t cutoff) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size <= kQualityShardPruneBytes) return;

    const auto temp = path.string() + ".tmp";
    std::ifstream input(path, std::ios::binary);
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!input.is_open() || !output.is_open()) return;

    std::string line;
    while (std::getline(input, line)) {
        if (line.size() < 4 || line.front() != '[') continue;
        const auto comma = line.find(',');
        if (comma == std::string::npos || comma <= 1) continue;
        try {
            const int64_t timestamp = std::stoll(line.substr(1, comma - 1));
            if (timestamp >= cutoff) output << line << '\n';
        } catch (...) {
        }
    }
    output.flush();
    if (!output.good()) {
        output.close();
        std::filesystem::remove(temp, ec);
        return;
    }
    output.close();
    input.close();
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temp, path, ec);
    }
    if (ec) std::filesystem::remove(temp, ec);
}

} // namespace

void HttpServer::recordAllQualitySamples() {
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
    // V10.8.178: never deserialize the legacy all-stream JSON during startup.
    // A 91-MB/292k-sample file was measured to delay HTTP readiness by ~2.6 s
    // and inflate startup RSS from ~5 MB to hundreds of MB.  V2 history is
    // sharded by stream and read lazily only when that stream's graph is opened.
    std::error_code ec;
    std::filesystem::create_directories(kQualityHistoryV2Dir, ec);
    if (ec) {
        std::cerr << "QUALITY HISTORY v2 mkdir failed: " << ec.message() << std::endl;
        return;
    }

    const std::filesystem::path legacy(kQualityHistoryPath);
    if (std::filesystem::exists(legacy, ec) && !ec) {
        std::cerr << "QUALITY HISTORY v2 lazy mode: legacy file kept but not loaded path="
                  << legacy.string() << std::endl;
    } else {
        std::cerr << "QUALITY HISTORY v2 lazy mode: startup_load=off dir="
                  << kQualityHistoryV2Dir << std::endl;
    }
}

void HttpServer::saveQualityHistoryToDisk() {
    // Persist one compact 5-minute point per stream.  Recent 30-second points
    // stay only in the small in-memory ring; older history lives in per-stream
    // shards so a process restart never has to parse every channel's history.
    std::vector<std::pair<std::string, QualitySample>> snapshots;
    const int64_t now = unixNowSeconds();
    const int64_t aggregateCutoff = now - kQualityPersistIntervalSeconds;

    {
        std::lock_guard<std::mutex> lock(qualityMutex);
        snapshots.reserve(qualitySamples.size());
        for (const auto& [id, rows] : qualitySamples) {
            if (rows.empty()) continue;
            QualitySample persisted = rows.back();
            uint64_t inputCc = 0;
            uint64_t outputCc = 0;
            for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                if (it->timestamp < aggregateCutoff) break;
                inputCc += it->inputCcErrors;
                outputCc += it->outputCcErrors;
            }
            persisted.inputCcErrors = inputCc;
            persisted.outputCcErrors = outputCc;
            if (inputCc > 0 || outputCc > 0) {
                persisted.level = "error";
                persisted.message = "CC-errors MPEG-TS за интервал: вход=" +
                    std::to_string(inputCc) + ", выход=" + std::to_string(outputCc);
            }
            snapshots.emplace_back(id, std::move(persisted));
        }
    }

    if (snapshots.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(kQualityHistoryV2Dir, ec);
    if (ec) {
        std::cerr << "QUALITY HISTORY v2 persist mkdir failed: " << ec.message() << std::endl;
        return;
    }

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const int64_t cutoff = now - kQualityRetentionSeconds;

    std::lock_guard<std::mutex> diskLock(qualityDiskMutex());
    for (const auto& [id, sample] : snapshots) {
        Json::Value row(Json::arrayValue);
        row.append(Json::Int64(sample.timestamp));
        row.append(sample.active);
        row.append(Json::UInt64(sample.inputKbps));
        row.append(Json::UInt64(sample.outputKbps));
        row.append(Json::UInt64(sample.targetKbps));
        row.append(Json::UInt64(sample.inputCcErrors));
        row.append(Json::UInt64(sample.outputCcErrors));
        row.append(Json::UInt64(sample.inputCcErrorsTotal));
        row.append(Json::UInt64(sample.outputCcErrorsTotal));
        row.append(sample.status);
        row.append(sample.level);
        row.append(sample.message);

        const auto path = qualityShardPath(id);
        std::ofstream output(path, std::ios::binary | std::ios::app);
        if (!output.is_open()) {
            std::cerr << "QUALITY HISTORY v2 persist open failed: " << path << std::endl;
            continue;
        }
        output << Json::writeString(writer, row) << '\n';
        output.flush();
        if (!output.good()) {
            std::cerr << "QUALITY HISTORY v2 persist write failed: " << path << std::endl;
            continue;
        }
        output.close();
        pruneQualityShardIfNeeded(path, cutoff);
    }
}

std::string HttpServer::qualityHistory(const std::string& target) {
    const std::string id = queryValue(target, "id");
    uint64_t periodSeconds = 3600;
    try {
        const std::string period = queryValue(target, "period");
        if (!period.empty()) periodSeconds = std::stoull(period);
    } catch (const std::exception&) {
        periodSeconds = 3600;
    }
    periodSeconds = std::clamp<uint64_t>(periodSeconds, 60, 30ULL * 24ULL * 60ULL * 60ULL);

    Json::Value liveState;
    const StreamConfig* liveConfig = nullptr;
    for (const auto& cfg : configManager.config.streams) {
        if (cfg.id == id) {
            liveConfig = &cfg;
            break;
        }
    }
    if (liveConfig) {
        const auto snap = streamManager.snapshot();
        const auto it = snap.find(id);
        if (it != snap.end() && it->second) {
            auto* streamState = it->second;
            liveState["active"] = streamState->active.load();
            liveState["status"] = streamState->statusMessage;
            liveState["bitrate_in_kbps"] = Json::UInt64(streamState->inputBitrate.load() / 1000);
            liveState["bitrate_out_kbps"] = Json::UInt64(streamState->outputBitrate.load() / 1000);
            liveState["payload_out_kbps"] = Json::UInt64(streamState->outputPayloadBitrate.load() / 1000);
            liveState["input_cc_errors"] = Json::UInt64(streamState->inputCcErrorsDelta.load());
            liveState["output_cc_errors"] = Json::UInt64(streamState->outputCcErrorsDelta.load());
            liveState["input_cc_errors_total"] = Json::UInt64(streamState->inputCcErrors.load());
            liveState["output_cc_errors_total"] = Json::UInt64(streamState->outputCcErrors.load());
        } else {
            liveState["active"] = false;
            liveState["status"] = "stopped";
            liveState["bitrate_in_kbps"] = Json::UInt64(0);
            liveState["bitrate_out_kbps"] = Json::UInt64(0);
            liveState["input_cc_errors"] = Json::UInt64(0);
            liveState["output_cc_errors"] = Json::UInt64(0);
            liveState["input_cc_errors_total"] = Json::UInt64(0);
            liveState["output_cc_errors_total"] = Json::UInt64(0);
        }
        recordQualitySample(*liveConfig, liveState);
    }

    const int64_t generatedAt = unixNowSeconds();
    const int64_t cutoff = generatedAt - static_cast<int64_t>(periodSeconds);
    std::map<int64_t, QualitySample> merged;

    // Read only the requested stream's shard. A 30-day graph now touches at
    // most one small file instead of materializing every stream at process boot.
    {
        std::lock_guard<std::mutex> diskLock(qualityDiskMutex());
        std::ifstream input(qualityShardPath(id), std::ios::binary);
        if (input.is_open()) {
            Json::CharReaderBuilder readerBuilder;
            std::string line;
            while (std::getline(input, line)) {
                if (line.empty()) continue;
                Json::Value row;
                std::string errors;
                std::unique_ptr<Json::CharReader> reader(readerBuilder.newCharReader());
                if (!reader->parse(line.data(), line.data() + line.size(), &row, &errors) ||
                    !row.isArray() || row.size() < 12) {
                    continue;
                }
                QualitySample sample;
                sample.timestamp = row[0].asInt64();
                if (sample.timestamp < cutoff || sample.timestamp > generatedAt + 300) continue;
                sample.active = row[1].asBool();
                sample.inputKbps = row[2].asUInt64();
                sample.outputKbps = row[3].asUInt64();
                sample.targetKbps = row[4].asUInt64();
                sample.inputCcErrors = row[5].asUInt64();
                sample.outputCcErrors = row[6].asUInt64();
                sample.inputCcErrorsTotal = row[7].asUInt64();
                sample.outputCcErrorsTotal = row[8].asUInt64();
                sample.status = row[9].asString();
                sample.level = row[10].asString();
                sample.message = row[11].asString();
                merged[sample.timestamp] = std::move(sample);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(qualityMutex);
        const auto found = qualitySamples.find(id);
        if (found != qualitySamples.end()) {
            for (const auto& sample : found->second) {
                if (sample.timestamp >= cutoff) merged[sample.timestamp] = sample;
            }
        }
    }

    Json::Value root;
    root["id"] = id;
    root["period_seconds"] = Json::UInt64(periodSeconds);
    root["generated_at"] = Json::Int64(generatedAt);
    Json::Value samples(Json::arrayValue);
    std::map<std::string, unsigned int> totals = {
        {"ok", 0}, {"warn", 0}, {"error", 0}, {"offline", 0}
    };

    QualitySample newestStored;
    bool haveNewestStored = false;
    for (const auto& [timestamp, sample] : merged) {
        (void)timestamp;
        Json::Value item;
        item["ts"] = Json::Int64(sample.timestamp);
        item["active"] = sample.active;
        item["input_kbps"] = Json::UInt64(sample.inputKbps);
        item["output_kbps"] = Json::UInt64(sample.outputKbps);
        item["target_kbps"] = Json::UInt64(sample.targetKbps);
        item["input_cc_errors"] = Json::UInt64(sample.inputCcErrors);
        item["output_cc_errors"] = Json::UInt64(sample.outputCcErrors);
        item["input_cc_errors_total"] = Json::UInt64(sample.inputCcErrorsTotal);
        item["output_cc_errors_total"] = Json::UInt64(sample.outputCcErrorsTotal);
        item["cc_errors"] = Json::UInt64(sample.inputCcErrors);
        item["status"] = sample.status;
        item["level"] = sample.level;
        item["message"] = sample.message;
        samples.append(item);
        totals[sample.level]++;
        newestStored = sample;
        haveNewestStored = true;
    }

    if (liveConfig && !liveState.isNull()) {
        Json::Value live;
        live["ts"] = Json::Int64(generatedAt);
        const bool active = liveState.get("active", false).asBool();
        const uint64_t inputKbps = liveState.get("bitrate_in_kbps", Json::UInt64(0)).asUInt64();
        const uint64_t outputKbps = liveState.get("bitrate_out_kbps", Json::UInt64(0)).asUInt64();
        const uint64_t targetKbps = (liveConfig->transcodeEnabled
            ? nativeMuxBitrate(*liveConfig)
            : liveConfig->targetBitrate) / 1000;
        const uint64_t inputTotal = liveState.get("input_cc_errors_total", Json::UInt64(0)).asUInt64();
        const uint64_t outputTotal = liveState.get("output_cc_errors_total", Json::UInt64(0)).asUInt64();
        const auto counterDelta = [](uint64_t current, uint64_t previous) {
            return current >= previous ? current - previous : current;
        };
        uint64_t inputCc = liveState.get("input_cc_errors", Json::UInt64(0)).asUInt64();
        uint64_t outputCc = liveState.get("output_cc_errors", Json::UInt64(0)).asUInt64();
        if (haveNewestStored && newestStored.timestamp < generatedAt) {
            inputCc = counterDelta(inputTotal, newestStored.inputCcErrorsTotal);
            outputCc = counterDelta(outputTotal, newestStored.outputCcErrorsTotal);
        } else if (haveNewestStored && newestStored.timestamp == generatedAt) {
            inputCc = newestStored.inputCcErrors;
            outputCc = newestStored.outputCcErrors;
        }

        live["active"] = active;
        live["input_kbps"] = Json::UInt64(inputKbps);
        live["output_kbps"] = Json::UInt64(outputKbps);
        live["target_kbps"] = Json::UInt64(targetKbps);
        live["input_cc_errors"] = Json::UInt64(inputCc);
        live["output_cc_errors"] = Json::UInt64(outputCc);
        live["input_cc_errors_total"] = Json::UInt64(inputTotal);
        live["output_cc_errors_total"] = Json::UInt64(outputTotal);
        live["cc_errors"] = Json::UInt64(inputCc);
        const std::string status = liveState.get("status", "").asString();
        live["status"] = status;
        const auto classified = classifyQualitySample(
            active, status, inputKbps, outputKbps, targetKbps, inputCc, outputCc);
        live["level"] = classified.first;
        live["message"] = classified.second;
        root["live_sample"] = live;
    }

    root["samples"] = samples;
    Json::Value summary;
    summary["ok"] = Json::UInt(totals["ok"]);
    summary["warn"] = Json::UInt(totals["warn"]);
    summary["error"] = Json::UInt(totals["error"]);
    summary["offline"] = Json::UInt(totals["offline"]);
    uint64_t inputCcErrorsTotal = 0;
    uint64_t outputCcErrorsTotal = 0;
    for (const auto& sample : samples) {
        inputCcErrorsTotal += sample.get("input_cc_errors", Json::UInt64(0)).asUInt64();
        outputCcErrorsTotal += sample.get("output_cc_errors", Json::UInt64(0)).asUInt64();
    }
    summary["input_cc_errors"] = Json::UInt64(inputCcErrorsTotal);
    summary["output_cc_errors"] = Json::UInt64(outputCcErrorsTotal);
    summary["cc_errors"] = Json::UInt64(inputCcErrorsTotal);
    root["summary"] = summary;

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

void HttpServer::recordQualitySample(const StreamConfig& cfg, const Json::Value& state) {
    const int64_t now = unixNowSeconds();

    uint64_t previousInputCcTotal = 0;
    uint64_t previousOutputCcTotal = 0;
    bool havePreviousCcTotals = false;
    {
        std::lock_guard<std::mutex> lock(qualityMutex);
        const auto found = qualitySamples.find(cfg.id);
        if (found != qualitySamples.end() && !found->second.empty()) {
            if (now - found->second.back().timestamp < kQualityRecordIntervalSeconds) return;
            previousInputCcTotal = found->second.back().inputCcErrorsTotal;
            previousOutputCcTotal = found->second.back().outputCcErrorsTotal;
            havePreviousCcTotals = true;
        }
    }

    QualitySample sample;
    sample.timestamp = now;
    sample.active = state.get("active", false).asBool();
    sample.inputKbps = state.get("bitrate_in_kbps", Json::UInt64(0)).asUInt64();
    sample.outputKbps = state.get("bitrate_out_kbps", Json::UInt64(0)).asUInt64();
    sample.targetKbps = (cfg.transcodeEnabled ? nativeMuxBitrate(cfg) : cfg.targetBitrate) / 1000;
    const uint64_t fallbackInputCc = state.get("input_cc_errors",
        state.get("cc_errors", Json::UInt64(0))).asUInt64();
    const uint64_t fallbackOutputCc = state.get("output_cc_errors", Json::UInt64(0)).asUInt64();
    sample.inputCcErrorsTotal = state.get("input_cc_errors_total",
        Json::UInt64(fallbackInputCc)).asUInt64();
    sample.outputCcErrorsTotal = state.get("output_cc_errors_total",
        Json::UInt64(fallbackOutputCc)).asUInt64();
    if (havePreviousCcTotals) {
        sample.inputCcErrors = sample.inputCcErrorsTotal >= previousInputCcTotal
            ? sample.inputCcErrorsTotal - previousInputCcTotal
            : sample.inputCcErrorsTotal;
        sample.outputCcErrors = sample.outputCcErrorsTotal >= previousOutputCcTotal
            ? sample.outputCcErrorsTotal - previousOutputCcTotal
            : sample.outputCcErrorsTotal;
    } else {
        sample.inputCcErrors = fallbackInputCc;
        sample.outputCcErrors = fallbackOutputCc;
    }
    sample.status = state.get("status", "").asString();
    const auto classified = classifyQualitySample(
        sample.active, sample.status, sample.inputKbps, sample.outputKbps,
        sample.targetKbps, sample.inputCcErrors, sample.outputCcErrors);
    sample.level = classified.first;
    sample.message = classified.second;

    std::lock_guard<std::mutex> lock(qualityMutex);
    auto& samples = qualitySamples[cfg.id];
    if (!samples.empty() && now - samples.back().timestamp < kQualityRecordIntervalSeconds) return;
    if (!samples.empty() && samples.back().timestamp == sample.timestamp) {
        samples.back() = std::move(sample);
    } else {
        samples.push_back(std::move(sample));
    }

    // Keep only a small high-resolution runtime ring. Long-term history is on
    // disk in the requested stream's V2 shard and is never bulk-loaded.
    while (samples.size() > kQualityMemorySamplesPerStream) samples.pop_front();
}
