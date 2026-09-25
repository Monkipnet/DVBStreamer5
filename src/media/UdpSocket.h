#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace tvs::media::network {

class UdpSocket {
public:
    UdpSocket() noexcept;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool openReceiver(
        const std::string& bindAddress,
        std::uint16_t port,
        const std::string& multicastGroup,
        const std::string& interfaceAddress,
        int receiveBufferBytes,
        std::string& error);
    bool openSender(
        const std::string& host,
        std::uint16_t port,
        const std::string& interfaceAddress,
        std::string& error);

    bool receive(
        std::uint8_t* buffer,
        std::size_t capacity,
        std::size_t& received,
        int timeoutMs,
        std::string& error);
    bool send(const std::uint8_t* data, std::size_t size, std::string& error);

    std::uint16_t localPort() const noexcept;
    void close() noexcept;

private:
    struct Destination;

#ifdef _WIN32
    using NativeSocket = std::uintptr_t;
    static constexpr NativeSocket kInvalidSocket = ~NativeSocket{0};
#else
    using NativeSocket = int;
    static constexpr NativeSocket kInvalidSocket = -1;
#endif

    NativeSocket socket_ = kInvalidSocket;
    std::unique_ptr<Destination> destination_;
    std::uint16_t localPort_ = 0;
    bool receiver_ = false;
};

} // namespace tvs::media::network
