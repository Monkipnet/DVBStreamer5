from pathlib import Path

version = Path('src/AppVersion.h')
text = version.read_text()
old = 'inline constexpr const char* kProgramVersion = "10.8.161";'
new = 'inline constexpr const char* kProgramVersion = "10.8.162";'
assert old in text
version.write_text(text.replace(old, new, 1))

p = Path('src/media/CbrTsPacer.cpp')
text = p.read_text()
old = '''    const auto now = std::chrono::steady_clock::now();
    observePayloadPacket(now);
    if (tvStreammerSat5Profile()) {
        observeTvStreammerSat5Arrival(packet, now);
    }
'''
new = '''    const auto now = std::chrono::steady_clock::now();
    if (tvStreammerSat5Profile()) {
        // SAT5 continuous-network profile owns its useful-packet clock via the
        // long-term arrival estimator + slow reservoir PLL. Do not let the
        // legacy 4-second DVBStreamer5 raise-only transport auto-tune change the
        // configured CBR rate underneath that clock.
        observeTvStreammerSat5Arrival(packet, now);
    } else {
        observePayloadPacket(now);
    }
'''
assert old in text
text = text.replace(old, new, 1)
old = '''                  << " datagram_bytes="
                  << (kPacketsPerCbrDatagram * kPacketSize)
                  << std::endl;
'''
new = '''                  << " datagram_bytes="
                  << (kPacketsPerCbrDatagram * kPacketSize)
                  << " transport_auto_tune=off"
                  << std::endl;
'''
assert old in text
p.write_text(text.replace(old, new, 1))
