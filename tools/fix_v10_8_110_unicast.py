from pathlib import Path

p = Path("src/UdpInputFirewall.h")
s = p.read_text()
old = '''            AllowedEndpoint allowed;
            allowed.interfaceName = iface.name;
            allowed.destination = parsed.host;
            allowed.port = parsed.port;
            allowed.wildcard = parsed.wildcard;
            endpoints.push_back(std::move(allowed));'''
new = '''            AllowedEndpoint allowed;
            allowed.interfaceName = iface.name;
            // NativeUdpRelay binds unicast receivers to 0.0.0.0:port and uses
            // URI host as a socket group only for multicast. For a concrete
            // unicast URI, filter on the selected local interface address,
            // not on the URI host (which NativeUdpRelay does not bind to).
            allowed.destination = parsed.multicast ? parsed.host : iface.address;
            allowed.port = parsed.port;
            allowed.wildcard = parsed.wildcard;
            endpoints.push_back(std::move(allowed));'''
if s.count(old) != 1:
    raise SystemExit(f"unicast endpoint mapping: expected 1 occurrence, got {s.count(old)}")
s = s.replace(old, new, 1)

old = '''        for (const auto& iface : enabled) {
            const std::string quoted = nftQuote(iface.name);
            if (!srtPorts.empty()) {'''
new = '''        for (const auto& iface : enabled) {
            const std::string quoted = nftQuote(iface.name);
            // Never break replies for existing UDP sessions (SRT caller,
            // DNS, WireGuard, etc.) even if an ephemeral port happens to
            // overlap a configured media input port.
            out << "    iifname " << quoted
                << " ct state established,related accept comment \\"DVBStreamer5 preserve established UDP\\"\\n";
            if (!srtPorts.empty()) {'''
if s.count(old) != 1:
    raise SystemExit(f"established UDP exemption: expected 1 occurrence, got {s.count(old)}")
s = s.replace(old, new, 1)
p.write_text(s)
