#include "TelegramNotifier.h"
#include "NativeHttpClient.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <sstream>
#include <utility>

namespace {
constexpr auto kRepeatedStreamEventWindow = std::chrono::minutes(30);
constexpr std::size_t kTelegramQueueMax = 64;

std::string extractBetween(
    const std::string& text, const std::string& begin, const std::string& end,
    size_t startAt = 0) {
    const auto beginPos = text.find(begin, startAt);
    if (beginPos == std::string::npos) return {};
    const auto valueStart = beginPos + begin.size();
    const auto endPos = text.find(end, valueStart);
    if (endPos == std::string::npos || endPos <= valueStart) return {};
    return text.substr(valueStart, endPos - valueStart);
}

std::string streamIdFromTelegramMessage(const std::string& text) {
    return extractBetween(text, "ID: <code>", "</code>");
}

std::string eventTitleFromTelegramMessage(const std::string& text) {
    return extractBetween(text, "<b>", "</b>");
}
}

TelegramNotifier::TelegramNotifier(ConfigManager& cfg)
    : manager(cfg) {
    try {
        worker = std::thread(&TelegramNotifier::workerLoop, this);
    } catch (const std::exception& ex) {
        std::cerr << "Telegram async worker unavailable: " << ex.what()
                  << " action=notifications-disabled" << std::endl;
    }
}

TelegramNotifier::~TelegramNotifier() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
        pendingMessages.clear();
    }
    queueCondition.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

void TelegramNotifier::sendMessage(const std::string& text) {
    const auto& config = manager.config;
    if (config.telegramToken.empty() || config.telegramChatId.empty() ||
        !worker.joinable()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (stopping) return;
        if (pendingMessages.size() >= kTelegramQueueMax) {
            pendingMessages.pop_front();
            std::cerr << "Telegram async queue full: limit=" << kTelegramQueueMax
                      << " action=drop-oldest" << std::endl;
        }
        pendingMessages.push_back(text);
    }
    queueCondition.notify_one();
}

void TelegramNotifier::workerLoop() {
    for (;;) {
        std::string text;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCondition.wait(lock, [this] {
                return stopping || !pendingMessages.empty();
            });
            if (stopping) return;
            text = std::move(pendingMessages.front());
            pendingMessages.pop_front();
        }

        try {
            sendMessageBlocking(text);
        } catch (const std::exception& ex) {
            std::cerr << "Telegram async worker exception: " << ex.what()
                      << std::endl;
        } catch (...) {
            std::cerr << "Telegram async worker unknown exception" << std::endl;
        }
    }
}

void TelegramNotifier::sendMessageBlocking(const std::string& text) {
    const auto& config = manager.config;
    if (config.telegramToken.empty() || config.telegramChatId.empty()) {
        return;
    }

    // 202.81: a pipeline rebuild creates a fresh StreamState and can otherwise
    // resend the same warning every few seconds while the same network incident
    // is still active. Suppress the same event title for the same stream for
    // 30 minutes. A different event for that stream (for example, recovery) is
    // delivered immediately and resets the state, so a later real outage is not
    // hidden. This dedupe is process-local and does not affect journald logging.
    const std::string streamId = streamIdFromTelegramMessage(text);
    const std::string eventTitle = eventTitleFromTelegramMessage(text);
    const auto now = std::chrono::steady_clock::now();
    bool hadPreviousStreamEvent = false;
    StreamEventState previousStreamEvent;
    if (!streamId.empty() && !eventTitle.empty()) {
        std::lock_guard<std::mutex> lock(repeatMutex);
        auto it = streamEvents.find(streamId);
        if (it != streamEvents.end()) {
            hadPreviousStreamEvent = true;
            previousStreamEvent = it->second;
            if (it->second.title == eventTitle &&
                now - it->second.lastSent < kRepeatedStreamEventWindow) {
                std::cerr << "Telegram duplicate stream event suppressed: stream="
                          << streamId << " title=" << eventTitle << std::endl;
                return;
            }
        }
        streamEvents[streamId] = StreamEventState{eventTitle, now};
    }

    const auto rollbackRepeatReservation = [&]() {
        if (streamId.empty() || eventTitle.empty()) return;
        std::lock_guard<std::mutex> lock(repeatMutex);
        auto it = streamEvents.find(streamId);
        if (it == streamEvents.end() || it->second.title != eventTitle ||
            it->second.lastSent != now) {
            return;
        }
        if (hadPreviousStreamEvent) {
            it->second = previousStreamEvent;
        } else {
            streamEvents.erase(it);
        }
    };

    std::ostringstream url;
    url << "https://api.telegram.org/bot"
        << tvs::http::encodeQueryComponent(config.telegramToken)
        << "/sendMessage?chat_id="
        << tvs::http::encodeQueryComponent(config.telegramChatId)
        << "&parse_mode=HTML"
        << "&disable_web_page_preview=true"
        << "&text=" << tvs::http::encodeQueryComponent(text);

    const std::string requestUrl = url.str();
    tvs::http::RequestOptions options;
    options.connectTimeoutMs = 2000;
    options.readTimeoutMs = 4000;
    options.writeTimeoutMs = 2000;
    options.totalTimeoutMs = 4000;
    options.maxRedirects = 4;
    options.maxBodyBytes = 1024U * 1024U;
    options.verifyTlsPeer = true;
    tvs::http::Response response;
    std::string error;
    if (!tvs::http::get(requestUrl, options, response, error)) {
        std::cerr << "Telegram send error: " << error << std::endl;
        rollbackRepeatReservation();
    }
}
