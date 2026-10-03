#include "media/NativeTsDemux.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <utility>

namespace dvbstreamer5::media::mpegts {
namespace {

std::uint16_t read16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

bool validSectionCrc(const std::vector<std::uint8_t>& section) {
    if (section.size() < 4) return false;
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t b : section) {
        crc ^= static_cast<std::uint32_t>(b) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) ? ((crc << 1) ^ 0x04c11db7U) : (crc << 1);
        }
    }
    return crc == 0;
}

struct NalSpan {
    std::size_t startCode = 0;
    std::size_t nal = 0;
};

std::vector<NalSpan> annexBStarts(const std::vector<std::uint8_t>& data) {
    std::vector<NalSpan> out;
    for (std::size_t i = 0; i + 3 < data.size();) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            out.push_back({i, i + 3});
            i += 3;
            continue;
        }
        if (i + 4 < data.size() && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            out.push_back({i, i + 4});
            i += 4;
            continue;
        }
        ++i;
    }
    return out;
}

class H264RbspReader {
public:
    H264RbspReader(const std::uint8_t* nal, std::size_t size) {
        if (!nal || size <= 1) return;
        rbsp_.reserve(size - 1);
        unsigned zeros = 0;
        for (std::size_t i = 1; i < size; ++i) {
            const std::uint8_t b = nal[i];
            if (zeros >= 2 && b == 0x03) {
                zeros = 0;
                continue;
            }
            rbsp_.push_back(b);
            zeros = b == 0 ? zeros + 1U : 0U;
        }
    }

    bool readBit(bool& value) {
        if (bit_ >= rbsp_.size() * 8U) return false;
        value = (rbsp_[bit_ / 8U] & (0x80U >> (bit_ % 8U))) != 0;
        ++bit_;
        return true;
    }

    bool readBits(unsigned count, std::uint32_t& value) {
        if (count > 32U) return false;
        value = 0;
        for (unsigned i = 0; i < count; ++i) {
            bool bit = false;
            if (!readBit(bit)) return false;
            value = (value << 1U) | (bit ? 1U : 0U);
        }
        return true;
    }

    bool readUe(std::uint32_t& value) {
        unsigned leading = 0;
        bool bit = false;
        while (true) {
            if (!readBit(bit)) return false;
            if (bit) break;
            if (++leading > 31U) return false;
        }
        std::uint32_t suffix = 0;
        if (leading != 0 && !readBits(leading, suffix)) return false;
        value = ((1U << leading) - 1U) + suffix;
        return true;
    }

    bool readSe(std::int32_t& value) {
        std::uint32_t codeNum = 0;
        if (!readUe(codeNum)) return false;
        if (codeNum & 1U)
            value = static_cast<std::int32_t>((codeNum + 1U) / 2U);
        else
            value = -static_cast<std::int32_t>(codeNum / 2U);
        return true;
    }

private:
    std::vector<std::uint8_t> rbsp_;
    std::size_t bit_ = 0;
};

bool h264FirstMbInSliceZero(const std::uint8_t* nal, std::size_t size) {
    if (!nal || size < 2) return false;
    H264RbspReader bits(nal, size);
    std::uint32_t firstMb = 1;
    return bits.readUe(firstMb) && firstMb == 0;
}

bool isVideoAuBoundary(ElementaryCodec codec,
                       const std::uint8_t* nal, std::size_t size,
                       bool seenVcl) {
    if (!nal || size == 0) return false;
    if (codec == ElementaryCodec::H264) {
        const std::uint8_t type = nal[0] & 0x1fU;
        if (type == 9) return seenVcl;
        if (type >= 1 && type <= 5 && seenVcl)
            return h264FirstMbInSliceZero(nal, size);
        return false;
    }
    if (codec == ElementaryCodec::H265) {
        if (size < 3) return false;
        const std::uint8_t type = static_cast<std::uint8_t>((nal[0] >> 1) & 0x3fU);
        if (type == 35) return seenVcl;
        if (type <= 31 && seenVcl) {
            // first_slice_segment_in_pic_flag is the first bit after the
            // two-byte HEVC NAL header.
            return (nal[2] & 0x80U) != 0;
        }
    }
    return false;
}

