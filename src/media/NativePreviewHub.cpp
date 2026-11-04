#include "NativePreviewHub.h"

#include "TransportStream.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>

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
    // Reserve enough kernel space for bursty producer scheduling. Preserve
    // packet ordering and the bounded user-space queue on sustained stalls.
    const int queueBytes = 2 * 1024 * 1024;
    ::setsockopt(sockets[1], SOL_SOCKET, SO_SNDBUF, &queueBytes, sizeof(queueBytes));
    ::setsockopt(sockets[0], SOL_SOCKET, SO_RCVBUF, &queueBytes, sizeof(queueBytes));

    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        error = "native preview hub is closed";
        ::close(sockets[0]);
        ::close(sockets[1]);
        return -1;
    }
    subscribers_.emplace(sockets[0], Subscriber{sockets[1], {}, 0, false});
    return sockets[0];
#endif
}

void NativePreviewHub::unsubscribe(int readFd) {
#if !defined(_WIN32)
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = subscribers_.find(readFd);
    if (found == subscribers_.end()) return;
    ::close(found->second.sendFd);
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
    // Production rate is measured before any socket backpressure or TCP writer.
    // This is the full MPEG-TS output rate, not the input HTTP bitrate.
    const auto rateNow = std::chrono::steady_clock::now();
    if (lastRateSample_ == std::chrono::steady_clock::time_point{})
        lastRateSample_ = rateNow;
    publishedBytes_ += alignedSize;
    ++publishedCalls_;
    const auto rateMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        rateNow - lastRateSample_).count();
    if (rateMs >= 5000) {
        if (!subscribers_.empty()) {
            std::size_t largestQueue = 0;
            for (const auto& entry : subscribers_)
                largestQueue = std::max(largestQueue, entry.second.pendingBytes);
            std::cerr << "NATIVE HTTP TS HUB PRODUCER"
                      << " output_kbps=" << (publishedBytes_ * 8ULL /
                          static_cast<std::uint64_t>(rateMs))
                      << " publish_calls=" << publishedCalls_
                      << " subscriber_count=" << subscribers_.size()
                      << " max_pending_bytes=" << largestQueue
                      << std::endl;
        }
        lastRateSample_ = rateNow;
        publishedBytes_ = 0;
        publishedCalls_ = 0;
    }
    for (auto subscriber = subscribers_.begin();
         subscriber != subscribers_.end();) {
        auto& client = subscriber->second;
        bool remove = false;
        int sendFailureErrno = 0;
        const char* disconnectReason = "unknown";

        // Every SO(K)_SEQPACKET send is a complete, TS-aligned datagram.
        // A full nonblocking socket must not silently discard the rest of a
        // transport chunk: that loses PES/IDR data and freezes HTTP MPEG-TS.
        const auto sendChunk = [&](const std::uint8_t* bytes,
                                   std::size_t count) -> int {
            const ssize_t sent = ::send(client.sendFd, bytes, count,
                                        MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent == static_cast<ssize_t>(count)) return 0;
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                             errno == ENOBUFS)) return 1;
            sendFailureErrno = sent < 0 ? errno : 0;
            disconnectReason = sent < 0 ? "socket_send_error" : "short_seqpacket_send";
            return -1;
        };

        // Drain queued data first to preserve MPEG-TS packet order.
        for (; !client.pending.empty();) {
            const auto& packet = client.pending.front();
            const int result = sendChunk(packet.data(), packet.size());
            if (result == 1) break;
            if (result < 0) { remove = true; break; }
            client.pendingBytes -= packet.size();
            client.pending.pop_front();
        }

        if (client.pendingBytes < (1U * 1024U * 1024U))
            client.backlogWarningLogged = false;
        if (client.pendingBytes >= (2U * 1024U * 1024U) &&
            !client.backlogWarningLogged) {
            std::cerr << "NATIVE HTTP TS HUB backlog"
                      << " hub_fd=" << subscriber->first
                      << " pending_bytes=" << client.pendingBytes
                      << std::endl;
            client.backlogWarningLogged = true;
        }

        for (std::size_t offset = 0; !remove && offset < alignedSize;) {
            const std::size_t chunk =
                std::min(kPreviewDatagramBytes, alignedSize - offset);
            if (client.pending.empty()) {
                const int result = sendChunk(data + offset, chunk);
                if (result == 0) {
                    offset += chunk;
                    continue;
                }
                if (result < 0) { remove = true; break; }
            }
            // The consumer is temporarily behind. Buffer in order rather
            // than silently skipping payload bytes. On sustained overflow,
            // close only that subscriber so it can reconnect at a new IDR;
            // do not disrupt other clients or the production transcoder.
            if (client.pendingBytes + chunk > kMaxPendingBytes) {
                std::cerr << "NATIVE HTTP TS subscriber backlog limit"
                          << " fd=" << subscriber->first
                          << " pending_bytes=" << client.pendingBytes
                          << " action=disconnect" << std::endl;
                disconnectReason = "pending_limit";
                remove = true;
                break;
            }
            client.pending.emplace_back(data + offset, data + offset + chunk);
            client.pendingBytes += chunk;
            offset += chunk;
        }

        if (remove) {
            std::cerr << "NATIVE HTTP TS HUB detach"
                      << " hub_fd=" << subscriber->first
                      << " reason=" << disconnectReason
                      << " errno=" << sendFailureErrno
                      << " pending_bytes=" << client.pendingBytes
                      << std::endl;
            ::close(client.sendFd);
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

// V337: published bytes can remain queued in this hub when the encoded
// producer pauses. The HTTP writer drives an additional nonblocking drain
// after consuming a datagram. This never waits for TCP, copies no new input
// and preserves packet order within this subscriber.
void NativePreviewHub::flushPending(int readFd) {
#if !defined(_WIN32)
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    auto it = subscribers_.find(readFd);
    if (it == subscribers_.end()) return;
    auto& client = it->second;
    while (!client.pending.empty()) {
        const auto& packet = client.pending.front();
        const ssize_t sent = ::send(client.sendFd, packet.data(), packet.size(),
                                    MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                         errno == ENOBUFS)) break;
        if (sent != static_cast<ssize_t>(packet.size())) {
            const int failedErrno = sent < 0 ? errno : 0;
            std::cerr << "NATIVE HTTP TS HUB detach"
                      << " hub_fd=" << readFd
                      << " reason=pending_flush_error"
                      << " errno=" << failedErrno
                      << " pending_bytes=" << client.pendingBytes
                      << std::endl;
            ::close(client.sendFd);
            subscribers_.erase(it);
            return;
        }
        client.pendingBytes -= packet.size();
        client.pending.pop_front();
    }
    if (client.pendingBytes < (1U * 1024U * 1024U))
        client.backlogWarningLogged = false;
#else
    (void)readFd;
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
        ::close(subscriber.second.sendFd);
    }
    subscribers_.clear();
#endif
}

} // namespace dvbstreamer5::media::network
