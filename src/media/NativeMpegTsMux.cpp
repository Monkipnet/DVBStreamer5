#include "media/NativeMpegTsMux.h"
#include "media/DvbText.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace dvbstreamer5::media::mpegts {
namespace {

constexpr std::uint16_t kPatPid = 0x0000;
constexpr std::uint16_t kSdtPid = 0x0011;
constexpr std::uint64_t kPsiInterval90k = 9000;   // 100 ms
constexpr std::uint64_t kSdtInterval90k = 45000; // 500 ms
constexpr std::uint64_t kDefaultVideoDuration90k = 3600; // 25 fps
constexpr std::uint64_t kDefaultAudioDuration90k = 1920; // AAC 1024/48k
constexpr std::uint64_t kPacketBits = kPacketSize * 8ULL;

std::uint32_t mpegCrc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= static_cast<std::uint32_t>(data[index]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U : (crc << 1);
        }
    }
    return crc;
}

std::string trimmedServiceString(const std::string& value, std::size_t maximum) {
    if (value.size() <= maximum) return value;
    return value.substr(0, maximum);
}

} // namespace

bool NativeMpegTsMux::validElementaryPid(std::uint16_t pid) noexcept {
    return pid >= 0x0020 && pid < kNullPid;
}

std::uint8_t NativeMpegTsMux::streamType(ElementaryCodec codec) noexcept {
    switch (codec) {
        case ElementaryCodec::H264: return 0x1b;
        case ElementaryCodec::H265: return 0x24;
        case ElementaryCodec::Mpeg2Video: return 0x02;
        case ElementaryCodec::AacAdts: return 0x0f;
        case ElementaryCodec::AacLatm: return 0x11;
        case ElementaryCodec::MpegAudio: return 0x03;
        case ElementaryCodec::Ac3:
        case ElementaryCodec::Eac3: return 0x06;
        default: return 0x00;
    }
}

std::uint8_t NativeMpegTsMux::pesStreamId(ElementaryKind kind) noexcept {
    return kind == ElementaryKind::Video ? 0xe0 : 0xc0;
}

bool NativeMpegTsMux::initialize(const NativeMuxConfig& config, std::string& error) {
    reset();
    config_ = config;
    if (config_.serviceId == 0) config_.serviceId = 1;
    if (config_.transportStreamId == 0) config_.transportStreamId = 1;
    if (config_.originalNetworkId == 0) config_.originalNetworkId = 1;
    if (!validElementaryPid(config_.videoPid)) config_.videoPid = 0x0100;
    if (!validElementaryPid(config_.audioPid) || config_.audioPid == config_.videoPid) {
        config_.audioPid = config_.videoPid == 0x0101 ? 0x0102 : 0x0101;
    }
    if (!validElementaryPid(config_.pmtPid) || config_.pmtPid == config_.videoPid ||
        config_.pmtPid == config_.audioPid) {
        config_.pmtPid = 0x1000;
        while (config_.pmtPid == config_.videoPid || config_.pmtPid == config_.audioPid) {
            ++config_.pmtPid;
        }
    }
    if (config_.targetBitrate != 0 &&
        (config_.targetBitrate < 256000 || config_.targetBitrate > 200000000ULL)) {
        error = "native MPEG-TS target bitrate must be 256 kbit/s .. 200 Mbit/s";
        return false;
    }
    if (config_.serviceName.empty()) config_.serviceName = "DVBStreamer5";
    if (config_.serviceProvider.empty()) config_.serviceProvider = "DVBStreamer5";
    initialized_ = true;
    return true;
}

void NativeMpegTsMux::reset() noexcept {
    config_ = {};
    videoCodec_ = ElementaryCodec::Unknown;
    audioCodec_ = ElementaryCodec::Unknown;
    continuity_.fill(0);
    patContinuity_ = pmtContinuity_ = sdtContinuity_ = nullContinuity_ = 0;
    pmtVersion_ = 0;
    lastPsi90k_ = lastSdt90k_ = 0;
    lastVideoPts90k_ = lastAudioPts90k_ = 0;
    clockBase90k_ = emittedPackets_ = 0;
    initialized_ = psiEmitted_ = clockStarted_ = false;
}

void NativeMpegTsMux::markCodecChange() noexcept {
    pmtVersion_ = static_cast<std::uint8_t>((pmtVersion_ + 1U) & 0x1fU);
    psiEmitted_ = false;
}