bool isVideoVcl(ElementaryCodec codec, const std::uint8_t* nal, std::size_t size) {
    if (!nal || size == 0) return false;
    if (codec == ElementaryCodec::H264) {
        const std::uint8_t type = nal[0] & 0x1fU;
        return type >= 1 && type <= 5;
    }
    if (codec == ElementaryCodec::H265) {
        const std::uint8_t type = static_cast<std::uint8_t>((nal[0] >> 1) & 0x3fU);
        return type <= 31;
    }
    return false;
}

} // namespace

void NativeTsDemux::setSampleCallback(SampleCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    sampleCallback_ = std::move(callback);
}

void NativeTsDemux::setProgramCallback(ProgramCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    programCallback_ = std::move(callback);
}

void NativeTsDemux::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    framer_.reset();
    psi_.clear();
    streamsByPid_.clear();
    pes_.clear();
    videoAu_.clear();
    programNumber_ = 0;
    pmtPid_ = 0xffff;
    pesTrimEvents_ = 0;
    pesTruncatedEvents_ = 0;
}

std::vector<DemuxStreamInfo> NativeTsDemux::streams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DemuxStreamInfo> result;
    result.reserve(streamsByPid_.size());
    for (const auto& [pid, stream] : streamsByPid_) {
        (void)pid;
        result.push_back(stream);
    }
    return result;
}

bool NativeTsDemux::push(const std::uint8_t* data, std::size_t size, std::string& error) {
    if (!data || size == 0) return true;
    std::vector<Packet> packets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        framer_.push(data, size, packets);
        for (const auto& packet : packets) consumePacket(packet, error);
    }
    return error.empty();
}

void NativeTsDemux::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint16_t> pids;
    pids.reserve(pes_.size());
    for (const auto& [pid, state] : pes_) {
        (void)state;
        pids.push_back(pid);
    }
    for (const auto pid : pids) flushPes(pid);
    std::vector<std::uint16_t> videoPids;
    videoPids.reserve(videoAu_.size());
    for (const auto& [pid, state] : videoAu_) { (void)state; videoPids.push_back(pid); }
    for (const auto pid : videoPids) flushVideoAu(pid);
}

void NativeTsDemux::consumePacket(const Packet& packet, std::string& error) {
    PacketInfo info;
    if (!inspectPacket(packet.data(), packet.size(), info)) return;
    if (!info.hasPayload || info.transportError || info.scrambled || info.payloadOffset >= packet.size()) return;
    const auto* payload = packet.data() + info.payloadOffset;
    const std::size_t payloadSize = packet.size() - info.payloadOffset;
    if (info.pid == 0 || info.pid == pmtPid_) {
        consumePsi(info.pid, info, payload, payloadSize);
        return;
    }
    if (streamsByPid_.find(info.pid) != streamsByPid_.end()) {
        consumePes(info, payload, payloadSize);
    }
    (void)error;
}

void NativeTsDemux::consumePsi(std::uint16_t pid, const PacketInfo& info,
                               const std::uint8_t* payload, std::size_t size) {
    if (!payload || size == 0) return;
    auto& a = psi_[pid];
    std::size_t offset = 0;
    if (info.payloadUnitStart) {
        const std::size_t pointer = payload[0];
        if (1 + pointer > size) { a = {}; return; }
        if (!a.bytes.empty() && pointer > 0) {
            const std::size_t append = std::min(pointer, size - 1);
            a.bytes.insert(a.bytes.end(), payload + 1, payload + 1 + append);
            if (a.expected && a.bytes.size() >= a.expected) {
                a.bytes.resize(a.expected);
                consumeSection(pid, a.bytes);
            }
        }
        a = {};
        offset = 1 + pointer;
    }

    while (offset < size) {
        if (a.bytes.empty() && payload[offset] == 0xff) break;
        if (a.bytes.size() < 3) {
            const std::size_t need = 3 - a.bytes.size();
            const std::size_t take = std::min(need, size - offset);
            a.bytes.insert(a.bytes.end(), payload + offset, payload + offset + take);
            offset += take;
            if (a.bytes.size() < 3) return;
            const std::size_t sectionLength = static_cast<std::size_t>(((a.bytes[1] & 0x0fU) << 8) | a.bytes[2]);
            if (sectionLength > 4093) { a = {}; return; }
            a.expected = 3 + sectionLength;
        }
        const std::size_t need = a.expected - a.bytes.size();
        const std::size_t take = std::min(need, size - offset);
        a.bytes.insert(a.bytes.end(), payload + offset, payload + offset + take);
        offset += take;
        if (a.bytes.size() == a.expected) {
            consumeSection(pid, a.bytes);
            a = {};
        }
    }
}

