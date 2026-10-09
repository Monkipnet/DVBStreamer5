from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)

cpp_path = Path('src/media/CbrTsPacer.cpp')
h_path = Path('src/media/CbrTsPacer.h')
ver_path = Path('src/AppVersion.h')
cpp = cpp_path.read_text()
h = h_path.read_text()
ver = ver_path.read_text()

cpp = replace_once(
    cpp,
    '// packet token pacing, strict 7x188 CBR slots, NULL stuffing and source-PCR passthrough.\n',
    '// packet token pacing, strict 7x188 CBR slots, NULL stuffing and an independent\n// continuous 20 ms PCR media clock, matching StableUdpOutput.\n',
    'cpp banner')

cpp = replace_once(
    cpp,
    '''    const auto datagramTime = nextDeadline_;\n    if (tvStreammerSat5Profile()) {\n        updateTvStreammerSat5Controller(now);\n        fillTvStreammerSat5Datagram(datagramTime, datagram);\n    } else {\n''',
    '''    // SAT5 deliberately keeps media/PCR time independent from the physical\n    // sender deadline. A late scheduler wake may rebase nextDeadline_ above, but\n    // it must never jump PCR with it because upstream PTS/DTS did not jump.\n    const auto datagramTime = tvStreammerSat5Profile()\n        ? tvSatMediaTimeline_\n        : nextDeadline_;\n    if (tvStreammerSat5Profile()) {\n        updateTvStreammerSat5Controller(now);\n        fillTvStreammerSat5Datagram(datagramTime, datagram);\n    } else {\n''',
    'datagram media clock')

cpp = replace_once(
    cpp,
    '''    }\n    advanceDeadline();\n    return true;\n}\n\nbool CbrTsPacer::started() const noexcept {\n''',
    '''    }\n    if (tvStreammerSat5Profile()) {\n        advanceTvStreammerSat5MediaTimeline();\n    }\n    advanceDeadline();\n    return true;\n}\n\nbool CbrTsPacer::started() const noexcept {\n''',
    'media clock advance call')

cpp = replace_once(
    cpp,
    '''void CbrTsPacer::makeNullPacket(Packet& packet) noexcept {\n''',
    '''void CbrTsPacer::advanceTvStreammerSat5MediaTimeline() noexcept {\n    const std::uint64_t numerator = kDatagramBits * kNanosecondsPerSecond;\n    std::uint64_t nanoseconds = numerator / targetBitrate_;\n    tvSatMediaRemainder_ += numerator % targetBitrate_;\n    if (tvSatMediaRemainder_ >= targetBitrate_) {\n        nanoseconds += tvSatMediaRemainder_ / targetBitrate_;\n        tvSatMediaRemainder_ %= targetBitrate_;\n    }\n    tvSatMediaTimeline_ += std::chrono::nanoseconds(nanoseconds);\n}\n\nvoid CbrTsPacer::makeNullPacket(Packet& packet) noexcept {\n''',
    'media clock advance method')

cpp = replace_once(
    cpp,
    '''    started_ = true;\n    nextDeadline_ = now;\n    tvSatLastControllerUpdate_ = now;\n''',
    '''    started_ = true;\n    nextDeadline_ = now;\n    tvSatMediaTimeline_ = now;\n    tvSatMediaRemainder_ = 0;\n    tvSatLastControllerUpdate_ = now;\n''',
    'media clock init')

cpp = replace_once(
    cpp,
    '''                  << " transport_auto_tune=off"\n                  << " source_pcr=passthrough"\n                  << std::endl;\n''',
    '''                  << " transport_auto_tune=off"\n                  << " source_pcr=stripped-after-lock"\n                  << " synthetic_pcr=20ms"\n                  << " media_clock=independent"\n                  << std::endl;\n''',
    'start log')

