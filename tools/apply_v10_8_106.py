from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, found {count}")
    return text.replace(old, new, 1)


# StreamManager: keep the V10.8.105 CBR transport untouched.  This patch only
# fixes UI telemetry and restores Telegram notifications lost in the native
# media-engine migration.
manager_path = Path("src/StreamManager.cpp")
manager = manager_path.read_text()

helper_anchor = '''bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

const char* browserPreviewCodecName(
'''
helper_replacement = '''bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string telegramEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

bool telegramUsesEnglish(const ConfigManager& manager) {
    return toLower(manager.config.language) == "en";
}

std::string telegramText(const ConfigManager& manager,
                         const char* ru, const char* en) {
    return telegramUsesEnglish(manager) ? en : ru;
}

std::string telegramStreamName(const StreamConfig& cfg) {
    return cfg.name.empty() ? cfg.id : cfg.name;
}

void sendTelegramStreamState(TelegramNotifier& notifier,
                             const ConfigManager& manager,
                             const StreamConfig& cfg,
                             const std::string& color,
                             const std::string& title,
                             const std::string& details) {
    const std::string serverName = manager.config.serverName.empty()
        ? "DVBStreamer5"
        : manager.config.serverName;
    const bool english = telegramUsesEnglish(manager);
    std::ostringstream message;
    message << color << " <b>" << telegramEscape(title) << "</b>\\n"
            << (english ? "Server" : "Сервер") << ": <b>"
            << telegramEscape(serverName) << "</b>\\n"
            << (english ? "Channel" : "Канал") << ": <b>"
            << telegramEscape(telegramStreamName(cfg)) << "</b>\\n"
            << "ID: <code>" << telegramEscape(cfg.id) << "</code>";
    if (!details.empty()) message << "\\n" << telegramEscape(details);
    notifier.sendMessage(message.str());
}

const char* browserPreviewCodecName(
'''
manager = replace_once(manager, helper_anchor, helper_replacement,
                       "StreamManager Telegram helper anchor")

start_anchor = '''    try {
        rawState->monitorThread = std::thread(&StreamManager::monitorNativeStream, this, rawState);
    } catch (const std::exception& ex) {
        stopStream(streamConfig.id);
        if (error) *error = std::string("native monitor thread failed: ") + ex.what();
        return false;
    }
    return true;
}

void StreamManager::monitorNativeStream(StreamState* state) {
'''
start_replacement = '''    try {
        rawState->monitorThread = std::thread(&StreamManager::monitorNativeStream, this, rawState);
    } catch (const std::exception& ex) {
        stopStream(streamConfig.id);
        if (error) *error = std::string("native monitor thread failed: ") + ex.what();
        return false;
    }
    sendTelegramStreamState(
        telegramNotifier, configManager, streamConfig, "🟢",
        telegramText(configManager, "Поток запущен", "Stream started"),
        telegramText(configManager, "Native media engine работает", "Native media engine is running"));
    return true;
}

void StreamManager::monitorNativeStream(StreamState* state) {
'''
manager = replace_once(manager, start_anchor, start_replacement,
                       "StreamManager start notification anchor")

monitor_anchor = '''    std::size_t rateWindowIndex = 0;
    std::size_t rateWindowSamples = 0;
    std::uint64_t monitorTicks = 0;
    while (!state->monitorStop.load()) {
'''
monitor_replacement = '''    std::size_t rateWindowIndex = 0;
    std::size_t rateWindowSamples = 0;
    std::uint64_t monitorTicks = 0;
    bool telegramInputUnavailable = false;
    while (!state->monitorStop.load()) {
'''
manager = replace_once(manager, monitor_anchor, monitor_replacement,
                       "StreamManager monitor Telegram state anchor")

bitrate_anchor = '''        const uint64_t outRate = (out - lastOut) * 8;
        // Outputs such as an SRT listener can have zero sentBytes until a
        // receiver connects. Never fall back to the raw DVB multiplex rate;
        // use the channel pipeline bitrate instead.
        state->outputBitrate.store(outRate ? outRate : payloadRate);
        state->outputPayloadBitrate.store(payloadRate);
'''
bitrate_replacement = '''        const uint64_t outRate = (out - lastOut) * 8;
        // V10.8.106: CBR for HTTP/SRT is produced by the observer pacer and is
        // independent of whether an HTTP client or SRT peer is currently
        // connected.  sentBytes therefore is not a valid stream-level CBR
        // meter.  Once real channel payload has started, show the configured
        // shaped transport rate in the tile; VBR keeps the measured rate.
        const bool cbrTransportStarted =
            state->config.cbr && state->config.targetBitrate > 0 && payloadOut > 0;
        state->outputBitrate.store(
            cbrTransportStarted
                ? state->config.targetBitrate
                : (outRate ? outRate : payloadRate));
        state->outputPayloadBitrate.store(payloadRate);
'''
manager = replace_once(manager, bitrate_anchor, bitrate_replacement,
                       "StreamManager CBR tile bitrate anchor")

