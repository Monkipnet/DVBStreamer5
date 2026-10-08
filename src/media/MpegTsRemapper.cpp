#include "media/MpegTsRemapper.h"
#include "media/DvbText.h"

#include <algorithm>
#include <limits>

namespace dvbstreamer5::media::mpegts {
namespace {

constexpr auto kPatPmtInterval = std::chrono::milliseconds(100);
constexpr auto kSdtInterval = std::chrono::milliseconds(500);

std::uint32_t sectionCrc32(const std::uint8_t* bytes, std::size_t size) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= static_cast<std::uint32_t>(bytes[index]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) != 0
                ? (crc << 1) ^ 0x04c11db7U
                : crc << 1;
        }
    }
    return crc;
}

std::size_t payloadOffset(const Packet& packet) noexcept {
    const std::uint8_t control = static_cast<std::uint8_t>((packet[3] >> 4) & 0x03U);
    if ((control & 0x01U) == 0) return kPacketSize;
    if ((control & 0x02U) == 0) return 4;
    const std::size_t offset = 5 + packet[4];
    return offset < kPacketSize ? offset : kPacketSize;
}

std::uint16_t packetPid(const Packet& packet) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8) | packet[2]);
}

std::uint16_t readPid(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0] & 0x1fU) << 8) | bytes[1]);
}

void writePid(std::uint8_t* bytes, std::uint16_t pid) noexcept {
    bytes[0] = static_cast<std::uint8_t>((bytes[0] & 0xe0U) | ((pid >> 8) & 0x1fU));
    bytes[1] = static_cast<std::uint8_t>(pid & 0xffU);
}

void writeCrc(std::uint8_t* section, std::size_t sectionSize) noexcept {
    const std::uint32_t crc = sectionCrc32(section, sectionSize - 4);
    section[sectionSize - 4] = static_cast<std::uint8_t>(crc >> 24);
    section[sectionSize - 3] = static_cast<std::uint8_t>(crc >> 16);
    section[sectionSize - 2] = static_cast<std::uint8_t>(crc >> 8);
    section[sectionSize - 1] = static_cast<std::uint8_t>(crc);
}

bool sectionView(Packet& packet, std::uint8_t expectedTableId,
                 std::uint8_t*& section, std::size_t& sectionSize,
                 std::string& error) {
    section = nullptr;
    sectionSize = 0;
    if (packet[0] != kSyncByte || (packet[1] & 0x40U) == 0) return true;
    const std::size_t payload = payloadOffset(packet);
    if (payload >= kPacketSize || payload + 1 >= kPacketSize) return true;
    const std::size_t start = payload + 1 + packet[payload];
    if (start + 3 > kPacketSize) return true;
    section = packet.data() + start;
    if (section[0] != expectedTableId) return true;
    sectionSize = 3 + static_cast<std::size_t>(
        ((section[1] & 0x0fU) << 8) | section[2]);
    if (sectionSize > kPacketSize - start) {
        error = "native remap requires PAT, PMT, and SDT sections to fit in one TS packet";
        section = nullptr;
        sectionSize = 0;
        return false;
    }
    return true;
}

bool isVideo(std::uint8_t type) noexcept {
    switch (type) {
        case 0x01:
        case 0x02:
        case 0x10:
        case 0x1b:
        case 0x24:
        case 0x42:
            return true;
        default:
            return false;
    }
}

bool isAudio(std::uint8_t type) noexcept {
    switch (type) {
        case 0x03:
        case 0x04:
        case 0x0f:
        case 0x11:
        case 0x81:
        case 0x87:
            return true;
        default:
            return false;
    }
}

bool addCaPids(const std::uint8_t* descriptors, std::size_t size,
               std::array<bool, 8192>& allowed) {
    std::size_t offset = 0;
    while (offset + 2 <= size) {
        const std::uint8_t tag = descriptors[offset++];
        const std::size_t length = descriptors[offset++];
        if (offset + length > size) return false;
        if (tag == 0x09 && length >= 4) {
            const std::uint16_t pid = readPid(descriptors + offset + 2);
            if (pid < 0x1fff) allowed[pid] = true;
        }
        offset += length;
    }
    return offset == size;
}

