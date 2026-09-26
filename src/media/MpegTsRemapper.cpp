#include "media/MpegTsRemapper.h"

#include <algorithm>
#include <limits>

namespace tvs::media::mpegts {
namespace {

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
    const std::size_t size = (std::min)(text.size(), maximum);
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        output[i] = ch >= 0x20 && ch <= 0x7e ? ch : static_cast<std::uint8_t>('?');
    }
    return size;
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
    patContinuity_ = 0;
    allowedPids_.fill(false);
    remapReady_ = false;
    initialized_ = true;
    return true;
}

bool Remapper::processPat(const Packet& input, std::string& error) {
    auto packet = input;
    std::uint8_t* section = nullptr;
    std::size_t size = 0;
    if (!sectionView(packet, 0x00, section, size, error) || !section) return error.empty();
    if (size < 12) return true;
    if (section[6] != 0 || section[7] != 0) {
        error = "native remap does not support multi-section PAT tables";
        return false;
    }

    transportStreamId_ = static_cast<std::uint16_t>((section[3] << 8) | section[4]);
    patContinuity_ = static_cast<std::uint8_t>(input[3] & 0x0fU);
    const std::size_t entriesEnd = size - 4;
    std::uint16_t selectedService = 0;
    std::uint16_t selectedPmtPid = 0x1fff;
    for (std::size_t offset = 8; offset + 4 <= entriesEnd; offset += 4) {
        const std::uint16_t service = static_cast<std::uint16_t>(
            (section[offset] << 8) | section[offset + 1]);
        const std::uint16_t pid = readPid(section + offset + 2);
        if (service == 0 || pid == 0 || pid >= 0x1fff) continue;
        if (inputServiceId_ == 0 || inputServiceId_ == service) {
            selectedService = service;
            selectedPmtPid = pid;
            break;
        }
    }
    if (selectedService != 0 && (inputServiceId_ != selectedService || pmtPid_ != selectedPmtPid)) {
        inputServiceId_ = selectedService;
        pmtPid_ = selectedPmtPid;
        inputVideoPid_ = 0;
        inputAudioPid_ = 0;
        allowedPids_.fill(false);
        remapReady_ = false;
    } else if (selectedService == 0 && inputServiceId_ != 0) {
        error = "native remap input service was not found in the PAT";
        return false;
    }
    return true;
}