bool NativeMpegTsMux::setCodec(ElementaryKind kind, ElementaryCodec codec, std::string& error) {
    if (!initialized_) {
        error = "native MPEG-TS mux is not initialized";
        return false;
    }
    if (codec == ElementaryCodec::Unknown || streamType(codec) == 0) {
        error = "unsupported native MPEG-TS elementary codec";
        return false;
    }
    ElementaryCodec& target = kind == ElementaryKind::Video ? videoCodec_ : audioCodec_;
    if (target != codec) {
        target = codec;
        markCodecChange();
    }
    return true;
}

void NativeMpegTsMux::appendCrc(std::vector<std::uint8_t>& section) {
    const std::uint32_t crc = mpegCrc32(section.data(), section.size());
    section.push_back(static_cast<std::uint8_t>(crc >> 24));
    section.push_back(static_cast<std::uint8_t>(crc >> 16));
    section.push_back(static_cast<std::uint8_t>(crc >> 8));
    section.push_back(static_cast<std::uint8_t>(crc));
}

void NativeMpegTsMux::appendPts(
    std::vector<std::uint8_t>& data, std::uint8_t prefix, std::uint64_t value90k) {
    const std::uint64_t value = value90k & ((1ULL << 33) - 1ULL);
    data.push_back(static_cast<std::uint8_t>((prefix << 4) | (((value >> 30) & 0x07U) << 1) | 0x01U));
    data.push_back(static_cast<std::uint8_t>(value >> 22));
    data.push_back(static_cast<std::uint8_t>((((value >> 15) & 0x7fU) << 1) | 0x01U));
    data.push_back(static_cast<std::uint8_t>(value >> 7));
    data.push_back(static_cast<std::uint8_t>(((value & 0x7fU) << 1) | 0x01U));
}

std::vector<std::uint8_t> NativeMpegTsMux::makePatSection() const {
    std::vector<std::uint8_t> section;
    section.reserve(16);
    section.push_back(0x00);
    section.push_back(0xb0);
    section.push_back(0x0d);
    section.push_back(static_cast<std::uint8_t>(config_.transportStreamId >> 8));
    section.push_back(static_cast<std::uint8_t>(config_.transportStreamId));
    section.push_back(0xc1); // version 0, current_next=1
    section.push_back(0x00);
    section.push_back(0x00);
    section.push_back(static_cast<std::uint8_t>(config_.serviceId >> 8));
    section.push_back(static_cast<std::uint8_t>(config_.serviceId));
    section.push_back(static_cast<std::uint8_t>(0xe0U | ((config_.pmtPid >> 8) & 0x1fU)));
    section.push_back(static_cast<std::uint8_t>(config_.pmtPid));
    appendCrc(section);
    return section;
}

std::vector<std::uint8_t> NativeMpegTsMux::makePmtSection() const {
    std::vector<std::uint8_t> es;
    const auto addStream = [&](ElementaryCodec codec, std::uint16_t pid) {
        if (codec == ElementaryCodec::Unknown) return;
        es.push_back(streamType(codec));
        es.push_back(static_cast<std::uint8_t>(0xe0U | ((pid >> 8) & 0x1fU)));
        es.push_back(static_cast<std::uint8_t>(pid));
        std::vector<std::uint8_t> descriptors;
        if (codec == ElementaryCodec::Ac3) descriptors = {0x6a, 0x00};
        if (codec == ElementaryCodec::Eac3) descriptors = {0x7a, 0x00};
        es.push_back(static_cast<std::uint8_t>(0xf0U | ((descriptors.size() >> 8) & 0x0fU)));
        es.push_back(static_cast<std::uint8_t>(descriptors.size()));
        es.insert(es.end(), descriptors.begin(), descriptors.end());
    };
    addStream(videoCodec_, config_.videoPid);
    addStream(audioCodec_, config_.audioPid);

    const std::uint16_t pcrPid = videoCodec_ != ElementaryCodec::Unknown
        ? config_.videoPid : config_.audioPid;
    const std::size_t sectionLength = 9 + es.size() + 4;
    std::vector<std::uint8_t> section;
    section.reserve(3 + sectionLength);
    section.push_back(0x02);
    section.push_back(static_cast<std::uint8_t>(0xb0U | ((sectionLength >> 8) & 0x0fU)));
    section.push_back(static_cast<std::uint8_t>(sectionLength));
    section.push_back(static_cast<std::uint8_t>(config_.serviceId >> 8));
    section.push_back(static_cast<std::uint8_t>(config_.serviceId));
    section.push_back(static_cast<std::uint8_t>(0xc1U | ((pmtVersion_ & 0x1fU) << 1)));
    section.push_back(0x00);
    section.push_back(0x00);
    section.push_back(static_cast<std::uint8_t>(0xe0U | ((pcrPid >> 8) & 0x1fU)));
    section.push_back(static_cast<std::uint8_t>(pcrPid));
    section.push_back(0xf0);
    section.push_back(0x00);
    section.insert(section.end(), es.begin(), es.end());
    appendCrc(section);
    return section;
}

