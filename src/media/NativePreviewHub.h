#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace tvs::media::network {

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
    void close();

private:
    std::mutex mutex_;
    std::map<int, int> subscribers_;
    bool closed_ = false;
};

} // namespace tvs::media::network