old_fill = '''void CbrTsPacer::fillTvStreammerSat5Datagram(\n    std::chrono::steady_clock::time_point datagramTime,\n    CbrDatagram& datagram) noexcept {\n    for (std::size_t index = 0; index < datagram.size(); ++index) {\n        const auto slotTime = datagramTime + std::chrono::nanoseconds(\n            packetOffsetNanoseconds(index, targetBitrate_));\n\n        // Accumulate useful-data entitlement on every fixed CBR transport slot.\n        // Continuous SRT/HTTP keeps provider PCR packets unchanged; NULL packets\n        // fill only the unused transport capacity.\n        tvSatRealTokenAccumulator_ += tvSatRealPaceBitrate_;\n\n        bool sendReal = false;\n        if (targetBitrate_ > 0 && tvSatRealTokenAccumulator_ >= targetBitrate_) {\n            tvSatRealTokenAccumulator_ -= targetBitrate_;\n            sendReal = !queuedPackets_.empty();\n            if (!sendReal) {\n                // Never accumulate a catch-up burst during an upstream gap.\n                tvSatRealTokenAccumulator_ = (std::min)(\n                    tvSatRealTokenAccumulator_, targetBitrate_ - std::uint64_t{1});\n            }\n        }\n\n        if (sendReal) {\n            datagram[index] = queuedPackets_.front();\n            queuedPackets_.pop_front();\n            processTvStreammerSat5RealPacket(datagram[index], slotTime);\n        } else {\n            makeNullPacket(datagram[index]);\n        }\n    }\n}\n\nvoid CbrTsPacer::processTvStreammerSat5RealPacket(\n    Packet& packet,\n    std::chrono::steady_clock::time_point) noexcept {\n    PacketInfo info;\n    if (!inspectPacket(packet.data(), packet.size(), info)) return;\n\n    // TVStreammerSAT5 continuous SRT/HTTP mode preserves the provider PCR\n    // domain. Do not rewrite or strip PCR and do not insert synthetic PCR-only\n    // packets. PTS/DTS and PCR therefore remain in the same source timeline.\n    if (!tvSatPcrInitialized_ && info.hasPcr) {\n        tvSatPcrInitialized_ = true;\n        tvSatPcrPid_ = info.pid;\n        std::cerr << "CBR TVSTREAMMERSAT5 PCR lock"\n                  << " pid=" << tvSatPcrPid_\n                  << " mode=source-passthrough"\n                  << " synthetic_pcr=off"\n                  << std::endl;\n    }\n}\n'''
new_fill = '''void CbrTsPacer::fillTvStreammerSat5Datagram(\n    std::chrono::steady_clock::time_point datagramTime,\n    CbrDatagram& datagram) noexcept {\n    for (std::size_t index = 0; index < datagram.size(); ++index) {\n        const auto slotTime = datagramTime + std::chrono::nanoseconds(\n            packetOffsetNanoseconds(index, targetBitrate_));\n\n        // StableUdpOutput accrues real-packet entitlement on every transport\n        // slot, including slots occupied by its periodic PCR-only packet.\n        tvSatRealTokenAccumulator_ += tvSatRealPaceBitrate_;\n\n        // TVStreamer5 continuous SRT/HTTP owns a synthetic 20 ms PCR domain.\n        // Periodic PCR consumes an otherwise NULL CBR slot; the real-packet\n        // token remains accrued so useful media is not throttled by PCR inserts.\n        if (tvSatPcrInitialized_ && slotTime >= tvSatNextPeriodicPcrTime_) {\n            makePeriodicPcrPacket(datagram[index], slotTime);\n            do {\n                tvSatNextPeriodicPcrTime_ += kTvSatPeriodicPcrInterval;\n            } while (tvSatNextPeriodicPcrTime_ <= slotTime);\n            continue;\n        }\n\n        bool sendReal = false;\n        if (targetBitrate_ > 0 && tvSatRealTokenAccumulator_ >= targetBitrate_) {\n            tvSatRealTokenAccumulator_ -= targetBitrate_;\n            sendReal = !queuedPackets_.empty();\n            if (!sendReal) {\n                // Never accumulate a catch-up burst during an upstream gap.\n                tvSatRealTokenAccumulator_ = (std::min)(\n                    tvSatRealTokenAccumulator_, targetBitrate_ - std::uint64_t{1});\n            }\n        }\n\n        if (sendReal) {\n            datagram[index] = queuedPackets_.front();\n            queuedPackets_.pop_front();\n            processTvStreammerSat5RealPacket(datagram[index], slotTime);\n        } else {\n            makeNullPacket(datagram[index]);\n        }\n    }\n}\n\nvoid CbrTsPacer::processTvStreammerSat5RealPacket(\n    Packet& packet,\n    std::chrono::steady_clock::time_point slotTime) noexcept {\n    PacketInfo info;\n    if (!inspectPacket(packet.data(), packet.size(), info)) return;\n\n    if (info.hasPcr) {\n        if (!tvSatPcrInitialized_) {\n            // Match StableUdpOutput 203.07/202.74: the first provider PCR is\n            // the phase anchor. Do not consume startup media or advance phase.\n            tvSatPcrInitialized_ = true;\n            tvSatPcrPid_ = info.pid;\n            tvSatPcrOriginTicks_ =\n                (info.pcrBase90k * 300ULL) % kPcrTicksModulus;\n            tvSatPcrOriginTime_ = slotTime;\n            tvSatNextPeriodicPcrTime_ = slotTime + kTvSatPeriodicPcrInterval;\n            writePcr(packet, tvSatPcrOriginTicks_);\n            std::cerr << "CBR TVSTREAMMERSAT5 PCR lock"\n                      << " pid=" << tvSatPcrPid_\n                      << " mode=synthetic-tvstreamer5-20ms"\n                      << " source_pcr=stripped-after-lock"\n                      << " media_clock=independent"\n                      << std::endl;\n        } else if (info.pid == tvSatPcrPid_) {\n            clearPcrFlag(packet);\n        }\n    }\n\n    // Adaptation-only PCR packets do not advance payload continuity. Periodic\n    // PCR packets reuse the most recently observed payload CC on the PCR PID.\n    if (tvSatPcrInitialized_ && info.pid == tvSatPcrPid_ && info.hasPayload) {\n        tvSatPcrPidContinuityCounter_ = info.continuityCounter;\n        tvSatPcrPidContinuityValid_ = true;\n    }\n}\n'''
cpp = replace_once(cpp, old_fill, new_fill, 'fill/process SAT5 PCR')