std::vector<std::uint8_t> NativeMpegTsMux::makeSdtSection() const {
    // A service_descriptor has an 8-bit descriptor_length. Keep the combined
    // provider/name payload below 255 bytes and use the DVB UTF-8 selector for
    // Cyrillic and other non-ASCII service names.
    auto provider = dvbtext::encode(config_.serviceProvider, 120);
    auto name = dvbtext::encode(config_.serviceName, 120);
    while (3 + provider.size() + name.size() > 255 && !name.empty()) name.pop_back();
    while (3 + provider.size() + name.size() > 255 && !provider.empty()) provider.pop_back();
    std::vector<std::uint8_t> descriptor;
    descriptor.reserve(5 + provider.size() + name.size());
    descriptor.push_back(0x48);
    descriptor.push_back(static_cast<std::uint8_t>(3 + provider.size() + name.size()));
    descriptor.push_back(0x01); // digital television service
    descriptor.push_back(static_cast<std::uint8_t>(provider.size()));
    descriptor.insert(descriptor.end(), provider.begin(), provider.end());
    descriptor.push_back(static_cast<std::uint8_t>(name.size()));
    descriptor.insert(descriptor.end(), name.begin(), name.end());

    const std::size_t serviceLoopLength = descriptor.size();
    const std::size_t sectionLength = 13 + serviceLoopLength + 4;
    std::vector<std::uint8_t> section;
    section.reserve(3 + sectionLength);
    section.push_back(0x42);
    section.push_back(static_cast<std::uint8_t>(0xf0U | ((sectionLength >> 8) & 0x0fU)));
    section.push_back(static_cast<std::uint8_t>(sectionLength));
    section.push_back(static_cast<std::uint8_t>(config_.transportStreamId >> 8));
    section.push_back(static_cast<std::uint8_t>(config_.transportStreamId));
    section.push_back(0xc1);
    section.push_back(0x00);
    section.push_back(0x00);
    section.push_back(static_cast<std::uint8_t>(config_.originalNetworkId >> 8));
    section.push_back(static_cast<std::uint8_t>(config_.originalNetworkId));
    section.push_back(0xff);
    section.push_back(static_cast<std::uint8_t>(config_.serviceId >> 8));
    section.push_back(static_cast<std::uint8_t>(config_.serviceId));
    section.push_back(0xfc); // reserved bits + EIT schedule/present-following flags = 0
    section.push_back(static_cast<std::uint8_t>(0x80U | ((serviceLoopLength >> 8) & 0x0fU))); // running
    section.push_back(static_cast<std::uint8_t>(serviceLoopLength));
    section.insert(section.end(), descriptor.begin(), descriptor.end());
    appendCrc(section);
    return section;
}

Packet NativeMpegTsMux::makeSectionPacket(
    std::uint16_t pid, std::uint8_t& continuity, const std::vector<std::uint8_t>& section) {
    Packet packet {};
    packet.fill(0xff);
    packet[0] = kSyncByte;
    packet[1] = static_cast<std::uint8_t>(0x40U | ((pid >> 8) & 0x1fU));
    packet[2] = static_cast<std::uint8_t>(pid);
    packet[3] = static_cast<std::uint8_t>(0x10U | (continuity & 0x0fU));
    continuity = static_cast<std::uint8_t>((continuity + 1U) & 0x0fU);
    packet[4] = 0x00; // pointer_field
    const std::size_t copySize = std::min<std::size_t>(section.size(), kPacketSize - 5);
    std::memcpy(packet.data() + 5, section.data(), copySize);
    return packet;
}

Packet NativeMpegTsMux::makeNullPacket() {
    Packet packet {};
    packet.fill(0xff);
    packet[0] = kSyncByte;
    packet[1] = 0x1f;
    packet[2] = 0xff;
    packet[3] = static_cast<std::uint8_t>(0x10U | (nullContinuity_ & 0x0fU));
    nullContinuity_ = static_cast<std::uint8_t>((nullContinuity_ + 1U) & 0x0fU);
    return packet;
}

