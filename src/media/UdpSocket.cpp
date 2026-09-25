#include "media/UdpSocket.h"

#include <cstring>
#include <limits>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__) && !defined(IP_MULTICAST_ALL)
#define IP_MULTICAST_ALL 49
#endif
#endif

namespace tvs::media::network {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

bool ensureSocketRuntime(std::string& error) {
    static std::once_flag initialized;
    static int startupResult = 0;
    std::call_once(initialized, [] {
        WSADATA data {};
        startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (startupResult != 0) {
        error = "Winsock initialization failed: " + std::to_string(startupResult);
        return false;
    }
    return true;
}

int lastSocketError() noexcept {
    return WSAGetLastError();
}

void closeSocket(NativeSocket socket) noexcept {
    ::closesocket(socket);
}

int setSocketOption(
    NativeSocket socket, int level, int option, const void* value, int size) noexcept {
    return ::setsockopt(
        socket, level, option, static_cast<const char*>(value), size);
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

bool ensureSocketRuntime(std::string&) noexcept {
    return true;
}

int lastSocketError() noexcept {
    return errno;
}

void closeSocket(NativeSocket socket) noexcept {
    ::close(socket);
}

int setSocketOption(
    NativeSocket socket, int level, int option, const void* value, socklen_t size) noexcept {
    return ::setsockopt(socket, level, option, value, size);
}
#endif

void setSocketError(std::string& error, const char* operation) {
    error = std::string(operation) + " failed (socket error " +
        std::to_string(lastSocketError()) + ")";
}

bool parseIpv4(const std::string& address, in_addr& result) noexcept {
    return ::inet_pton(AF_INET, address.c_str(), &result) == 1;
}

} // namespace

struct UdpSocket::Destination {
    sockaddr_in address {};
};

UdpSocket::UdpSocket() noexcept = default;

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::openReceiver(
    const std::string& bindAddress,
    std::uint16_t port,
    const std::string& multicastGroup,
    const std::string& interfaceAddress,
    int receiveBufferBytes,
    std::string& error) {
    close();
    if (!ensureSocketRuntime(error)) {
        return false;
    }
    if (receiveBufferBytes < 0 || (multicastGroup.empty() && !interfaceAddress.empty())) {
        error = "invalid UDP receiver buffer or interface settings";
        return false;
    }

    in_addr localAddress {};
    if (!parseIpv4(bindAddress, localAddress)) {
        error = "bind address must be a numeric IPv4 address";
        return false;
    }
    if (!interfaceAddress.empty()) {
        in_addr parsedInterface {};
        if (!parseIpv4(interfaceAddress, parsedInterface)) {
            error = "interface address must be a numeric IPv4 address";
            return false;
        }
    }

    NativeSocket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket) {
        setSocketError(error, "UDP receiver socket");
        return false;
    }

    const int reuseAddress = 1;
    if (setSocketOption(socket, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress)) != 0) {
        setSocketError(error, "SO_REUSEADDR");
        closeSocket(socket);
        return false;
    }
    if (receiveBufferBytes > 0 &&
        setSocketOption(
            socket, SOL_SOCKET, SO_RCVBUF, &receiveBufferBytes, sizeof(receiveBufferBytes)) != 0) {
        setSocketError(error, "SO_RCVBUF");
        closeSocket(socket);
        return false;
    }

    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr = localAddress;
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        setSocketError(error, "UDP receiver bind");
        closeSocket(socket);
        return false;
    }

    if (!multicastGroup.empty()) {
#if defined(__linux__) && defined(IP_MULTICAST_ALL)
        const int multicastAll = 0;
        if (setSocketOption(
                socket, IPPROTO_IP, IP_MULTICAST_ALL,
                &multicastAll, sizeof(multicastAll)) != 0) {
            setSocketError(error, "IP_MULTICAST_ALL");
            closeSocket(socket);
            return false;
        }
#endif
        ip_mreq membership {};
        if (!parseIpv4(multicastGroup, membership.imr_multiaddr) ||
            !IN_MULTICAST(ntohl(membership.imr_multiaddr.s_addr))) {
            error = "multicast group must be a numeric IPv4 multicast address";
            closeSocket(socket);
            return false;
        }
        if (interfaceAddress.empty()) {
            membership.imr_interface.s_addr = htonl(INADDR_ANY);
        } else if (!parseIpv4(interfaceAddress, membership.imr_interface)) {
            error = "interface address must be a numeric IPv4 address";
            closeSocket(socket);
            return false;
        }
        if (setSocketOption(
                socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) {
            setSocketError(error, "IP_ADD_MEMBERSHIP");
            closeSocket(socket);
            return false;
        }
    }

    sockaddr_in bound {};
#ifdef _WIN32
    int boundSize = sizeof(bound);
#else
    socklen_t boundSize = sizeof(bound);
#endif
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &boundSize) != 0) {
        setSocketError(error, "UDP receiver getsockname");
        closeSocket(socket);
        return false;
    }

    socket_ = static_cast<NativeSocket>(socket);
    localPort_ = ntohs(bound.sin_port);
    receiver_ = true;
    error.clear();
    return true;
}