void NativeTsDemux::consumeSection(std::uint16_t pid, const std::vector<std::uint8_t>& section) {
    if (section.size() < 8 || !validSectionCrc(section)) return;
    if (pid == 0 && section[0] == 0x00) parsePat(section);
    else if (pid == pmtPid_ && section[0] == 0x02) parsePmt(section);
}

void NativeTsDemux::parsePat(const std::vector<std::uint8_t>& section) {
    if (section.size() < 12) return;
    const std::size_t end = section.size() - 4;
    for (std::size_t pos = 8; pos + 4 <= end; pos += 4) {
        const std::uint16_t program = read16(section.data() + pos);
        if (program == 0) continue;
        const std::uint16_t pid = static_cast<std::uint16_t>(((section[pos + 2] & 0x1fU) << 8) | section[pos + 3]);
        if (programNumber_ != program || pmtPid_ != pid) {
            programNumber_ = program;
            pmtPid_ = pid;
            streamsByPid_.clear();
            pes_.clear();
            videoAu_.clear();
            psi_.erase(pid);
        }
        break;
    }
}

ElementaryCodec NativeTsDemux::codecFromStreamType(std::uint8_t streamType,
                                                     const std::uint8_t* descriptors,
                                                     std::size_t descriptorSize) {
    switch (streamType) {
        case 0x1b: return ElementaryCodec::H264;
        case 0x24: return ElementaryCodec::H265;
        case 0x01:
        case 0x02: return ElementaryCodec::Mpeg2Video;
        case 0x0f: return ElementaryCodec::AacAdts;
        case 0x11: return ElementaryCodec::AacLatm;
        case 0x03:
        case 0x04: return ElementaryCodec::MpegAudio;
        case 0x81: return ElementaryCodec::Ac3;
        default: break;
    }
    for (std::size_t pos = 0; descriptors && pos + 2 <= descriptorSize;) {
        const std::uint8_t tag = descriptors[pos];
        const std::size_t length = descriptors[pos + 1];
        if (pos + 2 + length > descriptorSize) break;
        if (tag == 0x6a) return ElementaryCodec::Ac3;
        if (tag == 0x7a) return ElementaryCodec::Eac3;
        pos += 2 + length;
    }
    return ElementaryCodec::Unknown;
}

ElementaryKind NativeTsDemux::kindFromCodec(ElementaryCodec codec) {
    switch (codec) {
        case ElementaryCodec::H264:
        case ElementaryCodec::H265:
        case ElementaryCodec::Mpeg2Video:
            return ElementaryKind::Video;
        default:
            return ElementaryKind::Audio;
    }
}

