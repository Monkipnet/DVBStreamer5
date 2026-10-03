#include "StreamManager.h"

// Keep the complete, previously validated implementation in one translation
// unit, but rename only its old HTTP-client method.  The replacement below
// separates public HTTP MPEG-TS from the private browser preview path.
#define addHttpClient addHttpClientOriginal
#include "StreamManagerImpl.inc"
#undef addHttpClient

bool StreamManager::addHttpClient(const std::string& id, int fd,
                                  const std::string& clientIp,
                                  const std::string& previewSession) {
    std::shared_ptr<dvbstreamer5::media::network::NativePreviewHub> hub;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto it = streams.find(id);
        if (it == streams.end() || !it->second || !it->second->active.load()) {
            ::close(fd);
            return false;
        }

        if (!previewSession.empty()) {
            // Private /preview.ts clients intentionally use the H.264/AAC
            // preview hub. Their subscriber count is the only thing allowed to
            // wake the lazy browser-preview transcoder.
            hub = it->second->nativePreviewHub;
        } else if (it->second->nativeRelay) {
            // Public HTTP MPEG-TS receives the final production transport
            // directly: post-remap, post-CA and post-production-transcode.
            // It must never become a browser-preview subscriber.
            hub = it->second->nativeRelay->httpOutputHub();
        }

        if (!hub) {
            ::close(fd);
            return false;
        }
    }

    std::string subscribeError;
    const int upstreamFd = hub->subscribe(subscribeError);
    if (upstreamFd < 0) {
        ::close(fd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(managerMutex);
        httpClients[fd] = {id, normalizeIpAddress(clientIp), "mpegts",
                           std::chrono::steady_clock::now(), upstreamFd,
                           previewSession};
    }

    try {
        std::thread([this, hub, fd, upstreamFd]() {
            std::array<char, 64 * 1024> buffer{};
            for (;;) {
                const ssize_t n = ::read(upstreamFd, buffer.data(), buffer.size());
                if (n > 0) {
                    if (!writeAll(fd, buffer.data(), static_cast<std::size_t>(n))) {
                        break;
                    }
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                break;
            }

            std::lock_guard<std::mutex> lock(managerMutex);
            httpClients.erase(fd);
            hub->unsubscribe(upstreamFd);
            ::close(upstreamFd);
            ::close(fd);
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(managerMutex);
        httpClients.erase(fd);
        hub->unsubscribe(upstreamFd);
        ::close(upstreamFd);
        ::close(fd);
        return false;
    }

    return true;
}
