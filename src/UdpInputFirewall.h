#pragma once

#include "ConfigManager.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <iostream>
#include <map>
#include <net/if.h>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <utility>
#include <vector>

namespace dvbstreamer5::network {

// V10.8.110: optional per-interface ingress protection for UDP/RTP transport
// streams.  The implementation owns one nftables table only and never flushes
// or rewrites the host firewall.  A failed rebuild removes our table and leaves
// the host fail-open rather than risking loss of management/SRT connectivity.
class UdpInputFirewall {
public:
    static bool enabledFor(const AppConfig& config, const std::string& address) {
        return std::find(config.udpInputFilterInterfaces.begin(),
                         config.udpInputFilterInterfaces.end(), address) !=
               config.udpInputFilterInterfaces.end();
    }

    static bool apply(const AppConfig& config, std::string& error) {
        error.clear();
        const auto systemInterfaces = ipv4Interfaces();
        std::vector<InterfaceInfo> enabled;
        std::set<std::string> seenAddresses;
        for (const auto& address : config.udpInputFilterInterfaces) {
            if (address.empty() || !seenAddresses.insert(address).second) continue;
            const auto found = std::find_if(systemInterfaces.begin(), systemInterfaces.end(),
                [&address](const InterfaceInfo& iface) {
                    return iface.address == address && !iface.loopback;
                });
            if (found == systemInterfaces.end()) {
                error = "configured UDP input filter interface is unavailable: " + address;
                failOpenCleanup();
                return false;
            }
            enabled.push_back(*found);
        }

        // With filtering disabled there must be no DVBStreamer5 nftables table.
        if (enabled.empty()) {
            if (nftAvailable()) deleteOwnedTable();
            return true;
        }
        if (!nftAvailable()) {
            error = "nft command is unavailable; install nftables";
            failOpenCleanup();
            return false;
        }

        std::set<int> mediaPorts;
        std::set<int> srtPorts;
        std::vector<AllowedEndpoint> endpoints;
        for (const auto& stream : config.streams) {
            collectUdpInput(stream.inputUri, stream, enabled, mediaPorts, endpoints);
            collectUdpInput(stream.backupInputUri, stream, enabled, mediaPorts, endpoints);
            collectSrtUriPort(stream.inputUri, srtPorts);
            collectSrtUriPort(stream.backupInputUri, srtPorts);
            if (lower(stream.outputType) == "srt" && validPort(stream.outputPort)) {
                srtPorts.insert(stream.outputPort);
            }
            for (const auto& output : stream.additionalOutputs) {
                if (lower(output.outputType) == "srt" && validPort(output.outputPort)) {
                    srtPorts.insert(output.outputPort);
                }
            }
        }

        const std::string script = buildScript("dvbstreamer5_udp_input", enabled,
                                               mediaPorts, srtPorts, endpoints);
        const std::string checkScript = buildScript("dvbstreamer5_udp_input_check", enabled,
                                                    mediaPorts, srtPorts, endpoints);
        if (!runNft("nft -c -f - >/dev/null 2>&1", checkScript)) {
            error = "nftables validation failed";
            failOpenCleanup();
            return false;
        }

        // Delete only our previous table. nft -f is transactional, therefore a
        // malformed/new ruleset cannot leave a half-installed DROP chain.
        deleteOwnedTable();
        if (!runNft("nft -f - >/dev/null 2>&1", script)) {
            error = "nftables apply failed";
            failOpenCleanup();
            return false;
        }

        std::cerr << "UDP INPUT FILTER applied interfaces=" << enabled.size()
                  << " media_ports=" << mediaPorts.size()
                  << " allowed_endpoints=" << endpoints.size()
                  << " srt_exempt_ports=" << srtPorts.size() << std::endl;
        return true;
    }

private:
    struct InterfaceInfo {
        std::string name;
        std::string address;
        bool loopback = false;
    };

    struct ParsedUdp {
        std::string host;
        int port = 0;
        bool multicast = false;
        bool wildcard = false;
    };

    struct AllowedEndpoint {
        std::string interfaceName;
        std::string destination;
        int port = 0;
        bool wildcard = false;
    };

    static bool validPort(int port) { return port > 0 && port <= 65535; }

    static std::string lower(std::string value) {
        for (char& ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    static std::string cleanInterface(std::string value) {
        if (value == "auto" || value == "Auto" || value == "0.0.0.0") return {};
        return value;
    }

    static bool isIpv4(const std::string& value) {
        in_addr address{};
        return !value.empty() && ::inet_pton(AF_INET, value.c_str(), &address) == 1;
    }

    static bool isMulticast(const std::string& value) {
        in_addr address{};
        if (::inet_pton(AF_INET, value.c_str(), &address) != 1) return false;
        const std::uint32_t host = ntohl(address.s_addr);
        const unsigned first = (host >> 24U) & 0xFFU;
        return first >= 224U && first <= 239U;
    }

    static std::vector<InterfaceInfo> ipv4Interfaces() {
        std::vector<InterfaceInfo> out;
        ifaddrs* addresses = nullptr;
        if (::getifaddrs(&addresses) != 0 || !addresses) return out;
        for (ifaddrs* item = addresses; item; item = item->ifa_next) {
            if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET || !item->ifa_name) continue;
            char text[INET_ADDRSTRLEN]{};
            const auto* in = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
            if (!::inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text))) continue;
            InterfaceInfo info;
            info.name = item->ifa_name;
            info.address = text;
            info.loopback = (item->ifa_flags & IFF_LOOPBACK) != 0 || info.name == "lo" ||
                            info.address.rfind("127.", 0) == 0;
            const auto duplicate = std::find_if(out.begin(), out.end(), [&info](const InterfaceInfo& existing) {
                return existing.name == info.name && existing.address == info.address;
            });
            if (duplicate == out.end()) out.push_back(std::move(info));
        }
        ::freeifaddrs(addresses);
        return out;
    }