void NativeTsDemux::parsePmt(const std::vector<std::uint8_t>& section) {
    if (section.size() < 16) return;
    const std::size_t sectionEnd = section.size() - 4;
    const std::size_t programInfoLength = static_cast<std::size_t>(((section[10] & 0x0fU) << 8) | section[11]);
    std::size_t pos = 12 + programInfoLength;
    if (pos > sectionEnd) return;
    std::map<std::uint16_t, DemuxStreamInfo> next;
    while (pos + 5 <= sectionEnd) {
        const std::uint8_t streamType = section[pos];
        const std::uint16_t pid = static_cast<std::uint16_t>(((section[pos + 1] & 0x1fU) << 8) | section[pos + 2]);
        const std::size_t esInfoLength = static_cast<std::size_t>(((section[pos + 3] & 0x0fU) << 8) | section[pos + 4]);
        if (pos + 5 + esInfoLength > sectionEnd) break;
        const ElementaryCodec codec = codecFromStreamType(streamType, section.data() + pos + 5, esInfoLength);
        if (codec != ElementaryCodec::Unknown) {
            next[pid] = {pid, kindFromCodec(codec), codec, streamType};
        }
        pos += 5 + esInfoLength;
    }
    bool changed = next.size() != streamsByPid_.size();
    if (!changed) {
        for (const auto& [pid, stream] : next) {
            const auto old = streamsByPid_.find(pid);
            if (old == streamsByPid_.end() || old->second.kind != stream.kind ||
                old->second.codec != stream.codec || old->second.streamType != stream.streamType) {
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        std::vector<std::uint16_t> removed;
        for (const auto& [pid, stream] : streamsByPid_) {
            (void)stream;
            if (!next.count(pid)) removed.push_back(pid);
        }
        for (const auto pid : removed) flushPes(pid);
        streamsByPid_ = std::move(next);
        for (const auto& [pid, stream] : streamsByPid_) {
            auto& state = pes_[pid];
            state.stream = stream;
        }
        notifyProgram();
    }
}

void NativeTsDemux::notifyProgram() {
    if (!programCallback_) return;
    std::vector<DemuxStreamInfo> list;
    list.reserve(streamsByPid_.size());
    for (const auto& [pid, stream] : streamsByPid_) { (void)pid; list.push_back(stream); }
    programCallback_(list);
}

void NativeTsDemux::consumePes(const PacketInfo& info, const std::uint8_t* payload, std::size_t size) {
    auto it = streamsByPid_.find(info.pid);
    if (it == streamsByPid_.end()) return;
    auto& state = pes_[info.pid];
    state.stream = it->second;
    if (info.payloadUnitStart && !state.bytes.empty()) flushPes(info.pid);
    if (info.payloadUnitStart) state.randomAccessHint = info.randomAccess;
    state.bytes.insert(state.bytes.end(), payload, payload + size);
    // Bound malformed/unbounded PES accumulation. Normal video PES is far below this.
    if (state.bytes.size() > 16U * 1024U * 1024U) state.bytes.clear();
}

std::uint64_t NativeTsDemux::readPts(const std::uint8_t* d) noexcept {
    return ((static_cast<std::uint64_t>(d[0] >> 1) & 0x07ULL) << 30) |
           (static_cast<std::uint64_t>(d[1]) << 22) |
           ((static_cast<std::uint64_t>(d[2] >> 1) & 0x7fULL) << 15) |
           (static_cast<std::uint64_t>(d[3]) << 7) |
           (static_cast<std::uint64_t>(d[4] >> 1) & 0x7fULL);
}

bool NativeTsDemux::containsRandomAccessNal(ElementaryCodec codec, const std::uint8_t* data, std::size_t size) {
    if (!data || size < 5) return false;
    for (std::size_t i = 0; i + 4 < size; ++i) {
        std::size_t nal = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) nal = i + 3;
        else if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) nal = i + 4;
        else continue;
        if (nal >= size) break;
        if (codec == ElementaryCodec::H264 && (data[nal] & 0x1fU) == 5) return true;
        if (codec == ElementaryCodec::H265) {
            const std::uint8_t type = static_cast<std::uint8_t>((data[nal] >> 1) & 0x3fU);
            if (type >= 16 && type <= 21) return true;
        }
        i = nal;
    }
    return false;
}

bool NativeTsDemux::parsePes(const PesAssembler& pes, DemuxSample& sample) {
    const auto& b = pes.bytes;
    if (b.size() < 9 || b[0] != 0 || b[1] != 0 || b[2] != 1) return false;
    const std::uint16_t pesPacketLength = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(b[4]) << 8) | b[5]);
    const std::uint8_t flags = b[7];
    const std::size_t headerLength = b[8];
    const std::size_t payloadOffset = 9 + headerLength;
    if (payloadOffset > b.size()) return false;

    std::size_t payloadEnd = b.size();
    if (pesPacketLength != 0) {
        // ISO/IEC 13818-1: PES_packet_length counts bytes following the
        // length field.  A TS packet may therefore contain payload stuffing
        // after the declared PES packet. Never pass those bytes to a codec.
        const std::size_t declaredEnd = 6U + static_cast<std::size_t>(pesPacketLength);
        if (declaredEnd < payloadOffset || b.size() < declaredEnd) return false;
        payloadEnd = declaredEnd;
    }

    sample.stream = pes.stream;
    sample.hasPts = (flags & 0x80U) != 0 && headerLength >= 5;
    sample.hasDts = (flags & 0x40U) != 0 && headerLength >= 10;
    if (sample.hasPts) sample.pts90k = readPts(b.data() + 9);
    if (sample.hasDts) sample.dts90k = readPts(b.data() + 14);
    else if (sample.hasPts) sample.dts90k = sample.pts90k;
    sample.data.assign(
        b.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
        b.begin() + static_cast<std::ptrdiff_t>(payloadEnd));
    sample.randomAccess = pes.randomAccessHint ||
        containsRandomAccessNal(pes.stream.codec, sample.data.data(), sample.data.size());
    return !sample.data.empty();
}