std::size_t boundedText(const std::string& text, std::uint8_t* output,
                        std::size_t maximum) {
    const auto encoded = dvbtext::encode(text, maximum);
    std::copy(encoded.begin(), encoded.end(), output);
    return encoded.size();
}

} // namespace

bool Remapper::initialize(const RemapConfig& config, std::string& error) {
    error.clear();
    if (config.outputServiceId == 0) {
        error = "native remap requires an output service ID from 1 to 65535";
        return false;
    }
    const auto validOutputPid = [](std::uint16_t pid) {
        return pid == 0 || (pid >= 0x20 && pid < 0x1fff);
    };
    if (!validOutputPid(config.outputVideoPid) ||
        !validOutputPid(config.outputAudioPid) ||
        (config.outputVideoPid != 0 && config.outputVideoPid == config.outputAudioPid)) {
        error = "native remap output PIDs must be distinct values from 32 to 8190";
        return false;
    }

    config_ = config;
    inputServiceId_ = config.inputServiceId;
    pmtPid_ = 0x1fff;
    inputVideoPid_ = 0;
    inputAudioPid_ = 0;
    transportStreamId_ = 1;
    originalNetworkId_ = 1;
    patOutputContinuity_ = 0;
    pmtOutputContinuity_ = 0;
    sdtOutputContinuity_ = 0;
    patSection_ = {};
    catSection_ = {};
    pmtSection_ = {};
    sdtSection_ = {};
    pmtOutputSection_.clear();
    nextPatPmtAt_ = {};
    nextSdtAt_ = {};
    allowedPids_.fill(false);
    caPids_.fill(false);
    inputAdmissionPids_.fill(false);
    inputAdmissionPids_[0x0000] = true;
    inputAdmissionPids_[0x0001] = true;
    remapReady_ = false;
    initialized_ = true;
    return true;
}

bool Remapper::collectSections(
    const Packet& input,
    PsiSectionState& state,
    std::vector<std::vector<std::uint8_t>>& sections,
    std::string& error) {
    PacketInfo info;
    if (!inspectPacket(input.data(), input.size(), info)) {
        error = "native remap received an invalid PSI MPEG-TS packet";
        return false;
    }
    if (info.transportError) {
        state = {};
        return true;
    }
    if (!info.hasPayload || info.payloadOffset >= kPacketSize) {
        return true;
    }

    if (state.continuityValid && !info.discontinuity) {
        const std::uint8_t expected =
            static_cast<std::uint8_t>((state.continuity + 1U) & 0x0fU);
        if (info.continuityCounter == state.continuity) {
            // Duplicate PSI packet. Keep the existing partial section and do
            // not append the same bytes twice.
            return true;
        }
        if (info.continuityCounter != expected) {
            // A lost PSI packet invalidates only the partial table. The next
            // payload-unit start will rebuild it; the live channel must stay up.
            state.bytes.clear();
            state.expected = 0;
        }
    }
    state.continuity = info.continuityCounter;
    state.continuityValid = true;

    const std::uint8_t* payload = input.data() + info.payloadOffset;
    std::size_t size = kPacketSize - info.payloadOffset;

    auto appendPartial = [&](const std::uint8_t* data, std::size_t count) {
        if (state.bytes.empty() || count == 0) return std::size_t{0};

        std::size_t consumed = 0;
        if (state.expected == 0) {
            const std::size_t needHeader =
                state.bytes.size() < 3 ? 3 - state.bytes.size() : 0;
            const std::size_t take = std::min(needHeader, count);
            state.bytes.insert(state.bytes.end(), data, data + take);
            consumed += take;
            if (state.bytes.size() < 3) return consumed;

            const std::size_t sectionLength = static_cast<std::size_t>(
                ((state.bytes[1] & 0x0fU) << 8) | state.bytes[2]);
            state.expected = 3 + sectionLength;
            if (state.expected < 3 || state.expected > 4096) {
                state.bytes.clear();
                state.expected = 0;
                return consumed;
            }
        }

        if (consumed < count && state.expected > state.bytes.size()) {
            const std::size_t take = std::min(
                state.expected - state.bytes.size(), count - consumed);
            state.bytes.insert(
                state.bytes.end(), data + consumed, data + consumed + take);
            consumed += take;
        }

        if (state.expected != 0 && state.bytes.size() == state.expected) {
            sections.push_back(state.bytes);
            state.bytes.clear();
            state.expected = 0;
        }
        return consumed;
    };

    std::size_t offset = 0;
    if (info.payloadUnitStart) {
        if (size == 0) return true;
        const std::size_t pointer = payload[0];
        offset = 1;
        if (offset + pointer > size) {
            state.bytes.clear();
            state.expected = 0;
            return true;
        }

        if (!state.bytes.empty() && pointer > 0) {
            appendPartial(payload + offset, pointer);
        }
        offset += pointer;

        // pointer_field ends the previous section. If it did not become
        // complete, discard it rather than joining it with a new section.
        if (!state.bytes.empty()) {
            state.bytes.clear();
            state.expected = 0;
        }
    } else {
        if (state.bytes.empty()) return true;
        appendPartial(payload, size);
        return true;
    }

    while (offset < size) {
        if (payload[offset] == 0xffU) break;
        if (size - offset < 3) {
            state.bytes.assign(payload + offset, payload + size);
            state.expected = 0;
            break;
        }

        const std::size_t sectionLength = static_cast<std::size_t>(
            ((payload[offset + 1] & 0x0fU) << 8) | payload[offset + 2]);
        const std::size_t total = 3 + sectionLength;
        if (total < 3 || total > 4096) {
            // Invalid/stuffing-like bytes: abandon this payload and wait for
            // the next payload-unit start instead of stopping the stream.
            state.bytes.clear();
            state.expected = 0;
            break;
        }
        if (size - offset >= total) {
            sections.emplace_back(payload + offset, payload + offset + total);
            offset += total;
            continue;
        }

        state.bytes.assign(payload + offset, payload + size);
        state.expected = total;
        break;
    }
    return true;
}

