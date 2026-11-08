#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <string>

namespace dvbstreamer5::media::network {

// Best-effort local MPEG-TS fan-out for browser previews. Each subscriber has
// a bounded datagram socket queue, so a slow browser can never back-pressure
// the production transport path.
class NativePreviewHub {
public:
    NativePreviewHub() = default;
    ~NativePreviewHub();

    NativePreviewHub(const NativePreviewHub&) = delete;
    NativePreviewHub& operator=(const NativePreviewHub&) = delete;

    int subscribe(std::string& error);
    void unsubscribe(int readFd);
    void publish(const std::uint8_t* data, std::size_t size);
    std::size_t subscriberCount();
    void close();
    // Keep subscribers open, but discard old media queued by slow consumers.
    void flushPending();

private:
    std::mutex mutex_;
    struct Subscriber {
        int sendFd = -1;
        std::mutex mutex;
        // Immutable pending chunks are shared by all lagging viewers of one
        // source publish() call. No duplicate large TS buffers per viewer.
        std::deque<std::shared_ptr<const std::vector<std::uint8_t>>> pending;
        std::size_t pendingBytes = 0;
        bool backlogWarningLogged = false;
        bool detached = false;
    };
    static constexpr std::size_t kMaxPendingBytes = 4U * 1024U * 1024U;
    std::map<int, std::shared_ptr<Subscriber>> subscribers_;
    bool closed_ = false;
    std::chrono::steady_clock::time_point lastRateSample_{};
    std::uint64_t publishedBytes_ = 0;
    std::uint64_t publishedCalls_ = 0;
};

} // namespace dvbstreamer5::media::network
