#pragma once
#include <arpa/inet.h>
#include <cstdint>
#include <set>
#include <sstream>
#include <string>

namespace oscamnetwork {
inline bool isUnicastIPv4(const std::string& ip) {
    in_addr address{};
    if (inet_pton(AF_INET, ip.c_str(), &address) != 1) return false;
    const uint32_t n = ntohl(address.s_addr);
    const unsigned first = n >> 24;
    return first > 0 && first < 224 && n != 0xffffffffu;
}
inline bool validBind(const std::string& bind, const std::set<std::string>& addresses) {
    return bind == "127.0.0.1" || (isUnicastIPv4(bind) && addresses.count(bind) != 0);
}
// Only explicit IPv4 peers are accepted by the managed UI. No open ranges.
inline bool normalizeAllowed(const std::string& bind, const std::string& input, std::string& output) {
    output.clear();
    if (bind == "127.0.0.1") return input.empty();
    if (input.empty() || input.size() > 2048 || input.back() == ',') return false;
    std::set<std::string> peers;
    std::istringstream stream(input);
    std::string peer;
    while (std::getline(stream, peer, ',')) {
        const auto start = peer.find_first_not_of(" \t");
        if (start == std::string::npos) return false;
        peer = peer.substr(start, peer.find_last_not_of(" \t") - start + 1);
        if (!isUnicastIPv4(peer)) return false;
        peers.insert(peer);
    }
    if (peers.empty()) return false;
    peers.insert(bind); // auto-generated local CAM clients use this address
    for (const auto& ip : peers) {
        if (!output.empty()) output += ',';
        output += ip;
    }
    return true;
}
}