bool Remapper::processCatSection(
    const std::vector<std::uint8_t>& section,
    std::string& error) {
    if (section.size() < 12 || section[0] != 0x01) return true;

    // V10.8.130: CAT normally carries only a handful of CA descriptors. The
    // previous path cleared an 8192-entry bitmap and then scanned all 8192 PIDs
    // twice for every repeated CAT section. Collect only the descriptor PIDs,
    // validate the whole section first, then commit them transactionally.
    // A section is capped at 4096 bytes by collectSections(); a CA descriptor
    // needs at least tag+length+4 bytes, so 682 entries safely cover the limit.
    std::array<std::uint16_t, 682> foundPids;
    std::size_t foundCount = 0;
    const std::uint8_t* descriptors = section.data() + 8;
    const std::size_t descriptorBytes = section.size() - 12;
    std::size_t offset = 0;
    while (offset + 2 <= descriptorBytes) {
        const std::uint8_t tag = descriptors[offset++];
        const std::size_t length = descriptors[offset++];
        if (offset + length > descriptorBytes) {
            error = "native remap encountered malformed CAT descriptors";
            return false;
        }
        if (tag == 0x09 && length >= 4) {
            const std::uint16_t pid = readPid(descriptors + offset + 2);
            if (pid < 0x1fff) {
                if (foundCount >= foundPids.size()) {
                    error = "native remap encountered too many CAT CA descriptors";
                    return false;
                }
                foundPids[foundCount++] = pid;
            }
        }
        offset += length;
    }
    if (offset != descriptorBytes) {
        error = "native remap encountered malformed CAT descriptors";
        return false;
    }

    if (remapReady_) {
        allowedPids_[0x01] = true;
        inputAdmissionPids_[0x01] = true;
    }
    for (std::size_t index = 0; index < foundCount; ++index) {
        const std::uint16_t pid = foundPids[index];
        caPids_[pid] = true;
        if (remapReady_) {
            allowedPids_[pid] = true;
            inputAdmissionPids_[pid] = true;
        }
    }
    return true;
}