    static bool parseUdpInput(const std::string& uri, ParsedUdp& parsed) {
        static const std::regex pattern(
            R"(^(udp|rtp)://@?([^:/]*):([0-9]+)(?:[/?#].*)?$)",
            std::regex::icase);
        std::smatch match;
        if (!std::regex_match(uri, match, pattern)) return false;
        int port = 0;
        try { port = std::stoi(match[3].str()); } catch (...) { return false; }
        if (!validPort(port)) return false;
        parsed.host = match[2].str();
        parsed.port = port;
        parsed.wildcard = parsed.host.empty() || parsed.host == "0.0.0.0";
        parsed.multicast = isMulticast(parsed.host);
        if (!parsed.wildcard && !isIpv4(parsed.host)) return false;
        return true;
    }

    static void collectUdpInput(const std::string& uri, const StreamConfig& stream,
                                const std::vector<InterfaceInfo>& enabled,
                                std::set<int>& mediaPorts,
                                std::vector<AllowedEndpoint>& endpoints) {
        ParsedUdp parsed;
        if (!parseUdpInput(uri, parsed)) return;
        mediaPorts.insert(parsed.port);

        // Match NativeUdpRelay's interface selection semantics exactly.
        std::string selectedInterface;
        if (stream.inputInterfaceAddressConfigured) {
            selectedInterface = cleanInterface(stream.inputInterfaceAddress);
        } else if (parsed.multicast || parsed.wildcard) {
            selectedInterface = cleanInterface(stream.interfaceAddress);
        }

        for (const auto& iface : enabled) {
            if (!selectedInterface.empty() && selectedInterface != iface.address) continue;
            AllowedEndpoint allowed;
            allowed.interfaceName = iface.name;
            allowed.destination = parsed.host;
            allowed.port = parsed.port;
            allowed.wildcard = parsed.wildcard;
            endpoints.push_back(std::move(allowed));
        }
    }

    static void collectSrtUriPort(const std::string& uri, std::set<int>& ports) {
        if (lower(uri).rfind("srt://", 0) != 0) return;
        std::string authority = uri.substr(6);
        const auto query = authority.find('?');
        if (query != std::string::npos) authority.resize(query);
        std::string portText;
        if (!authority.empty() && authority.front() == '[') {
            const auto close = authority.find(']');
            if (close == std::string::npos || close + 1 >= authority.size() || authority[close + 1] != ':') return;
            portText = authority.substr(close + 2);
        } else {
            const auto colon = authority.rfind(':');
            if (colon == std::string::npos) return;
            portText = authority.substr(colon + 1);
        }
        try {
            const int port = std::stoi(portText);
            if (validPort(port)) ports.insert(port);
        } catch (...) {}
    }

    static std::string nftQuote(const std::string& value) {
        std::string out = "\"";
        for (char ch : value) {
            if (ch == '\\' || ch == '"') out.push_back('\\');
            out.push_back(ch);
        }
        out.push_back('"');
        return out;
    }

    static std::string portSet(const std::set<int>& ports) {
        std::ostringstream out;
        out << "{ ";
        bool first = true;
        for (int port : ports) {
            if (!first) out << ", ";
            first = false;
            out << port;
        }
        out << " }";
        return out.str();
    }

    static std::string buildScript(const std::string& tableName,
                                   const std::vector<InterfaceInfo>& enabled,
                                   const std::set<int>& mediaPorts,
                                   const std::set<int>& srtPorts,
                                   const std::vector<AllowedEndpoint>& endpoints) {
        std::ostringstream out;
        out << "table inet " << tableName << " {\n"
            << "  chain input {\n"
            << "    type filter hook input priority -10; policy accept;\n";

        for (const auto& iface : enabled) {
            const std::string quoted = nftQuote(iface.name);
            if (!srtPorts.empty()) {
                out << "    iifname " << quoted << " udp dport " << portSet(srtPorts)
                    << " accept comment \"DVBStreamer5 preserve SRT\"\n";
            }
            for (const auto& endpoint : endpoints) {
                if (endpoint.interfaceName != iface.name) continue;
                out << "    iifname " << quoted << ' ';
                if (!endpoint.wildcard) out << "ip daddr " << endpoint.destination << ' ';
                out << "udp dport " << endpoint.port
                    << " accept comment \"DVBStreamer5 configured UDP/RTP input\"\n";
            }
            // Protect the selected media interface from unrelated multicast
            // storms while leaving all non-media unicast UDP services alone.
            out << "    iifname " << quoted
                << " ip daddr 224.0.0.0/4 udp drop comment \"DVBStreamer5 reject unconfigured multicast\"\n";
            if (!mediaPorts.empty()) {
                out << "    iifname " << quoted << " udp dport " << portSet(mediaPorts)
                    << " drop comment \"DVBStreamer5 reject wrong media endpoint\"\n";
            }
        }
        out << "  }\n}\n";
        return out.str();
    }

    static bool nftAvailable() {
        return std::system("command -v nft >/dev/null 2>&1") == 0;
    }

    static bool runNft(const char* command, const std::string& script) {
        FILE* pipe = ::popen(command, "w");
        if (!pipe) return false;
        const std::size_t written = std::fwrite(script.data(), 1, script.size(), pipe);
        const int status = ::pclose(pipe);
        return written == script.size() && status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    static void deleteOwnedTable() {
        (void)std::system("nft delete table inet dvbstreamer5_udp_input >/dev/null 2>&1");
    }

    static void failOpenCleanup() {
        if (nftAvailable()) deleteOwnedTable();
    }
};

} // namespace dvbstreamer5::network