cpp = replace_once(
    cpp,
    '''void CbrTsPacer::writePcr(Packet& packet, std::uint64_t pcrTicks) noexcept {\n''',
    '''void CbrTsPacer::clearPcrFlag(Packet& packet) noexcept {\n    if (packet[0] != kSyncByte) return;\n    const std::uint8_t adaptationFieldControl =\n        static_cast<std::uint8_t>((packet[3] >> 4) & 0x03U);\n    if (adaptationFieldControl != 2U && adaptationFieldControl != 3U) return;\n    if (packet[4] == 0) return;\n    packet[5] = static_cast<std::uint8_t>(packet[5] & ~0x10U);\n}\n\nvoid CbrTsPacer::writePcr(Packet& packet, std::uint64_t pcrTicks) noexcept {\n''',
    'clear PCR helper')

h = replace_once(
    h,
    '''    void fillTvStreammerSat5Datagram(\n''',
    '''    void advanceTvStreammerSat5MediaTimeline() noexcept;\n    void fillTvStreammerSat5Datagram(\n''',
    'header media advance')

h = replace_once(
    h,
    '''    static void writePcr(Packet& packet, std::uint64_t pcrTicks) noexcept;\n''',
    '''    static void clearPcrFlag(Packet& packet) noexcept;\n    static void writePcr(Packet& packet, std::uint64_t pcrTicks) noexcept;\n''',
    'header clear PCR')

h = replace_once(
    h,
    '''    std::chrono::steady_clock::time_point tvSatLastControllerUpdate_ {};\n''',
    '''    std::chrono::steady_clock::time_point tvSatLastControllerUpdate_ {};\n    // SAT5 keeps this media/PCR timeline independent from nextDeadline_. A\n    // scheduler reset may rebase the physical sender but never the PCR clock.\n    std::chrono::steady_clock::time_point tvSatMediaTimeline_ {};\n    std::uint64_t tvSatMediaRemainder_ = 0;\n''',
    'header media state')

ver = replace_once(ver, 'kProgramVersion = "10.8.162"', 'kProgramVersion = "10.8.163"', 'version')

cpp_path.write_text(cpp)
h_path.write_text(h)
ver_path.write_text(ver)

print('Applied V10.8.163 SAT5 independent media/PCR clock patch')