bool Remapper::processPatSection(
    const std::vector<std::uint8_t>& section,
    std::string& error) {
    (void)error;
    if (section.size() < 12 || section[0] != 0x00) return true;

    transportStreamId_ = static_cast<std::uint16_t>(
        (section[3] << 8) | section[4]);
    const std::size_t entriesEnd = section.size() - 4;
    std::uint16_t selectedService = 0;
    std::uint16_t selectedPmtPid = 0x1fff;
    for (std::size_t offset = 8; offset + 4 <= entriesEnd; offset += 4) {
        const std::uint16_t service = static_cast<std::uint16_t>(
            (section[offset] << 8) | section[offset + 1]);
        const std::uint16_t pid = readPid(section.data() + offset + 2);
        if (service == 0 || pid == 0 || pid >= 0x1fff) continue;
        if (inputServiceId_ == 0 || inputServiceId_ == service) {
            selectedService = service;
            selectedPmtPid = pid;
            break;
        }
    }

    // PAT itself may span multiple packets or sections. Do not fail just
    // because the selected service is not in this particular section.
    if (selectedService != 0 &&
        (inputServiceId_ != selectedService || pmtPid_ != selectedPmtPid)) {
        inputServiceId_ = selectedService;
        pmtPid_ = selectedPmtPid;
        inputVideoPid_ = 0;
        inputAudioPid_ = 0;
        pmtSection_ = {};
        pmtOutputSection_.clear();
        pmtOutputContinuity_ = 0;
        nextPatPmtAt_ = {};
        nextSdtAt_ = {};
        allowedPids_.fill(false);
        inputAdmissionPids_.fill(false);
        inputAdmissionPids_[0x0000] = true;
        inputAdmissionPids_[0x0001] = true;
        inputAdmissionPids_[pmtPid_] = true;
        remapReady_ = false;
    }
    return true;
}

void Remapper::packetizeSection(
    std::uint16_t pid,
    const std::vector<std::uint8_t>& section,
    std::uint8_t& continuity,
    std::vector<Packet>& output) {
    if (section.empty() || pid >= 0x1fff) return;

    std::size_t offset = 0;
    bool first = true;
    while (offset < section.size()) {
        Packet packet;
        packet.fill(0xff);
        packet[0] = kSyncByte;
        packet[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1fU);
        if (first) packet[1] |= 0x40U;
        packet[2] = static_cast<std::uint8_t>(pid);
        packet[3] = static_cast<std::uint8_t>(0x10U | (continuity & 0x0fU));
        continuity = static_cast<std::uint8_t>((continuity + 1U) & 0x0fU);

        std::size_t payloadOffset = 4;
        if (first) {
            packet[payloadOffset++] = 0; // pointer_field
        }
        const std::size_t take = std::min(
            kPacketSize - payloadOffset, section.size() - offset);
        std::copy(
            section.begin() + static_cast<std::ptrdiff_t>(offset),
            section.begin() + static_cast<std::ptrdiff_t>(offset + take),
            packet.begin() + static_cast<std::ptrdiff_t>(payloadOffset));
        offset += take;
        output.push_back(std::move(packet));
        first = false;
    }
}

