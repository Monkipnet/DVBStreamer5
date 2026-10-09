from pathlib import Path

version = Path('src/AppVersion.h')
text = version.read_text()
old = 'inline constexpr const char* kProgramVersion = "10.8.161";'
new = 'inline constexpr const char* kProgramVersion = "10.8.162";'
assert old in text
version.write_text(text.replace(old, new, 1))

p = Path('src/media/CbrTsPacer.cpp')
text = p.read_text()
text = text.replace(
'''// shaper. TvStreammerSat5Network ports the stable continuous SRT/HTTP strategy
// from TVStreammerSAT5 StableUdpOutput: a real jitter reservoir, uniform useful
// packet token pacing, strict 7x188 CBR slots, NULL stuffing and a 20 ms PCR clock.
''',
'''// shaper. TvStreammerSat5Network ports the stable continuous SRT/HTTP strategy
// from TVStreammerSAT5 StableUdpOutput: a real jitter reservoir, uniform useful
// packet token pacing, strict 7x188 CBR slots, NULL stuffing and source-PCR passthrough.
''', 1)

old = '''    const auto now = std::chrono::steady_clock::now();
    observePayloadPacket(now);
    if (tvStreammerSat5Profile()) {
        observeTvStreammerSat5Arrival(packet, now);
    }
'''
new = '''    const auto now = std::chrono::steady_clock::now();
    if (tvStreammerSat5Profile()) {
        // SAT5 continuous-network profile owns its useful-packet clock via the
        // long-term arrival estimator + slow reservoir PLL. The configured
        // transport CBR must stay fixed; legacy 4-second auto-tune is disabled.
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
                  << " source_pcr=passthrough"
                  << std::endl;
'''
assert old in text
text = text.replace(old, new, 1)

old = '''        // Accumulate useful-data entitlement on every transport slot, including
        // the slots occupied by synthetic PCR-only packets, as StableUdpOutput does.
        tvSatRealTokenAccumulator_ += tvSatRealPaceBitrate_;

        if (tvSatPcrInitialized_ && slotTime >= tvSatNextPeriodicPcrTime_) {
            makePeriodicPcrPacket(datagram[index], slotTime);
            do {
                tvSatNextPeriodicPcrTime_ += kTvSatPeriodicPcrInterval;
            } while (tvSatNextPeriodicPcrTime_ <= slotTime);
            continue;
        }
'''
new = '''        // Accumulate useful-data entitlement on every fixed CBR transport slot.
        // Continuous SRT/HTTP keeps provider PCR packets unchanged; NULL packets
        // fill only the unused transport capacity.
        tvSatRealTokenAccumulator_ += tvSatRealPaceBitrate_;
'''
assert old in text
text = text.replace(old, new, 1)

start = text.index('void CbrTsPacer::processTvStreammerSat5RealPacket(')
end = text.index('\nvoid CbrTsPacer::makePeriodicPcrPacket(', start)
new_func = '''void CbrTsPacer::processTvStreammerSat5RealPacket(
    Packet& packet,
    std::chrono::steady_clock::time_point) noexcept {
    PacketInfo info;
    if (!inspectPacket(packet.data(), packet.size(), info)) return;

    // TVStreammerSAT5 continuous SRT/HTTP mode preserves the provider PCR
    // domain. Do not rewrite or strip PCR and do not insert synthetic PCR-only
    // packets. PTS/DTS and PCR therefore remain in the same source timeline.
    if (!tvSatPcrInitialized_ && info.hasPcr) {
        tvSatPcrInitialized_ = true;
        tvSatPcrPid_ = info.pid;
        std::cerr << "CBR TVSTREAMMERSAT5 PCR lock"
                  << " pid=" << tvSatPcrPid_
                  << " mode=source-passthrough"
                  << " synthetic_pcr=off"
                  << std::endl;
    }
}
'''
text = text[:start] + new_func + text[end:]
p.write_text(text)