void NativeTsDemux::flushPes(std::uint16_t pid) {
    auto it = pes_.find(pid);
    if (it == pes_.end() || it->second.bytes.empty()) return;
    PesAssembler current;
    current.stream = it->second.stream;
    current.randomAccessHint = it->second.randomAccessHint;
    current.bytes.swap(it->second.bytes);
    it->second.randomAccessHint = false;

    if (current.bytes.size() >= 6) {
        const std::uint16_t pesPacketLength = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(current.bytes[4]) << 8) | current.bytes[5]);
        if (pesPacketLength != 0) {
            const std::size_t declaredEnd = 6U + static_cast<std::size_t>(pesPacketLength);
            if (current.bytes.size() > declaredEnd) {
                ++pesTrimEvents_;
                if (pesTrimEvents_ <= 8 || (pesTrimEvents_ % 500U) == 0U) {
                    std::cerr << "NATIVE PES TRIM pid=" << pid
                              << " codec=" << static_cast<int>(current.stream.codec)
                              << " assembled=" << current.bytes.size()
                              << " declared=" << declaredEnd
                              << " trim=" << (current.bytes.size() - declaredEnd)
                              << " events=" << pesTrimEvents_ << std::endl;
                }
            } else if (current.bytes.size() < declaredEnd) {
                ++pesTruncatedEvents_;
                if (pesTruncatedEvents_ <= 8 || (pesTruncatedEvents_ % 100U) == 0U) {
                    std::cerr << "NATIVE PES TRUNCATED pid=" << pid
                              << " codec=" << static_cast<int>(current.stream.codec)
                              << " assembled=" << current.bytes.size()
                              << " declared=" << declaredEnd
                              << " events=" << pesTruncatedEvents_ << std::endl;
                }
            }
        }
    }

    DemuxSample sample;
    if (!parsePes(current, sample)) return;

    // V10.8.45: an MPEG-TS PES boundary is not an AVC access-unit or NAL
    // boundary. The live LVM AVC Main service can split SPS/PPS and slices
    // across adjacent PES packets. Passing each PES directly to OpenH264 made
    // the decoder cache truncated parameter sets and then report
    // dsNoParamSets/dsBitstreamError forever. Reassemble H.264 Annex-B across
    // PES boundaries and emit only complete access units. Keep all other
    // elementary codecs on their existing PES path.
    if (sample.stream.kind == ElementaryKind::Video &&
        sample.stream.codec == ElementaryCodec::H264) {
        queueVideoSample(std::move(sample));
        return;
    }
    if (sampleCallback_) sampleCallback_(std::move(sample));
}