void NativeMpegTsMux::emitPsi(
    std::uint64_t clock90k, bool force, std::vector<Packet>& output) {
    if (videoCodec_ == ElementaryCodec::Unknown && audioCodec_ == ElementaryCodec::Unknown) return;
    if (!force && psiEmitted_ && clock90k >= lastPsi90k_ &&
        clock90k - lastPsi90k_ < kPsiInterval90k) {
        return;
    }
    output.push_back(makeSectionPacket(kPatPid, patContinuity_, makePatSection()));
    output.push_back(makeSectionPacket(config_.pmtPid, pmtContinuity_, makePmtSection()));
    emittedPackets_ += 2;
    lastPsi90k_ = clock90k;
    psiEmitted_ = true;

    if (lastSdt90k_ == 0 || clock90k < lastSdt90k_ ||
        clock90k - lastSdt90k_ >= kSdtInterval90k) {
        const auto sdt = makeSdtSection();
        if (sdt.size() <= kPacketSize - 5) {
            output.push_back(makeSectionPacket(kSdtPid, sdtContinuity_, sdt));
            ++emittedPackets_;
            lastSdt90k_ = clock90k;
        }
    }
}

void NativeMpegTsMux::padToClock(std::uint64_t clock90k, std::vector<Packet>& output) {
    if (config_.targetBitrate == 0) return;
    if (!clockStarted_) {
        clockStarted_ = true;
        clockBase90k_ = clock90k;
        return;
    }
    if (clock90k <= clockBase90k_) return;
    const std::uint64_t elapsed90k = clock90k - clockBase90k_;
    const long double expected =
        static_cast<long double>(elapsed90k) * static_cast<long double>(config_.targetBitrate) /
        (90000.0L * static_cast<long double>(kPacketBits));
    std::uint64_t expectedPackets = expected > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(expected);
    // Do not create an unbounded burst after a discontinuity. At most 250 ms of
    // filler is generated by a single sample callback; a later sample catches up.
    const std::uint64_t maximumBurst = std::max<std::uint64_t>(1,
        config_.targetBitrate / kPacketBits / 4ULL);
    if (expectedPackets > emittedPackets_) {
        const std::uint64_t missing = std::min(expectedPackets - emittedPackets_, maximumBurst);
        for (std::uint64_t i = 0; i < missing; ++i) output.push_back(makeNullPacket());
        emittedPackets_ += missing;
    }
}

std::uint64_t NativeMpegTsMux::sampleClock(
    ElementaryKind kind, const ElementarySample& sample) {
    std::uint64_t& previous = kind == ElementaryKind::Video ? lastVideoPts90k_ : lastAudioPts90k_;
    const std::uint64_t fallbackDuration = sample.duration90k != 0
        ? sample.duration90k
        : (kind == ElementaryKind::Video ? kDefaultVideoDuration90k : kDefaultAudioDuration90k);
    std::uint64_t value = sample.hasDts ? sample.dts90k : (sample.hasPts ? sample.pts90k : 0);
    if ((!sample.hasDts && !sample.hasPts) || (previous != 0 && value <= previous)) {
        value = previous != 0 ? previous + fallbackDuration : fallbackDuration;
    }
    previous = value;
    return value;
}