bool Remapper::processPmtSection(
    std::vector<std::uint8_t> section,
    std::vector<Packet>& output,
    std::string& error) {
    (void)output;
    if (section.size() < 16 || section[0] != 0x02 ||
        static_cast<std::uint16_t>((section[3] << 8) | section[4]) != inputServiceId_) {
        return true;
    }

    std::array<bool, 8192> allowed {};
    allowed[0] = true;
    allowed[0x01] = true;
    allowed[0x11] = true;
    allowed[pmtPid_] = true;
    allowed[kNullPid] = true;
    for (std::size_t pid = 0; pid < caPids_.size(); ++pid) {
        if (caPids_[pid]) allowed[pid] = true;
    }

    std::uint16_t inputVideo = 0;
    std::uint16_t inputAudio = 0;
    const std::size_t end = section.size() - 4;
    std::uint16_t pcrPid = readPid(section.data() + 8);
    if (pcrPid < 0x1fff) allowed[pcrPid] = true;

    const std::size_t programInfoLength = static_cast<std::size_t>(
        ((section[10] & 0x0fU) << 8) | section[11]);
    std::size_t offset = 12;
    if (offset + programInfoLength > end ||
        !addCaPids(section.data() + offset, programInfoLength, allowed)) {
        error = "native remap encountered malformed PMT program descriptors";
        return false;
    }
    offset += programInfoLength;

    while (offset + 5 <= end) {
        const std::uint8_t streamType = section[offset];
        std::uint16_t pid = readPid(section.data() + offset + 1);
        const std::size_t infoLength = static_cast<std::size_t>(
            ((section[offset + 3] & 0x0fU) << 8) | section[offset + 4]);
        if (offset + 5 + infoLength > end) {
            error = "native remap encountered malformed PMT elementary stream data";
            return false;
        }
        if (pid < allowed.size()) allowed[pid] = true;
        if (inputVideo == 0 && isVideo(streamType)) inputVideo = pid;
        if (inputAudio == 0 && isAudio(streamType)) inputAudio = pid;
        if (!addCaPids(section.data() + offset + 5, infoLength, allowed)) {
            error = "native remap encountered malformed elementary stream descriptors";
            return false;
        }
        offset += 5 + infoLength;
    }
    if (offset != end) {
        error = "native remap encountered trailing bytes in PMT";
        return false;
    }

    if ((config_.outputVideoPid != 0 && inputVideo == 0) ||
        (config_.outputAudioPid != 0 && inputAudio == 0)) {
        error = "native remap requested an audio or video PID absent from the selected PMT";
        return false;
    }
    if ((inputVideo != 0 && inputVideo < 0x20) ||
        (inputAudio != 0 && inputAudio < 0x20)) {
        error = "native remap encountered an invalid elementary stream PID";
        return false;
    }
    if ((config_.outputVideoPid != 0 && allowed[config_.outputVideoPid] &&
            config_.outputVideoPid != inputVideo) ||
        (config_.outputAudioPid != 0 && allowed[config_.outputAudioPid] &&
            config_.outputAudioPid != inputAudio)) {
        error = "native remap output PID conflicts with another selected-program PID";
        return false;
    }

    inputVideoPid_ = inputVideo;
    inputAudioPid_ = inputAudio;
    section[3] = static_cast<std::uint8_t>(config_.outputServiceId >> 8);
    section[4] = static_cast<std::uint8_t>(config_.outputServiceId);

    if (pcrPid == inputVideo && config_.outputVideoPid != 0) {
        pcrPid = config_.outputVideoPid;
    }
    if (pcrPid == inputAudio && config_.outputAudioPid != 0) {
        pcrPid = config_.outputAudioPid;
    }
    writePid(section.data() + 8, pcrPid);

    offset = 12 + programInfoLength;
    while (offset + 5 <= end) {
        std::uint16_t pid = readPid(section.data() + offset + 1);
        if (pid == inputVideo && config_.outputVideoPid != 0) {
            pid = config_.outputVideoPid;
        }
        if (pid == inputAudio && config_.outputAudioPid != 0) {
            pid = config_.outputAudioPid;
        }
        writePid(section.data() + offset + 1, pid);
        offset += 5 + static_cast<std::size_t>(
            ((section[offset + 3] & 0x0fU) << 8) | section[offset + 4]);
    }
    writeCrc(section.data(), section.size());

    if (config_.outputVideoPid != 0 && config_.outputVideoPid != inputVideo) {
        allowed[config_.outputVideoPid] = true;
        allowed[inputVideo] = false;
    }
    if (config_.outputAudioPid != 0 && config_.outputAudioPid != inputAudio) {
        allowed[config_.outputAudioPid] = true;
        allowed[inputAudio] = false;
    }

    allowedPids_ = allowed;
    inputAdmissionPids_ = allowed;
    if (inputVideoPid_ < inputAdmissionPids_.size()) {
        inputAdmissionPids_[inputVideoPid_] = true;
    }
    if (inputAudioPid_ < inputAdmissionPids_.size()) {
        inputAdmissionPids_[inputAudioPid_] = true;
    }
    pmtOutputSection_ = std::move(section);
    const bool becameReady = !remapReady_;
    remapReady_ = true;
    if (becameReady) {
        // Emit a complete PSI/SI set on the very next TS packet, then keep
        // repeating it from our own monotonic clock. This avoids long gaps
        // when the provider repeats PAT/PMT/SDT irregularly.
        nextPatPmtAt_ = {};
        nextSdtAt_ = {};
    }
    return true;
}

bool Remapper::processSdtSection(
    const std::vector<std::uint8_t>& section,
    std::vector<Packet>& output,
    std::string& error) {
    (void)output;
    (void)error;
    if (section.size() < 15 || section[0] != 0x42) return true;

    originalNetworkId_ = static_cast<std::uint16_t>(
        (section[8] << 8) | section[9]);
    // The regenerated SDT content changed; advertise it promptly instead of
    // waiting for the normal 500 ms repeat deadline.
    nextSdtAt_ = {};
    return true;
}