health_anchor = '''        const std::string relayErrorNow = relay->lastError();
        if (in == 0 && !relayErrorNow.empty()) {
            state->statusMessage = relayErrorNow;
        } else if (in > 0 && lastIn == 0) {
            const std::string normalized = normalizeInputUri(state->config.inputUri);
'''
health_replacement = '''        const std::string relayErrorNow = relay->lastError();
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
'''
manager = replace_once(manager, health_anchor, health_replacement,
                       "StreamManager input health Telegram anchor")

stop_anchor = '''            state->running.store(false);
            state->active.store(false);
            const std::string relayError = relay->lastError();
            state->statusMessage = relayError.empty() ? "native input stopped" : relayError;
            break;
'''
stop_replacement = '''            state->running.store(false);
            state->active.store(false);
            const std::string relayError = relay->lastError();
            state->statusMessage = relayError.empty() ? "native input stopped" : relayError;
            sendTelegramStreamState(
                telegramNotifier, configManager, state->config, "🔴",
                telegramText(configManager, "Поток аварийно остановлен", "Stream stopped unexpectedly"),
                state->statusMessage);
            break;
'''
manager = replace_once(manager, stop_anchor, stop_replacement,
                       "StreamManager unexpected stop Telegram anchor")

manager_path.write_text(manager)


# TelegramNotifier: keep query values encoded, but the bot token is a path
# segment and must not be query-escaped (':' is part of normal Telegram bot
# tokens).  Validate it before using it as raw path data and verify Bot API JSON.
notifier_path = Path("src/TelegramNotifier.cpp")
notifier = notifier_path.read_text()
notifier = replace_once(
    notifier,
    '#include <chrono>\n#include <exception>\n',
    '#include <chrono>\n#include <cctype>\n#include <exception>\n',
    "TelegramNotifier include anchor")

const_anchor = '''constexpr auto kRepeatedStreamEventWindow = std::chrono::minutes(30);
constexpr std::size_t kTelegramQueueMax = 64;

std::string extractBetween(
'''
const_replacement = '''constexpr auto kRepeatedStreamEventWindow = std::chrono::minutes(30);
constexpr std::size_t kTelegramQueueMax = 64;

std::string normalizedTelegramToken(std::string token) {
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
        token.erase(token.begin());
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
        token.pop_back();
    if (token.rfind("bot", 0) == 0 && token.find(':', 3) != std::string::npos)
        token.erase(0, 3);
    return token;
}

bool telegramTokenPathSafe(const std::string& token) {
    if (token.empty() || token.find(':') == std::string::npos) return false;
    for (const unsigned char ch : token) {
        if (std::isalnum(ch) || ch == ':' || ch == '_' || ch == '-') continue;
        return false;
    }
    return true;
}

bool telegramApiResponseOk(const dvbstreamer5::http::Response& response) {
    if (response.body.empty()) return false;
    const std::string body(response.body.begin(), response.body.end());
    return body.find("\\\"ok\\\":true") != std::string::npos ||
           body.find("\\\"ok\\\": true") != std::string::npos;
}

std::string extractBetween(
'''
notifier = replace_once(notifier, const_anchor, const_replacement,
                        "TelegramNotifier helper anchor")

url_anchor = '''    std::ostringstream url;
    url << "https://api.telegram.org/bot"
        << dvbstreamer5::http::encodeQueryComponent(config.telegramToken)
        << "/sendMessage?chat_id="
'''
url_replacement = '''    const std::string botToken = normalizedTelegramToken(config.telegramToken);
    if (!telegramTokenPathSafe(botToken)) {
        std::cerr << "Telegram send error: invalid bot token format" << std::endl;
        rollbackRepeatReservation();
        return;
    }

    std::ostringstream url;
    url << "https://api.telegram.org/bot"
        << botToken
        << "/sendMessage?chat_id="
'''
notifier = replace_once(notifier, url_anchor, url_replacement,
                        "TelegramNotifier URL anchor")

send_anchor = '''    if (!dvbstreamer5::http::get(requestUrl, options, response, error)) {
        std::cerr << "Telegram send error: " << error << std::endl;
        rollbackRepeatReservation();
    }
}
'''
send_replacement = '''    if (!dvbstreamer5::http::get(requestUrl, options, response, error)) {
        std::cerr << "Telegram send error: " << error
                  << " status=" << response.status << std::endl;
        rollbackRepeatReservation();
        return;
    }
    if (!telegramApiResponseOk(response)) {
        std::cerr << "Telegram send error: Bot API returned unexpected response"
                  << " status=" << response.status
                  << " body_bytes=" << response.body.size() << std::endl;
        rollbackRepeatReservation();
        return;
    }
    std::cerr << "Telegram send ok: status=" << response.status << std::endl;
}
'''
notifier = replace_once(notifier, send_anchor, send_replacement,
                        "TelegramNotifier send result anchor")
notifier_path.write_text(notifier)


version_path = Path("src/AppVersion.h")
version = version_path.read_text()
version = replace_once(
    version,
    'inline constexpr const char* kProgramVersion = "10.8.105";',
    'inline constexpr const char* kProgramVersion = "10.8.106";',
    "AppVersion")
version_path.write_text(version)