bool Remapper::processPmt(const Packet& input,
                          std::vector<Packet>& output,
                          std::string& error) {
    const bool wasReady = remapReady_;
    Packet packet = input;
    std::uint8_t* section = nullptr;
    std::size_t size = 0;
    if (!sectionView(packet, 0x02, section, size, error) || !section) return error.empty();
    if (size < 16 ||
        packetPid(packet) != pmtPid_ ||
        static_cast<std::uint16_t>((section[3] << 8) | section[4]) != inputServiceId_) {
        return true;
    }
    if (section[6] != 0 || section[7] != 0) {
        error = "native remap does not support multi-section PMT tables";
        return false;
    }

    std::array<bool, 8192> allowed {};
    allowed[0] = true;
    allowed[0x11] = true;
    allowed[pmtPid_] = true;
    allowed[kNullPid] = true;
    std::uint16_t inputVideo = 0;
    std::uint16_t inputAudio = 0;

    const std::size_t end = size - 4;
    std::uint16_t pcrPid = readPid(section + 8);
    if (pcrPid < 0x1fff) allowed[pcrPid] = true;
    const std::size_t programInfoLength = static_cast<std::size_t>(
        ((section[10] & 0x0fU) << 8) | section[11]);
    std::size_t offset = 12;
    if (offset + programInfoLength > end ||
        !addCaPids(section + offset, programInfoLength, allowed)) {
        error = "native remap encountered malformed PMT program descriptors";
        return false;
    }
    offset += programInfoLength;

    while (offset + 5 <= end) {
        const std::uint8_t streamType = section[offset];
        std::uint16_t pid = readPid(section + offset + 1);
        const std::size_t infoLength = static_cast<std::size_t>(
            ((section[offset + 3] & 0x0fU) << 8) | section[offset + 4]);
        if (offset + 5 + infoLength > end) {
            error = "native remap encountered malformed PMT elementary stream data";
            return false;
        }
        allowed[pid] = true;
        if (inputVideo == 0 && isVideo(streamType)) inputVideo = pid;
        if (inputAudio == 0 && isAudio(streamType)) inputAudio = pid;
        if (!addCaPids(section + offset + 5, infoLength, allowed)) {
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
    if (pcrPid == inputVideo && config_.outputVideoPid != 0) pcrPid = config_.outputVideoPid;
    if (pcrPid == inputAudio && config_.outputAudioPid != 0) pcrPid = config_.outputAudioPid;
    writePid(section + 8, pcrPid);
    offset = 12 + programInfoLength;
    while (offset + 5 <= end) {
        std::uint16_t pid = readPid(section + offset + 1);
        if (pid == inputVideo && config_.outputVideoPid != 0) pid = config_.outputVideoPid;
        if (pid == inputAudio && config_.outputAudioPid != 0) pid = config_.outputAudioPid;
        writePid(section + offset + 1, pid);
        offset += 5 + static_cast<std::size_t>(
            ((section[offset + 3] & 0x0fU) << 8) | section[offset + 4]);
    }
    writeCrc(section, size);

    if (config_.outputVideoPid != 0 && config_.outputVideoPid != inputVideo) {
        allowed[config_.outputVideoPid] = true;
        allowed[inputVideo] = false;
    }
    if (config_.outputAudioPid != 0 && config_.outputAudioPid != inputAudio) {
        allowed[config_.outputAudioPid] = true;
        allowed[inputAudio] = false;
    }
    allowedPids_ = allowed;
    remapReady_ = true;
    if (!wasReady) output.push_back(makePat(patContinuity_));
    output.push_back(packet);
    return true;
}

bool Remapper::processSdt(const Packet& input, Packet& output, std::string& error) {
    auto packet = input;
    std::uint8_t* section = nullptr;
    std::size_t size = 0;
    if (!sectionView(packet, 0x42, section, size, error)) return false;
    if (!section) {
        output = packet;
        return true;
    }
    if (size < 15) {
        output = packet;
        return true;
    }
    originalNetworkId_ = static_cast<std::uint16_t>((section[8] << 8) | section[9]);
    output = makeSdt(static_cast<std::uint8_t>(input[3] & 0x0fU));
    return true;
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

    if (info.pid == 0) {
        if (!processPat(input, error)) return false;
        if (remapReady_) output.push_back(makePat(info.continuityCounter));
        return true;
    }
    if (info.pid == pmtPid_ && pmtPid_ != 0x1fff) {
        return processPmt(input, output, error);
    }
    if (!remapReady_) return true;
    const bool mappedMediaPid = info.pid == inputVideoPid_ || info.pid == inputAudioPid_;
    if (!isAllowed(info.pid) && !mappedMediaPid) return true;
    if (info.pid == 0x11) {
        Packet rewritten;
        if (!processSdt(input, rewritten, error)) return false;
        if (rewritten[0] == kSyncByte) output.push_back(std::move(rewritten));
        return true;
    }

    Packet packet = input;
    std::uint16_t mappedPid = info.pid;
    if (info.pid != 0x1fff && config_.outputVideoPid != 0 &&
        info.pid == inputVideoPid_) mappedPid = config_.outputVideoPid;
    if (info.pid != 0x1fff && config_.outputAudioPid != 0 &&
        info.pid == inputAudioPid_) mappedPid = config_.outputAudioPid;
    if (info.pid != mappedPid) rewritePid(packet.data(), packet.size(), mappedPid);
    output.push_back(std::move(packet));
    return true;
}

} // namespace tvs::media::mpegts
