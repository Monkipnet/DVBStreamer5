#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
#include <map>
#include <mutex>
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

private:
    std::mutex mutex_;
    struct Subscriber {
        int sendFd = -1;
        std::deque<std::vector<std::uint8_t>> pending;
        std::size_t pendingBytes = 0;
    };
    static constexpr std::size_t kMaxPendingBytes = 4U * 1024U * 1024U;
    std::map<int, Subscriber> subscribers_;
    bool closed_ = false;
};

} // namespace dvbstreamer5::media::network
