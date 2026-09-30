#include "media/NativeTsDemux.h"

#include <algorithm>
#include <cstring>
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
    programNumber_ = 0;
    pmtPid_ = 0xffff;
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
        case 0x0f:
        case 0x11: return ElementaryCodec::AacAdts;
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
    const std::uint8_t flags = b[7];
    const std::size_t headerLength = b[8];
    const std::size_t payloadOffset = 9 + headerLength;
    if (payloadOffset > b.size()) return false;
    sample.stream = pes.stream;
    sample.hasPts = (flags & 0x80U) != 0 && headerLength >= 5;
    sample.hasDts = (flags & 0x40U) != 0 && headerLength >= 10;
    if (sample.hasPts) sample.pts90k = readPts(b.data() + 9);
    if (sample.hasDts) sample.dts90k = readPts(b.data() + 14);
    else if (sample.hasPts) sample.dts90k = sample.pts90k;
    sample.data.assign(b.begin() + static_cast<std::ptrdiff_t>(payloadOffset), b.end());
    sample.randomAccess = pes.randomAccessHint || containsRandomAccessNal(pes.stream.codec, sample.data.data(), sample.data.size());
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
    DemuxSample sample;
    if (parsePes(current, sample) && sampleCallback_) sampleCallback_(std::move(sample));
}

} // namespace dvbstreamer5::media::mpegts