void Remapper::emitPeriodicPsi(std::vector<Packet>& output) {
    if (!remapReady_ || pmtPid_ == 0x1fff || pmtOutputSection_.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    if (nextPatPmtAt_ == std::chrono::steady_clock::time_point{} ||
        now >= nextPatPmtAt_) {
        output.push_back(makePat(patOutputContinuity_));
        patOutputContinuity_ =
            static_cast<std::uint8_t>((patOutputContinuity_ + 1U) & 0x0fU);
        packetizeSection(
            pmtPid_, pmtOutputSection_, pmtOutputContinuity_, output);
        nextPatPmtAt_ = now + kPatPmtInterval;
    }

    if (nextSdtAt_ == std::chrono::steady_clock::time_point{} ||
        now >= nextSdtAt_) {
        output.push_back(makeSdt(sdtOutputContinuity_));
        sdtOutputContinuity_ =
            static_cast<std::uint8_t>((sdtOutputContinuity_ + 1U) & 0x0fU);
        nextSdtAt_ = now + kSdtInterval;
    }
}

void Remapper::tick(std::vector<Packet>& output) {
    emitPeriodicPsi(output);
}

bool Remapper::isAllowed(std::uint16_t pid) const noexcept {
    return pid == kNullPid || (pid < allowedPids_.size() && allowedPids_[pid]);
}

Packet Remapper::makePat(std::uint8_t continuity) const {
    Packet packet;
    packet.fill(0xff);
    packet[0] = kSyncByte;
    packet[1] = 0x40;
    packet[2] = 0;
    packet[3] = static_cast<std::uint8_t>(0x10U | (continuity & 0x0fU));
    packet[4] = 0;
    std::uint8_t* section = packet.data() + 5;
    section[0] = 0x00;
    section[1] = 0xb0;
    section[2] = 13;
    section[3] = static_cast<std::uint8_t>(transportStreamId_ >> 8);
    section[4] = static_cast<std::uint8_t>(transportStreamId_);
    section[5] = 0xc1;
    section[6] = 0;
    section[7] = 0;
    section[8] = static_cast<std::uint8_t>(config_.outputServiceId >> 8);
    section[9] = static_cast<std::uint8_t>(config_.outputServiceId);
    section[10] = static_cast<std::uint8_t>(0xe0U | ((pmtPid_ >> 8) & 0x1fU));
    section[11] = static_cast<std::uint8_t>(pmtPid_);
    writeCrc(section, 16);
    return packet;
}

Packet Remapper::makeSdt(std::uint8_t continuity) const {
    Packet packet;
    packet.fill(0xff);
    packet[0] = kSyncByte;
    packet[1] = 0x40;
    packet[2] = 0x11;
    packet[3] = static_cast<std::uint8_t>(0x10U | (continuity & 0x0fU));
    packet[4] = 0;

    std::array<std::uint8_t, 120> descriptors {};
    std::size_t pos = 0;
    descriptors[pos++] = 0x48;
    const std::size_t descriptorLengthOffset = pos++;
    descriptors[pos++] = 0x01;
    const std::size_t providerLengthOffset = pos++;
    const std::size_t providerLength =
        boundedText(config_.serviceProvider, descriptors.data() + pos, 48);
    descriptors[providerLengthOffset] = static_cast<std::uint8_t>(providerLength);
    pos += providerLength;
    const std::size_t nameLengthOffset = pos++;
    const std::string fallbackName = "Service " + std::to_string(config_.outputServiceId);
    const std::string& name = config_.serviceName.empty() ? fallbackName : config_.serviceName;
    const std::size_t nameLength = boundedText(name, descriptors.data() + pos, 48);
    descriptors[nameLengthOffset] = static_cast<std::uint8_t>(nameLength);
    pos += nameLength;
    descriptors[descriptorLengthOffset] = static_cast<std::uint8_t>(pos - 2);

    std::uint8_t* section = packet.data() + 5;
    section[0] = 0x42;
    section[1] = 0xf0;
    const std::size_t sectionLength = 8 + 5 + pos + 4;
    section[2] = static_cast<std::uint8_t>(sectionLength);
    section[3] = static_cast<std::uint8_t>(transportStreamId_ >> 8);
    section[4] = static_cast<std::uint8_t>(transportStreamId_);
    section[5] = 0xc1;
    section[6] = 0;
    section[7] = 0;
    section[8] = static_cast<std::uint8_t>(originalNetworkId_ >> 8);
    section[9] = static_cast<std::uint8_t>(originalNetworkId_);
    section[10] = 0xff;
    std::size_t offset = 11;
    section[offset++] = static_cast<std::uint8_t>(config_.outputServiceId >> 8);
    section[offset++] = static_cast<std::uint8_t>(config_.outputServiceId);
    section[offset++] = 0xfc;
    section[offset++] = static_cast<std::uint8_t>(0x80U | ((pos >> 8) & 0x0fU));
    section[offset++] = static_cast<std::uint8_t>(pos);
    std::copy(descriptors.begin(), descriptors.begin() + static_cast<std::ptrdiff_t>(pos),
              section + offset);
    offset += pos;
    writeCrc(section, offset + 4);
    return packet;
}

bool Remapper::process(const Packet& input, std::vector<Packet>& output, std::string& error) {
    error.clear();
    if (!initialized_) {
        error = "native remapper is not initialized";
        return false;
    }

    PacketInfo info;
    if (!inspectPacket(input.data(), input.size(), info)) {
        error = "native remapper received an invalid MPEG-TS packet";
        return false;
    }
    return processValidated(input, info, output, error);
}

bool Remapper::processTrusted(
    const Packet& input, const PacketInfo& info,
    std::vector<Packet>& output, std::string& error) {
    error.clear();
    if (!initialized_) {
        error = "native remapper is not initialized";
        return false;
    }
    // V10.8.133: shared-DVB already performed inspectPacket() immediately
    // before this call. Reuse that immutable PacketInfo instead of validating
    // the same selected-service packet a second time.
    return processValidated(input, info, output, error);
}

bool Remapper::processValidated(
    const Packet& input, const PacketInfo& info,
    std::vector<Packet>& output, std::string& error) {
    // Keep the service discoverable independently of the provider table
    // cadence. The CBR pacer downstream absorbs this tiny deterministic PSI
    // overhead with its normal NULL stuffing; media/PCR handling is untouched.
    emitPeriodicPsi(output);

    std::vector<std::vector<std::uint8_t>> sections;
    if (info.pid == 0x0000) {
        if (!collectSections(input, patSection_, sections, error)) return false;
        for (const auto& section : sections) {
            if (!section.empty() && section[0] == 0x00) {
                if (!processPatSection(section, error)) return false;
            }
        }
        return true;
    }

    if (info.pid == 0x0001) {
        if (!collectSections(input, catSection_, sections, error)) return false;
        for (const auto& section : sections) {
            if (!processCatSection(section, error)) return false;
        }
        // Keep the broadcaster's complete CAT packets. This preserves EMM
        // descriptors while the assembled copy above learns their PIDs.
        if (remapReady_) output.push_back(input);
        return true;
    }

    if (info.pid == pmtPid_ && pmtPid_ != 0x1fff) {
        if (!collectSections(input, pmtSection_, sections, error)) return false;
        for (auto& section : sections) {
            if (!processPmtSection(std::move(section), output, error)) return false;
        }
        return true;
    }

    if (!remapReady_) return true;

    if (info.pid == 0x0011) {
        if (!collectSections(input, sdtSection_, sections, error)) return false;
        for (const auto& section : sections) {
            if (!processSdtSection(section, output, error)) return false;
        }
        return true;
    }

    const bool mappedMediaPid =
        info.pid == inputVideoPid_ || info.pid == inputAudioPid_;
    if (!isAllowed(info.pid) && !mappedMediaPid) return true;

    Packet packet = input;
    std::uint16_t mappedPid = info.pid;
    if (info.pid != 0x1fff && config_.outputVideoPid != 0 &&
        info.pid == inputVideoPid_) {
        mappedPid = config_.outputVideoPid;
    }
    if (info.pid != 0x1fff && config_.outputAudioPid != 0 &&
        info.pid == inputAudioPid_) {
        mappedPid = config_.outputAudioPid;
    }
    if (info.pid != mappedPid) {
        rewritePid(packet.data(), packet.size(), mappedPid);
    }
    output.push_back(std::move(packet));
    return true;
}

} // namespace dvbstreamer5::media::mpegts