void NativeTsDemux::queueVideoSample(DemuxSample&& sample) {
    auto& state = videoAu_[sample.stream.pid];

    // A damaged live source must not let an incomplete AVC access unit grow
    // without bound. Reset only the assembler; the next Annex-B start code
    // below will establish a clean byte-stream boundary again.
    constexpr std::size_t kMaxVideoAuBytes = 16U * 1024U * 1024U;
    if (state.bytes.size() + sample.data.size() > kMaxVideoAuBytes) {
        state = {};
    }
    state.stream = sample.stream;

    if (state.bytes.empty()) {
        state.pts90k = sample.pts90k;
        state.dts90k = sample.dts90k;
        state.hasPts = sample.hasPts;
        state.hasDts = sample.hasDts;
        state.randomAccess = sample.randomAccess;
    }
    state.bytes.insert(state.bytes.end(), sample.data.begin(), sample.data.end());
    state.randomAccess = state.randomAccess || sample.randomAccess;

    const auto parseSps = [&](const std::uint8_t* nal, std::size_t nalSize) {
        H264RbspReader bits(nal, nalSize);
        std::uint32_t profile = 0, constraints = 0, level = 0, spsId = 0;
        if (!bits.readBits(8, profile) || !bits.readBits(8, constraints) ||
            !bits.readBits(8, level) || !bits.readUe(spsId)) return;
        (void)constraints;
        (void)level;

        std::uint32_t chromaFormatIdc = 1;
        const bool highProfile =
            profile == 100 || profile == 110 || profile == 122 ||
            profile == 244 || profile == 44 || profile == 83 ||
            profile == 86 || profile == 118 || profile == 128 ||
            profile == 138 || profile == 139 || profile == 134 ||
            profile == 135;
        if (highProfile) {
            if (!bits.readUe(chromaFormatIdc) || chromaFormatIdc > 3) return;
            if (chromaFormatIdc == 3) {
                bool separateColourPlane = false;
                if (!bits.readBit(separateColourPlane)) return;
            }
            std::uint32_t ignoredUe = 0;
            bool ignoredBit = false;
            if (!bits.readUe(ignoredUe) || !bits.readUe(ignoredUe) ||
                !bits.readBit(ignoredBit) || !bits.readBit(ignoredBit)) return;
            if (ignoredBit) {
                const unsigned listCount = chromaFormatIdc != 3 ? 8U : 12U;
                for (unsigned list = 0; list < listCount; ++list) {
                    bool present = false;
                    if (!bits.readBit(present)) return;
                    if (!present) continue;
                    int lastScale = 8;
                    int nextScale = 8;
                    const unsigned count = list < 6 ? 16U : 64U;
                    for (unsigned j = 0; j < count; ++j) {
                        if (nextScale != 0) {
                            std::int32_t deltaScale = 0;
                            if (!bits.readSe(deltaScale)) return;
                            nextScale = (lastScale + deltaScale + 256) % 256;
                        }
                        if (nextScale != 0) lastScale = nextScale;
                    }
                }
            }
        }

        H264SpsState parsed;
        parsed.id = spsId;
        std::uint32_t value = 0;
        if (!bits.readUe(value) || value > 12U) return;
        parsed.log2MaxFrameNum = value + 4U;
        if (!bits.readUe(parsed.picOrderCntType) || parsed.picOrderCntType > 2U) return;
        if (parsed.picOrderCntType == 0) {
            if (!bits.readUe(value) || value > 12U) return;
            parsed.log2MaxPicOrderCntLsb = value + 4U;
        } else if (parsed.picOrderCntType == 1) {
            bool deltaAlwaysZero = false;
            if (!bits.readBit(deltaAlwaysZero)) return;
            parsed.deltaPicOrderAlwaysZero = deltaAlwaysZero;
            std::int32_t ignoredSe = 0;
            if (!bits.readSe(ignoredSe) || !bits.readSe(ignoredSe)) return;
            std::uint32_t cycle = 0;
            if (!bits.readUe(cycle) || cycle > 255U) return;
            for (std::uint32_t i = 0; i < cycle; ++i)
                if (!bits.readSe(ignoredSe)) return;
        }
        if (!bits.readUe(value)) return; // max_num_ref_frames
        bool ignoredBit = false;
        if (!bits.readBit(ignoredBit) || // gaps_in_frame_num_value_allowed_flag
            !bits.readUe(value) ||       // pic_width_in_mbs_minus1
            !bits.readUe(value) ||       // pic_height_in_map_units_minus1
            !bits.readBit(parsed.frameMbsOnly)) return;
        parsed.valid = true;
        state.h264Sps[parsed.id] = parsed;
    };

    const auto parsePps = [&](const std::uint8_t* nal, std::size_t nalSize) {
        H264RbspReader bits(nal, nalSize);
        H264PpsState parsed;
        if (!bits.readUe(parsed.id) || !bits.readUe(parsed.spsId)) return;
        bool entropyCodingMode = false;
        if (!bits.readBit(entropyCodingMode) ||
            !bits.readBit(parsed.bottomFieldPicOrderInFramePresent)) return;
        (void)entropyCodingMode;
        parsed.valid = true;
        state.h264Pps[parsed.id] = parsed;
    };

    const auto parseSlice = [&](const std::uint8_t* nal, std::size_t nalSize,
                                H264SliceState& parsed) -> bool {
        if (!nal || nalSize < 2) return false;
        H264RbspReader bits(nal, nalSize);
        std::uint32_t sliceType = 0;
        if (!bits.readUe(parsed.firstMb) || !bits.readUe(sliceType) ||
            !bits.readUe(parsed.ppsId)) return false;
        const auto ppsIt = state.h264Pps.find(parsed.ppsId);
        if (ppsIt == state.h264Pps.end() || !ppsIt->second.valid) return false;
        const auto spsIt = state.h264Sps.find(ppsIt->second.spsId);
        if (spsIt == state.h264Sps.end() || !spsIt->second.valid) return false;
        const auto& sps = spsIt->second;
        if (sps.log2MaxFrameNum == 0 || sps.log2MaxFrameNum > 32U ||
            !bits.readBits(sps.log2MaxFrameNum, parsed.frameNum)) return false;
        if (!sps.frameMbsOnly) {
            if (!bits.readBit(parsed.fieldPic)) return false;
            if (parsed.fieldPic && !bits.readBit(parsed.bottomField)) return false;
        }
        parsed.idr = (nal[0] & 0x1fU) == 5U;
        parsed.nalRefIdc = static_cast<std::uint8_t>((nal[0] >> 5U) & 0x03U);
        if (parsed.idr && !bits.readUe(parsed.idrPicId)) return false;
        parsed.valid = true;
        (void)sliceType;
        return true;
    };

    // V10.8.51: keep complementary H.264 field pictures in one decoder AU.
    // The LVM Main-profile PAL service carries interlaced top/bottom field
    // pictures. V10.8.45 cut on every second first_mb_in_slice==0, which made
    // each field look like a 40 ms frame. Ittiam correctly emitted one frame
    // only after both fields, so output timestamps advanced at 12.5 fps and
    // audio ran seconds ahead. Parse the minimal SPS/PPS/slice syntax needed
    // for field_pic_flag/bottom_field_flag and only cut after a complete field
    // pair. Progressive/frame-coded AVC keeps the previous picture boundary.
    for (;;) {
        auto starts = annexBStarts(state.bytes);
        if (starts.empty()) return;

        // A live HTTP join can begin in the middle of a NAL.  Discard only
        // the undecodable prefix before the first Annex-B start code.  Do it
        // once the start code is known; never alter bytes between NAL units.
        if (starts.front().startCode != 0) {
            const std::size_t prefix = starts.front().startCode;
            state.bytes.erase(state.bytes.begin(),
                              state.bytes.begin() + static_cast<std::ptrdiff_t>(prefix));
            starts = annexBStarts(state.bytes);
        }
        if (starts.size() < 2) return; // last NAL may still span the next PES

        unsigned spsCount = 0;
        unsigned ppsCount = 0;
        unsigned audCount = 0;
        bool seenVcl = false;
        bool seenPictureStart = false;
        bool haveParsedFirstPicture = false;
        bool complementaryPairComplete = false;
        H264SliceState firstPicture;
        std::size_t boundary = 0;

        // Only NAL units with a following start code are known complete.
        for (std::size_t i = 0; i + 1 < starts.size(); ++i) {
            const auto& cur = starts[i];
            const std::size_t nalEnd = starts[i + 1].startCode;
            if (cur.nal >= nalEnd) continue;
            const std::uint8_t* nal = state.bytes.data() + cur.nal;
            const std::size_t nalSize = nalEnd - cur.nal;
            const std::uint8_t type = static_cast<std::uint8_t>(nal[0] & 0x1fU);

            if (type == 7) {
                ++spsCount;
                if (seenVcl || spsCount >= 2U) {
                    boundary = cur.startCode;
                    break;
                }
                parseSps(nal, nalSize);
                continue;
            }
            if (type == 8) {
                ++ppsCount;
                if (seenVcl || ppsCount >= 2U) {
                    boundary = cur.startCode;
                    break;
                }
                parsePps(nal, nalSize);
                continue;
            }
            if (type == 9) {
                ++audCount;
                if (audCount >= 2U) {
                    boundary = cur.startCode;
                    break;
                }
                continue;
            }
            if (type != 1 && type != 5) continue;

            const bool firstMbZero = h264FirstMbInSliceZero(nal, nalSize);
            H264SliceState currentPicture;
            const bool parsed = parseSlice(nal, nalSize, currentPicture);
            seenVcl = true;
            if (!firstMbZero) continue;

            if (!seenPictureStart) {
                seenPictureStart = true;
                if (parsed) {
                    firstPicture = currentPicture;
                    haveParsedFirstPicture = true;
                }
                continue;
            }

            bool complementaryField = false;
            if (!complementaryPairComplete && haveParsedFirstPicture && parsed &&
                firstPicture.fieldPic && currentPicture.fieldPic &&
                firstPicture.ppsId == currentPicture.ppsId &&
                firstPicture.frameNum == currentPicture.frameNum &&
                firstPicture.idr == currentPicture.idr &&
                firstPicture.bottomField != currentPicture.bottomField &&
                (!firstPicture.idr ||
                 firstPicture.idrPicId == currentPicture.idrPicId)) {
                complementaryField = true;
            }

            if (complementaryField) {
                complementaryPairComplete = true;
                ++state.h264FieldPairs;
                if (state.h264FieldPairs <= 4U ||
                    (state.h264FieldPairs % 250U) == 0U) {
                    std::cerr << "NATIVE AVC AU FIELD_PAIR pid="
                              << sample.stream.pid
                              << " frame_num=" << firstPicture.frameNum
                              << " first_bottom=" << (firstPicture.bottomField ? 1 : 0)
                              << " pairs=" << state.h264FieldPairs
                              << std::endl;
                }
                continue;
            }

            boundary = cur.startCode;
            break;
        }

        if (boundary == 0) return;

        DemuxSample ready;
        ready.stream = state.stream;
        ready.data.assign(state.bytes.begin(),
                          state.bytes.begin() + static_cast<std::ptrdiff_t>(boundary));
        ready.pts90k = state.pts90k;
        ready.dts90k = state.dts90k;
        ready.hasPts = state.hasPts;
        ready.hasDts = state.hasDts;
        ready.randomAccess = state.randomAccess ||
            containsRandomAccessNal(state.stream.codec, ready.data.data(), ready.data.size());

        if (sampleCallback_ && !ready.data.empty()) sampleCallback_(std::move(ready));

        state.bytes.erase(state.bytes.begin(),
                          state.bytes.begin() + static_cast<std::ptrdiff_t>(boundary));

        // V10.8.52: a standalone H.264 field picture occupies half of a
        // 25-fps PAL frame interval. If both complementary fields were kept
        // in this AU, advance one complete frame instead. Progressive and
        // frame-coded AVC retain the normal 3600-tick frame step.
        const bool singleFieldPicture =
            state.stream.codec == ElementaryCodec::H264 &&
            haveParsedFirstPicture &&
            firstPicture.fieldPic &&
            !complementaryPairComplete;

        const std::uint64_t step90k =
            singleFieldPicture ? (90000U / 50U) : (90000U / 25U);

        if (singleFieldPicture) {
            ++state.h264SingleFields;
            if (state.h264SingleFields <= 4U ||
                (state.h264SingleFields % 250U) == 0U) {
                std::cerr << "NATIVE AVC AU FIELD_CLOCK pid="
                          << sample.stream.pid
                          << " frame_num=" << firstPicture.frameNum
                          << " bottom=" << (firstPicture.bottomField ? 1 : 0)
                          << " step90k=" << step90k
                          << " fields=" << state.h264SingleFields
                          << std::endl;
            }
        }

        if (state.hasPts) state.pts90k += step90k;
        if (state.hasDts) state.dts90k += step90k;
        state.randomAccess = containsRandomAccessNal(
            state.stream.codec, state.bytes.data(), state.bytes.size());
    }
}

void NativeTsDemux::flushVideoAu(std::uint16_t pid) {
    auto it = videoAu_.find(pid);
    if (it == videoAu_.end() || it->second.bytes.empty()) return;
    auto& state = it->second;
    DemuxSample sample;
    sample.stream = state.stream;
    sample.data.swap(state.bytes);
    sample.pts90k = state.pts90k;
    sample.dts90k = state.dts90k;
    sample.hasPts = state.hasPts;
    sample.hasDts = state.hasDts;
    sample.randomAccess = state.randomAccess ||
        containsRandomAccessNal(state.stream.codec, sample.data.data(), sample.data.size());
    state = {};
    if (sampleCallback_ && !sample.data.empty()) sampleCallback_(std::move(sample));
}

} // namespace dvbstreamer5::media::mpegts
