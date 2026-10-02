#include "NativePreviewHub.h"

#include "TransportStream.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dvbstreamer5::media::network {

NativePreviewHub::~NativePreviewHub() {
    close();
}

int NativePreviewHub::subscribe(std::string& error) {
#if defined(_WIN32)
    error = "native preview socket fan-out is unavailable on Windows";
    return -1;
#else
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        error = std::string("cannot create native preview socket pair: ") +
            std::strerror(errno);
        return -1;
    }
    const int flags = ::fcntl(sockets[1], F_GETFL, 0);
    if (flags < 0 || ::fcntl(sockets[1], F_SETFL, flags | O_NONBLOCK) != 0) {
        error = std::string("cannot make native preview nonblocking: ") +
            std::strerror(errno);
        ::close(sockets[0]);
        ::close(sockets[1]);
        return -1;
    }
    const int queueBytes = 64 * 1024;
    ::setsockopt(sockets[1], SOL_SOCKET, SO_SNDBUF, &queueBytes, sizeof(queueBytes));

    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        error = "native preview hub is closed";
        ::close(sockets[0]);
        ::close(sockets[1]);
        return -1;
    }
    subscribers_.emplace(sockets[0], sockets[1]);
    return sockets[0];
#endif
}

void NativePreviewHub::unsubscribe(int readFd) {
#if !defined(_WIN32)
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = subscribers_.find(readFd);
    if (found == subscribers_.end()) return;
    ::close(found->second);
    subscribers_.erase(found);
#else
    (void)readFd;
#endif
}

void NativePreviewHub::publish(const std::uint8_t* data, std::size_t size) {
#if !defined(_WIN32)
    if (!data || size < dvbstreamer5::media::mpegts::kPacketSize) return;
    constexpr std::size_t kPreviewDatagramBytes =
        7 * dvbstreamer5::media::mpegts::kPacketSize;
    const std::size_t alignedSize =
        size - (size % dvbstreamer5::media::mpegts::kPacketSize);

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto subscriber = subscribers_.begin(); subscriber != subscribers_.end();) {
        bool remove = false;
        for (std::size_t offset = 0; offset < alignedSize;) {
            const std::size_t chunk =
                std::min(kPreviewDatagramBytes, alignedSize - offset);
            const ssize_t sent = ::send(
                subscriber->second, data + offset, chunk,
                MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent == static_cast<ssize_t>(chunk)) {
                offset += chunk;
                continue;
            }
            if (sent < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
                break;
            }
            remove = true;
            break;
        }
        if (remove) {
            ::close(subscriber->second);
            subscriber = subscribers_.erase(subscriber);
        } else {
            ++subscriber;
        }
    }
#else
    (void)data;
    (void)size;
#endif
}

std::size_t NativePreviewHub::subscriberCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_ ? 0U : subscribers_.size();
}

void NativePreviewHub::close() {
#if !defined(_WIN32)
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    closed_ = true;
    for (const auto& subscriber : subscribers_) {
        ::close(subscriber.second);
    }
    subscribers_.clear();
#endif
}

} // namespace dvbstreamer5::media::network