void NativeMpegTsMux::packetizePes(
    ElementaryKind kind,
    std::uint16_t pid,
    const ElementarySample& sample,
    std::vector<Packet>& output) {
    const std::uint64_t pts = sample.hasPts ? sample.pts90k
        : (sample.hasDts ? sample.dts90k : sampleClock(kind, sample));
    const std::uint64_t dts = sample.hasDts ? sample.dts90k : pts;
    const bool separateDts = sample.hasDts && sample.hasPts && dts != pts;

    std::vector<std::uint8_t> pes;
    pes.reserve(sample.size + 32);
    pes.insert(pes.end(), {0x00, 0x00, 0x01, pesStreamId(kind), 0x00, 0x00, 0x80});
    pes.push_back(separateDts ? 0xc0 : 0x80);
    pes.push_back(separateDts ? 10 : 5);
    appendPts(pes, separateDts ? 0x03 : 0x02, pts);
    if (separateDts) appendPts(pes, 0x01, dts);
    if (sample.data && sample.size) pes.insert(pes.end(), sample.data, sample.data + sample.size);

    const std::size_t afterLength = pes.size() - 6;
    if (kind == ElementaryKind::Audio && afterLength <= 0xffffU) {
        pes[4] = static_cast<std::uint8_t>(afterLength >> 8);
        pes[5] = static_cast<std::uint8_t>(afterLength);
    }

    std::size_t offset = 0;
    bool first = true;
    std::uint8_t& continuity = continuity_[pid];
    while (offset < pes.size()) {
        Packet packet {};
        packet.fill(0xff);
        packet[0] = kSyncByte;
        packet[1] = static_cast<std::uint8_t>(((first ? 0x40U : 0x00U)) | ((pid >> 8) & 0x1fU));
        packet[2] = static_cast<std::uint8_t>(pid);

        const std::uint16_t pcrPid = videoCodec_ != ElementaryCodec::Unknown
            ? config_.videoPid : config_.audioPid;
        const bool writePcr = first && pid == pcrPid;
        const std::size_t remaining = pes.size() - offset;
        const std::size_t maxPayload = writePcr ? 176 : 184;
        const std::size_t payload = std::min(remaining, maxPayload);
        const bool needsAdaptation = writePcr || payload < 184;
        packet[3] = static_cast<std::uint8_t>((needsAdaptation ? 0x30U : 0x10U) | (continuity & 0x0fU));
        continuity = static_cast<std::uint8_t>((continuity + 1U) & 0x0fU);

        std::size_t payloadOffset = 4;
        if (needsAdaptation) {
            const std::size_t adaptationLength = 183 - payload;
            packet[4] = static_cast<std::uint8_t>(adaptationLength);
            payloadOffset = 5 + adaptationLength;
            if (adaptationLength > 0) {
                packet[5] = static_cast<std::uint8_t>((writePcr ? 0x10U : 0x00U) |
                    ((first && sample.randomAccess) ? 0x40U : 0x00U));
                if (writePcr) {
                    const std::uint64_t pcr = dts & ((1ULL << 33) - 1ULL);
                    packet[6] = static_cast<std::uint8_t>(pcr >> 25);
                    packet[7] = static_cast<std::uint8_t>(pcr >> 17);
                    packet[8] = static_cast<std::uint8_t>(pcr >> 9);
                    packet[9] = static_cast<std::uint8_t>(pcr >> 1);
                    packet[10] = static_cast<std::uint8_t>((pcr & 1U) << 7 | 0x7eU);
                    packet[11] = 0x00;
                }
            }
        }
        std::memcpy(packet.data() + payloadOffset, pes.data() + offset, payload);
        offset += payload;
        output.push_back(packet);
        ++emittedPackets_;
        first = false;
    }
}

bool NativeMpegTsMux::write(
    ElementaryKind kind,
    const ElementarySample& sample,
    std::vector<Packet>& output,
    std::string& error) {
    if (!initialized_) {
        error = "native MPEG-TS mux is not initialized";
        return false;
    }
    if (!sample.data || sample.size == 0) {
        error = "empty elementary sample";
        return false;
    }
    const ElementaryCodec codec = kind == ElementaryKind::Video ? videoCodec_ : audioCodec_;
    if (codec == ElementaryCodec::Unknown) {
        error = kind == ElementaryKind::Video
            ? "video codec is not configured in native MPEG-TS mux"
            : "audio codec is not configured in native MPEG-TS mux";
        return false;
    }

    const std::uint64_t originalClock = sample.hasDts ? sample.dts90k : (sample.hasPts ? sample.pts90k : 0);
    const std::uint64_t clock90k = sampleClock(kind, sample);
    ElementarySample normalized = sample;
    if (sample.hasPts && sample.hasDts && sample.pts90k >= originalClock) {
        normalized.pts90k = clock90k + (sample.pts90k - originalClock);
        normalized.dts90k = clock90k;
        normalized.hasPts = true;
        normalized.hasDts = true;
    } else {
        normalized.pts90k = clock90k;
        normalized.dts90k = clock90k;
        normalized.hasPts = true;
        normalized.hasDts = true;
    }
    padToClock(clock90k, output);
    emitPsi(clock90k, !psiEmitted_, output);
    packetizePes(kind,
        kind == ElementaryKind::Video ? config_.videoPid : config_.audioPid,
        normalized, output);
    return true;
}

} // namespace dvbstreamer5::media::mpegts
