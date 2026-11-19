#include "EpgUdpOutput.h"
#include "DvbEitOutput.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using Packet = dvbstreamer5::media::mpegts::Packet;
constexpr std::size_t kPacketsPerDatagram = 7;
constexpr auto kWakeInterval = std::chrono::milliseconds(200);

bool ipv4(const std::string& ip, in_addr& out) {
    return !ip.empty() && inet_pton(AF_INET, ip.c_str(), &out) == 1;
}
bool destination(const std::string& ip) {
    in_addr addr{};
    if (!ipv4(ip, addr)) return false;
    const auto n = ntohl(addr.s_addr);
    // No wildcard, 0/8, loopback is allowed for local test, 224/4 multicast
    // is allowed for WISI. Broadcast and 240/4 are rejected.
    return n != 0 && (n & 0xff000000U) != 0 &&
           (n & 0xf0000000U) != 0xf0000000U &&
           n != 0xffffffffU && (n & 0xff000000U) != 0x7f000000U
           ? true : (n & 0xff000000U) == 0x7f000000U;
}
Packet nullPacket() {
    Packet out{};
    out.fill(0xffU);
    out[0] = 0x47U; out[1] = 0x1fU; out[2] = 0xffU; out[3] = 0x10U;
    return out;
}
struct Target {
    int fd = -1;
    sockaddr_in address{};
    std::vector<std::unique_ptr<DvbEitOutput>> generators;
    std::uint8_t continuity = 0;
    ~Target() { if (fd >= 0) close(fd); }
    Target() = default;
    Target(const Target&) = delete;
    Target& operator=(const Target&) = delete;
};
}
EpgUdpOutputManager::EpgUdpOutputManager(Snapshot snapshot)
    : snapshot_(std::move(snapshot)) {}
EpgUdpOutputManager::~EpgUdpOutputManager() { configure({}); }
bool EpgUdpOutputManager::validDestination(const std::string& ip) {
    return destination(ip);
}
bool EpgUdpOutputManager::validInterface(const std::string& ip) {
    in_addr addr{}; return ipv4(ip, addr);
}
void EpgUdpOutputManager::configure(const std::vector<EpgUdpOutputConfig>& outputs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outputs_ = outputs;
        stop_ = false;
    }
    bool active = false;
    for (const auto& output : outputs) if (output.enabled) active = true;
    if (active) thread_ = std::thread([this, outputs] { run(outputs); });
}
Json::Value EpgUdpOutputManager::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value result(Json::objectValue), outputs(Json::arrayValue);
    for (const auto& output : outputs_) outputs.append(output.toJson());
    result["outputs"] = outputs;
    return result;
}
void EpgUdpOutputManager::run(std::vector<EpgUdpOutputConfig> outputs) {
    std::vector<std::unique_ptr<Target>> targets;
    for (const auto& cfg : outputs) {
        if (!cfg.enabled || !validDestination(cfg.outputHost) ||
            cfg.services.empty() || cfg.outputPort <= 0) continue;
        auto target = std::make_unique<Target>();
        target->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (target->fd < 0) continue;
        target->address.sin_family = AF_INET;
        target->address.sin_port = htons(static_cast<uint16_t>(cfg.outputPort));
        inet_pton(AF_INET, cfg.outputHost.c_str(), &target->address.sin_addr);
        if (!cfg.interfaceAddress.empty()) {
            in_addr src{};
            if (!ipv4(cfg.interfaceAddress, src)) continue;
            if (setsockopt(target->fd, IPPROTO_IP, IP_MULTICAST_IF,
                           &src, sizeof(src)) != 0) {
                std::cerr << "EPG UDP invalid multicast interface output="
                          << cfg.id << " errno=" << errno << '\n';
                continue;
            }
        }
        for (const auto& service : cfg.services) {
            if (service.streamId.empty() && service.xmltvChannelId.empty()) continue;
            target->generators.push_back(std::make_unique<DvbEitOutput>(
                [this, service] { return snapshot_(service); },
                static_cast<uint16_t>(service.serviceId),
                static_cast<uint16_t>(service.transportStreamId),
                static_cast<uint16_t>(service.originalNetworkId),
                cfg.mode == "barker"));
        }
        if (!target->generators.empty()) {
            std::cerr << "EPG UDP enabled output=" << cfg.id
                      << " endpoint=" << cfg.outputHost << ":" << cfg.outputPort
                      << " mode=" << cfg.mode
                      << " services=" << target->generators.size() << '\n';
            targets.push_back(std::move(target));
        }
    }
    std::array<std::uint8_t,kPacketsPerDatagram * 188> datagram{};
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (wake_.wait_for(lock, kWakeInterval, [this] { return stop_; })) break;
        }
        for (auto& target : targets) {
            std::vector<Packet> packets;
            for (auto& generator : target->generators) {
                // Each generator owns PID 0x12 in its input vector. Do not
                // pass the previous service's packets into the next generator,
                // or its EIT filter would remove all earlier services.
                std::vector<Packet> servicePackets;
                generator->process(servicePackets);
                packets.insert(packets.end(),
                               servicePackets.begin(), servicePackets.end());
            }
            for (std::size_t begin=0; begin<packets.size(); begin+=kPacketsPerDatagram) {
                for (std::size_t i=0; i<kPacketsPerDatagram; ++i) {
                    Packet packet = begin + i < packets.size()
                        ? packets[begin + i] : nullPacket();
                    if (begin + i < packets.size())
                        packet[3] = static_cast<std::uint8_t>(
                            0x10U | (target->continuity++ & 0x0fU));
                    std::memcpy(datagram.data()+188*i, packet.data(), 188);
                }
                const ssize_t sent=sendto(target->fd,datagram.data(),datagram.size(),
                    MSG_DONTWAIT,reinterpret_cast<const sockaddr*>(&target->address),
                    sizeof(target->address));
                if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    // Do not spam logs on persistent send failures.
                    static unsigned errors = 0;
                    if (++errors % 500U == 1U)
                        std::cerr << "EPG UDP send failure errno=" << errno << '\n';
                }
            }
        }
    }
}