bool UdpSocket::openSender(
    const std::string& host,
    std::uint16_t port,
    const std::string& interfaceAddress,
    std::string& error) {
    close();
    if (!ensureSocketRuntime(error)) {
        return false;
    }
    if (port == 0) {
        error = "UDP destination port must be non-zero";
        return false;
    }

    in_addr localInterface {};
    if (!interfaceAddress.empty() && !parseIpv4(interfaceAddress, localInterface)) {
        error = "interface address must be a numeric IPv4 address";
        return false;
    }

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* resolvedAddresses = nullptr;
    const std::string service = std::to_string(port);
    const int lookupResult =
        ::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolvedAddresses);
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(
        resolvedAddresses, &::freeaddrinfo);
    if (lookupResult != 0 || !addresses) {
#ifdef _WIN32
        error = "UDP destination lookup failed (socket error " +
            std::to_string(lookupResult) + ")";
#else
        error = std::string("UDP destination lookup failed: ") +
            ::gai_strerror(lookupResult);
#endif
        return false;
    }
    if (addresses->ai_addrlen < sizeof(sockaddr_in)) {
        error = "UDP destination lookup returned an invalid IPv4 address";
        return false;
    }

    auto destination = std::make_unique<Destination>();
    std::memcpy(&destination->address, addresses->ai_addr, sizeof(destination->address));

    NativeSocket socket = ::socket(
        addresses->ai_family, addresses->ai_socktype, addresses->ai_protocol);
    if (socket == kInvalidSocket) {
        setSocketError(error, "UDP sender socket");
        return false;
    }

    if (!interfaceAddress.empty()) {
        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr = localInterface;
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            setSocketError(error, "UDP sender bind");
            closeSocket(socket);
            return false;
        }
        if (IN_MULTICAST(ntohl(destination->address.sin_addr.s_addr)) &&
            setSocketOption(
                socket, IPPROTO_IP, IP_MULTICAST_IF, &localInterface, sizeof(localInterface)) != 0) {
            setSocketError(error, "IP_MULTICAST_IF");
            closeSocket(socket);
            return false;
        }
    }

    destination_ = std::move(destination);
    socket_ = static_cast<NativeSocket>(socket);
    receiver_ = false;
    error.clear();
    return true;
}

bool UdpSocket::receive(
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t& received,
    int timeoutMs,
    std::string& error) {
    received = 0;
    if (socket_ == kInvalidSocket || !receiver_ || !buffer || capacity == 0 || timeoutMs < 0) {
        error = "UDP receiver is not open or receive arguments are invalid";
        return false;
    }
    if (capacity > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        error = "UDP receive buffer exceeds the supported size";
        return false;
    }

    fd_set readable;
    FD_ZERO(&readable);
    const NativeSocket socket = static_cast<NativeSocket>(socket_);
    FD_SET(socket, &readable);
    timeval timeout {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
    const int ready = ::select(socket + 1, &readable, nullptr, nullptr, &timeout);
#endif
    if (ready == 0) {
        error.clear();
        return true;
    }
    if (ready < 0) {
        setSocketError(error, "UDP receiver select");
        return false;
    }

#ifdef _WIN32
    const int count = ::recvfrom(
        socket,
        reinterpret_cast<char*>(buffer),
        static_cast<int>(capacity),
        0,
        nullptr,
        nullptr);
#else
    const ssize_t count = ::recvfrom(socket, buffer, capacity, 0, nullptr, nullptr);
#endif
    if (count < 0) {
        setSocketError(error, "UDP receive");
        return false;
    }
    received = static_cast<std::size_t>(count);
    error.clear();
    return true;
}

bool UdpSocket::send(const std::uint8_t* data, std::size_t size, std::string& error) {
    if (socket_ == kInvalidSocket || receiver_ || !data || size == 0 ||
        size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        error = "UDP sender is not open or send arguments are invalid";
        return false;
    }

    const NativeSocket socket = static_cast<NativeSocket>(socket_);
#ifdef _WIN32
    const int count = ::sendto(
        socket,
        reinterpret_cast<const char*>(data),
        static_cast<int>(size),
        0,
        reinterpret_cast<const sockaddr*>(&destination_->address),
        sizeof(destination_->address));
#else
    const ssize_t count = ::sendto(
        socket,
        data,
        size,
        0,
        reinterpret_cast<const sockaddr*>(&destination_->address),
        sizeof(destination_->address));
#endif
    if (count < 0 || static_cast<std::size_t>(count) != size) {
        setSocketError(error, "UDP send");
        return false;
    }
    error.clear();
    return true;
}

std::uint16_t UdpSocket::localPort() const noexcept {
    return localPort_;
}

void UdpSocket::close() noexcept {
    if (socket_ != kInvalidSocket) {
        closeSocket(static_cast<NativeSocket>(socket_));
        socket_ = kInvalidSocket;
    }
    destination_.reset();
    localPort_ = 0;
    receiver_ = false;
}

} // namespace tvs::media::network
