from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, found {count}")
    return text.replace(old, new, 1)


manager_path = Path("src/StreamManager.cpp")
manager = manager_path.read_text(encoding="utf-8")

monitor_state_anchor = '''    std::size_t rateWindowIndex = 0;
    std::size_t rateWindowSamples = 0;
    std::uint64_t monitorTicks = 0;
    bool telegramInputUnavailable = false;
    while (!state->monitorStop.load()) {
'''
monitor_state_replacement = '''    std::size_t rateWindowIndex = 0;
    std::size_t rateWindowSamples = 0;
    std::uint64_t monitorTicks = 0;
    std::uint64_t telegramNoInputSeconds = 0;
    bool telegramInputUnavailable = false;
    constexpr std::uint64_t kTelegramInputLossSeconds = 5;
    while (!state->monitorStop.load()) {
'''
manager = replace_once(
    manager,
    monitor_state_anchor,
    monitor_state_replacement,
    "StreamManager Telegram monitor state",
)

health_anchor = '''        // For long-running HTTP inputs the relay intentionally stays alive while
        // reconnecting.  Surface that state instead of leaving the UI falsely
        // ONLINE with empty bitrate fields.  As soon as data resumes, restore
        // the normal running status.
        const std::string relayErrorNow = relay->lastError();
        if (in == 0 && !relayErrorNow.empty()) {
            state->statusMessage = relayErrorNow;
            if (!telegramInputUnavailable) {
                sendTelegramStreamState(
                    telegramNotifier, configManager, state->config, "🔴",
                    telegramText(configManager, "Входной поток недоступен", "Input stream unavailable"),
                    relayErrorNow);
                telegramInputUnavailable = true;
            }
        } else if (in > 0 && lastIn == 0) {
            if (telegramInputUnavailable) {
                sendTelegramStreamState(
                    telegramNotifier, configManager, state->config, "🟢",
                    telegramText(configManager, "Входной поток восстановлен", "Input stream recovered"),
                    telegramText(configManager, "Медиаданные снова поступают", "Media data is flowing again"));
                telegramInputUnavailable = false;
            }
            const std::string normalized = normalizeInputUri(state->config.inputUri);
            const std::string mode = toLower(state->config.inputMode);
            const bool hls = mode == "hls" || toLower(normalized).find(".m3u8") != std::string::npos ||
                toLower(normalized).rfind("hls://", 0) == 0;
            const bool srt = toLower(normalized).rfind("srt://", 0) == 0;
            const bool rtsp = toLower(normalized).rfind("rtsp://", 0) == 0;
            const bool rtmp = toLower(normalized).rfind("rtmp://", 0) == 0 || toLower(normalized).rfind("rtmps://", 0) == 0;
            state->statusMessage = hls ? "running (native HLS input)" :
                (srt ? "running (native SRT input)" :
                (rtsp ? "running (native RTSP input)" :
                (rtmp ? "running (native RTMP input)" : "running (native media engine)")));
            if (state->config.transcodeEnabled) state->statusMessage += " + native transcoder";
        }
'''
health_replacement = '''        // Runtime input-health notification must use packet progress, not the
        // absolute cumulative byte counter.  A UDP/SRT/HTTP relay can stay alive
        // forever after packets disappear, so `in == 0` only detects the special
        // case where the stream never delivered a byte.  Five seconds without
        // counter progress is treated as an outage; an explicit relay error is
        // reported immediately.  Any later byte progress is a recovery.
        const std::string relayErrorNow = relay->lastError();
        const bool inputAdvanced = in > lastIn;
        const bool recoveredFromInputLoss = inputAdvanced && telegramInputUnavailable;
        if (inputAdvanced) {
            telegramNoInputSeconds = 0;
            if (recoveredFromInputLoss) {
                sendTelegramStreamState(
                    telegramNotifier, configManager, state->config, "🟢",
                    telegramText(configManager, "Входной поток восстановлен", "Input stream recovered"),
                    telegramText(configManager, "Медиаданные снова поступают", "Media data is flowing again"));
                telegramInputUnavailable = false;
            }

            if (lastIn == 0 || recoveredFromInputLoss) {
                const std::string normalized = normalizeInputUri(state->config.inputUri);
                const std::string mode = toLower(state->config.inputMode);
                const bool hls = mode == "hls" || toLower(normalized).find(".m3u8") != std::string::npos ||
                    toLower(normalized).rfind("hls://", 0) == 0;
                const bool srt = toLower(normalized).rfind("srt://", 0) == 0;
                const bool rtsp = toLower(normalized).rfind("rtsp://", 0) == 0;
                const bool rtmp = toLower(normalized).rfind("rtmp://", 0) == 0 || toLower(normalized).rfind("rtmps://", 0) == 0;
                state->statusMessage = hls ? "running (native HLS input)" :
                    (srt ? "running (native SRT input)" :
                    (rtsp ? "running (native RTSP input)" :
                    (rtmp ? "running (native RTMP input)" : "running (native media engine)")));
                if (state->config.transcodeEnabled) state->statusMessage += " + native transcoder";
            }
        } else {
            if (telegramNoInputSeconds < kTelegramInputLossSeconds) {
                ++telegramNoInputSeconds;
            }
            const bool inputTimedOut = telegramNoInputSeconds >= kTelegramInputLossSeconds;
            if (!relayErrorNow.empty() || inputTimedOut) {
                const std::string inputLossDetails = !relayErrorNow.empty()
                    ? relayErrorNow
                    : telegramText(
                        configManager,
                        "Нет входных медиаданных более 5 секунд",
                        "No input media data for more than 5 seconds");
                state->statusMessage = inputLossDetails;
                if (!telegramInputUnavailable) {
                    sendTelegramStreamState(
                        telegramNotifier, configManager, state->config, "🔴",
                        telegramText(configManager, "Входной поток недоступен", "Input stream unavailable"),
                        inputLossDetails);
                    telegramInputUnavailable = true;
                }
            }
        }
'''
manager = replace_once(
    manager,
    health_anchor,
    health_replacement,
    "StreamManager runtime input health",
)

async_stop_anchor = '''bool StreamManager::stopStreamAsync(const std::string& id) { return stopStream(id); }
'''
async_stop_replacement = '''bool StreamManager::stopStreamAsync(const std::string& id) {
    // This entry point is used by the control-panel /api/stop-stream action.
    // Internal teardown/restart paths call stopStream() directly and therefore
    // do not generate a misleading "stopped by user" Telegram notification.
    StreamConfig stoppedConfig;
    bool haveStoppedConfig = false;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto it = streams.find(id);
        if (it != streams.end() && it->second) {
            stoppedConfig = it->second->config;
            haveStoppedConfig = true;
        }
    }

    const bool stopped = stopStream(id);
    if (stopped && haveStoppedConfig) {
        sendTelegramStreamState(
            telegramNotifier, configManager, stoppedConfig, "🟡",
            telegramText(configManager, "Поток остановлен", "Stream stopped"),
            telegramText(configManager, "Остановлен пользователем через панель", "Stopped by user from control panel"));
    }
    return stopped;
}
'''
manager = replace_once(
    manager,
    async_stop_anchor,
    async_stop_replacement,
    "StreamManager manual stop notification",
)

manager_path.write_text(manager, encoding="utf-8")

version_path = Path("src/AppVersion.h")
version = version_path.read_text(encoding="utf-8")
version = replace_once(
    version,
    'inline constexpr const char* kProgramVersion = "10.8.106";',
    'inline constexpr const char* kProgramVersion = "10.8.107";',
    "AppVersion 10.8.107",
)
version_path.write_text(version, encoding="utf-8")
