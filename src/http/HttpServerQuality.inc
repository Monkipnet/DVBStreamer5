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

std::string HttpServer::qualityHistory(const std::string& target) {
    const std::string id = queryValue(target, "id");
    uint64_t periodSeconds = 3600;
    try {
        const std::string period = queryValue(target, "period");
        if (!period.empty()) {
            periodSeconds = std::stoull(period);
        }
    } catch (const std::exception&) {
        periodSeconds = 3600;
    }
    periodSeconds = std::clamp<uint64_t>(periodSeconds, 60, 30ULL * 24ULL * 60ULL * 60ULL);

    // 203.16: the quality modal is allowed to keep history alive by itself.
    // Previously the history was recorded only while /api/state happened to be
    // polled. If that polling was throttled/stalled by the browser, the chart
    // ended tens of minutes before the right edge even though the stream was
    // still running. Sample the requested live stream here as well; the normal
    // 30-second recorder guard keeps memory/CPU bounded.
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
    Json::Value root;
    root["id"] = id;
    root["period_seconds"] = Json::UInt64(periodSeconds);
    root["generated_at"] = Json::Int64(generatedAt);
    Json::Value samples(Json::arrayValue);

    const int64_t cutoff = generatedAt - static_cast<int64_t>(periodSeconds);
    std::map<std::string, unsigned int> totals = {
        {"ok", 0}, {"warn", 0}, {"error", 0}, {"offline", 0}
    };
    QualitySample newestStored;
    bool haveNewestStored = false;

    {
        std::lock_guard<std::mutex> lock(qualityMutex);
        auto found = qualitySamples.find(id);
        if (found != qualitySamples.end()) {
            if (!found->second.empty()) {
                newestStored = found->second.back();
                haveNewestStored = true;
            }
            for (const auto& sample : found->second) {
                if (sample.timestamp < cutoff) {
                    continue;
                }
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
            }
        }
    }

    // Add a non-persistent live point at the exact right edge. This keeps the
    // graph visually continuous even with the intentionally sparse 30-second
    // retained history, without increasing the history allocation rate.
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
        const std::string statusLower = toLower(status);
        std::string level = "ok";
        std::string message = "Качество в норме";
        if (!active) {
            level = "offline";
            message = status == "stopped" ? "Поток остановлен" : "Поток не активен: " + status;
        } else if (statusLower.find("error") != std::string::npos ||
                   statusLower.find("failed") != std::string::npos ||
                   statusLower.find("ended") != std::string::npos) {
            level = "error";
            message = "Ошибка media engine: " + status;
        } else if (inputKbps == 0) {
            level = "warn";
            message = "Нет входного битрейта при активном потоке";
        } else if (inputCc > 0 || outputCc > 0) {
            level = "error";
            message = "CC-errors MPEG-TS: вход=" + std::to_string(inputCc) +
                ", выход=" + std::to_string(outputCc);
        }
        live["level"] = level;
        live["message"] = message;
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

    // 202.50: do not allocate/copy status/message strings for every /api/state
    // poll.  The quality chart does not need 2-second resolution, and this early
    // check also avoids allocator churn on the skipped polls.
    uint64_t previousInputCcTotal = 0;
    uint64_t previousOutputCcTotal = 0;
    bool havePreviousCcTotals = false;
    {
        std::lock_guard<std::mutex> lock(qualityMutex);
        const auto found = qualitySamples.find(cfg.id);
        if (found != qualitySamples.end() && !found->second.empty()) {
            if (now - found->second.back().timestamp < kQualityRecordIntervalSeconds) {
                return;
            }
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
    const uint64_t fallbackInputCc = state.get("input_cc_errors", state.get("cc_errors", Json::UInt64(0))).asUInt64();
    const uint64_t fallbackOutputCc = state.get("output_cc_errors", Json::UInt64(0)).asUInt64();
    sample.inputCcErrorsTotal = state.get("input_cc_errors_total", Json::UInt64(fallbackInputCc)).asUInt64();
    sample.outputCcErrorsTotal = state.get("output_cc_errors_total", Json::UInt64(fallbackOutputCc)).asUInt64();
    if (havePreviousCcTotals) {
        sample.inputCcErrors = sample.inputCcErrorsTotal >= previousInputCcTotal
            ? sample.inputCcErrorsTotal - previousInputCcTotal
            : sample.inputCcErrorsTotal;
        sample.outputCcErrors = sample.outputCcErrorsTotal >= previousOutputCcTotal
            ? sample.outputCcErrorsTotal - previousOutputCcTotal
            : sample.outputCcErrorsTotal;
    } else {
        // First retained point has no previous 30-second baseline. Keep only the
        // current StreamManager delta rather than treating lifetime totals as one
        // enormous CC-error spike.
        sample.inputCcErrors = fallbackInputCc;
        sample.outputCcErrors = fallbackOutputCc;
    }
    sample.status = state.get("status", "").asString();

    const std::string statusLower = toLower(sample.status);
    if (!sample.active) {
        sample.level = "offline";
        sample.message = sample.status == "stopped" ? "Поток остановлен" : "Поток не активен: " + sample.status;
    } else if (statusLower.find("error") != std::string::npos ||
               statusLower.find("failed") != std::string::npos ||
               statusLower.find("ended") != std::string::npos) {
        sample.level = "error";
        sample.message = "Ошибка media engine: " + sample.status;
    } else if (sample.inputKbps == 0) {
        sample.level = "warn";
        sample.message = "Нет входного битрейта при активном потоке";
    } else if (sample.inputCcErrors > 0 || sample.outputCcErrors > 0) {
        sample.level = "error";
        sample.message = "CC-errors MPEG-TS: вход=" + std::to_string(sample.inputCcErrors) +
            ", выход=" + std::to_string(sample.outputCcErrors);
    } else if (sample.targetKbps > 0 && sample.outputKbps > 0) {
        const double diff = std::abs(static_cast<double>(sample.outputKbps) - static_cast<double>(sample.targetKbps));
        const double deviation = diff / static_cast<double>(sample.targetKbps);
        if (deviation > 0.20) {
            sample.level = "warn";
            sample.message = "Выходной битрейт отклоняется от цели больше чем на 20%";
        } else {
            sample.level = "ok";
            sample.message = "Качество в норме";
        }
    } else {
        sample.level = "ok";
        sample.message = "Качество в норме";
    }

    std::lock_guard<std::mutex> lock(qualityMutex);
    auto& samples = qualitySamples[cfg.id];
    // Another HTTP worker may have recorded this stream while the sample was
    // being prepared.  Re-check under the owning mutex before appending.
    if (!samples.empty() &&
        now - samples.back().timestamp < kQualityRecordIntervalSeconds) {
        return;
    }
    if (!samples.empty() && samples.back().timestamp == sample.timestamp) {
        samples.back() = sample;
    } else {
        samples.push_back(sample);
    }

    const int64_t cutoff = now - kQualityRetentionSeconds;
    while (!samples.empty() && samples.front().timestamp < cutoff) {
        samples.pop_front();
    }

    auto& lastCompaction = qualityLastCompaction[cfg.id];
    if (lastCompaction == 0 || now - lastCompaction >= kQualityCompactionIntervalSeconds) {
        std::deque<QualitySample> compacted;
        int64_t previousBucket = -1;
        int64_t previousStep = -1;
        for (const auto& existing : samples) {
            const int64_t age = std::max<int64_t>(0, now - existing.timestamp);
            const int64_t step = age <= kQualityFullResolutionSeconds
                ? 1LL
                : (age <= 24LL * 60LL * 60LL
                    ? kQualityDayResolutionSeconds
                    : kQualityMonthResolutionSeconds);
            const int64_t bucket = existing.timestamp / step;
            if (!compacted.empty() && step == previousStep && bucket == previousBucket) {
                // 203.16: bitrate/status use the newest sample in a compacted
                // bucket, but CC errors are interval counters and must be summed.
                // Replacing the whole sample used to erase continuity errors that
                // happened earlier in the same 1-minute/5-minute history bucket.
                const uint64_t inputCcInBucket = compacted.back().inputCcErrors + existing.inputCcErrors;
                const uint64_t outputCcInBucket = compacted.back().outputCcErrors + existing.outputCcErrors;
                compacted.back() = existing;
                compacted.back().inputCcErrors = inputCcInBucket;
                compacted.back().outputCcErrors = outputCcInBucket;
                if ((inputCcInBucket > 0 || outputCcInBucket > 0) && compacted.back().level == "ok") {
                    compacted.back().level = "error";
                    compacted.back().message = "CC-errors MPEG-TS за интервал: вход=" +
                        std::to_string(inputCcInBucket) + ", выход=" + std::to_string(outputCcInBucket);
                }
            } else {
                compacted.push_back(existing);
                previousStep = step;
                previousBucket = bucket;
            }
        }
        samples.swap(compacted);
        lastCompaction = now;
    }

    while (samples.size() > kQualityMaxSamplesPerStream) {
        samples.pop_front();
    }
}

