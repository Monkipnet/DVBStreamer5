from pathlib import Path

p = Path("src/UdpInputFirewall.h")
s = p.read_text()
old = '                << " ip daddr 224.0.0.0/4 udp drop comment \\"DVBStreamer5 reject unconfigured multicast\\"\\n";'
new = '                << " ip daddr 224.0.0.0/4 meta l4proto udp drop comment \\"DVBStreamer5 reject unconfigured multicast\\"\\n";'
if s.count(old) != 1:
    raise SystemExit(f"multicast nft rule: expected 1 occurrence, got {s.count(old)}")
s = s.replace(old, new, 1)
if '#include <cstdint>\n' not in s:
    s = s.replace('#include <cstdio>\n', '#include <cstdint>\n#include <cstdio>\n', 1)
p.write_text(s)
