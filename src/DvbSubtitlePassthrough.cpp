#include "DvbSubtitlePassthrough.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <utility>

namespace {
constexpr std::size_t kPacketSize = 188;
constexpr std::size_t kMaxPsi = 1024;
constexpr std::size_t kMaxQueuedPackets = 4096;
constexpr std::size_t kMaxFlushPackets = 96;
constexpr std::size_t kMaxSubtitleTracks = 8;
std::uint16_t u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8U) | p[1]);
}
std::uint16_t esPid(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(((p[0] & 0x1fU) << 8U) | p[1]);
}
void appendCrc(std::vector<std::uint8_t>& s) {
    std::uint32_t crc = 0xffffffffU;
    for (std::uint8_t byte : s) {
        crc ^= static_cast<std::uint32_t>(byte) << 24U;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x80000000U) ? (crc << 1U) ^ 0x04c11db7U : crc << 1U;
    }
    s.push_back(static_cast<std::uint8_t>(crc >> 24U));
    s.push_back(static_cast<std::uint8_t>(crc >> 16U));
    s.push_back(static_cast<std::uint8_t>(crc >> 8U));
    s.push_back(static_cast<std::uint8_t>(crc));
}
} // namespace

std::uint16_t DvbSubtitlePassthrough::packetPid(const Packet& packet) {
    return static_cast<std::uint16_t>(((packet[1] & 0x1fU) << 8U) | packet[2]);
}
std::size_t DvbSubtitlePassthrough::payloadOffset(const Packet& packet) {
    const auto ctrl = static_cast<unsigned>((packet[3] >> 4U) & 3U);
    if ((ctrl & 1U) == 0U) return kPacketSize;
    if ((ctrl & 2U) == 0U) return 4U;
    return 5U + packet[4] < kPacketSize ? 5U + packet[4] : kPacketSize;
}
std::uint32_t DvbSubtitlePassthrough::crc32(
    const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24U;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000U) ? (crc << 1U) ^ 0x04c11db7U : crc << 1U;
    }
    return crc;
}
void DvbSubtitlePassthrough::readPat(const Packet& packet) {
    const auto start = payloadOffset(packet);
    if (!(packet[1] & 0x40U) || start >= kPacketSize) return;
    const std::size_t at = start + 1U + packet[start];
    if (at + 12U > kPacketSize || packet[at] != 0x00U) return;
    const std::size_t length = 3U +
        (((packet[at + 1U] & 0x0fU) << 8U) | packet[at + 2U]);
    if (length < 16U || at + length > kPacketSize ||
        crc32(packet.data() + at, length) != 0U) return;
    for (std::size_t pos = at + 8U; pos + 4U <= at + length - 4U; pos += 4U) {
        if (u16(packet.data() + pos) == 0U) continue;
        const auto pid = esPid(packet.data() + pos + 2U);
        if (pid != sourcePmtPid_) {
            sourcePmtPid_ = pid;
            sourcePmt_ = {};
            sourceTracks_.clear();
            selectedPids_.fill(false);
            queued_.clear();
        }
        break; // Single selected service after remap.
    }
}
void DvbSubtitlePassthrough::acceptPmtSection(
    const std::vector<std::uint8_t>& section) {
    if (section.size() < 16U || section[0] != 0x02U ||
        crc32(section.data(), section.size()) != 0U) return;
    const std::size_t end = section.size() - 4U;
    std::size_t at = 12U + (((section[10] & 0x0fU) << 8U) | section[11]);
    if (at > end) return;
    std::vector<Track> tracks;
    for (; at + 5U <= end;) {
        const auto pid = esPid(section.data() + at + 1U);
        const auto type = section[at];
        const std::size_t length =
            ((section[at + 3U] & 0x0fU) << 8U) | section[at + 4U];
        if (at + 5U + length > end) return;
        if (type == 0x06U && pid >= 0x20U && pid < 0x1fffU &&
            tracks.size() < kMaxSubtitleTracks) {
            std::vector<std::uint8_t> desc;
            const std::size_t limit = at + 5U + length;
            for (std::size_t off = at + 5U; off + 2U <= limit;) {
                const auto tag = section[off];
                const std::size_t n = section[off + 1U];
                if (off + 2U + n > limit) break;
                // Preserve subtitle page IDs, language, composition/ancillary
                // page IDs exactly. Other PMT descriptors are not required.
                if (tag == 0x59U && n > 0U && n % 8U == 0U)
                    desc.insert(desc.end(), section.begin() + off,
                                section.begin() + off + n + 2U);
                off += n + 2U;
            }
            if (!desc.empty()) tracks.push_back({pid, std::move(desc)});
        }
        at += 5U + length;
    }
    if (at != end) return;
    bool same = tracks.size() == sourceTracks_.size();
    if (same) for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].pid != sourceTracks_[i].pid ||
            tracks[i].descriptors != sourceTracks_[i].descriptors) {
            same = false;
            break;
        }
    if (!same) {
        sourceTracks_ = std::move(tracks);
        selectedPids_.fill(false);
        queued_.clear();
        awaitStart_.fill(false);
        std::clog << "DVB SUBTITLES source tracks=" << sourceTracks_.size() << '\n';
    }
}
void DvbSubtitlePassthrough::readPmt(const Packet& packet) {
    const std::size_t off = payloadOffset(packet);
    if (off >= kPacketSize) return;
    const auto cc = packet[3] & 15U;
    if (sourcePmt_.continuityValid &&
        cc != ((sourcePmt_.lastContinuity + 1U) & 15U)) {
        sourcePmt_.bytes.clear();
        sourcePmt_.expected = 0;
    }
    sourcePmt_.continuityValid = true;
    sourcePmt_.lastContinuity = static_cast<std::uint8_t>(cc);
    std::size_t start = off;
    if ((packet[1] & 0x40U) != 0U) {
        start += 1U + packet[off];
        sourcePmt_.bytes.clear();
        sourcePmt_.expected = 0;
    }
    if (start >= kPacketSize) return;
    auto& buffer = sourcePmt_.bytes;
    if (buffer.size() >= kMaxPsi) { buffer.clear(); return; }
    const auto wanted = sourcePmt_.expected;
    const auto bytes = wanted && wanted > buffer.size()
        ? std::min(kPacketSize - start, wanted - buffer.size())
        : kPacketSize - start;
    buffer.insert(buffer.end(), packet.begin() + start, packet.begin() + start + bytes);
    if (buffer.size() >= 3U && !sourcePmt_.expected) {
        sourcePmt_.expected = 3U + (((buffer[1] & 0x0fU) << 8U) | buffer[2]);
        if (sourcePmt_.expected > kMaxPsi || sourcePmt_.expected < 16U) {
            buffer.clear();
            sourcePmt_.expected = 0;
            return;
        }
    }
    if (sourcePmt_.expected && buffer.size() >= sourcePmt_.expected) {
        buffer.resize(sourcePmt_.expected);
        acceptPmtSection(buffer);
        buffer.clear();
        sourcePmt_.expected = 0;
    }
}
void DvbSubtitlePassthrough::observe(const std::uint8_t* data, std::size_t size) {
    if (!data) return;
    for (std::size_t offset = 0; offset + kPacketSize <= size; offset += kPacketSize) {
        Packet packet{};
        std::memcpy(packet.data(), data + offset, packet.size());
        if (packet[0] != 0x47U) continue;
        const auto pid = packetPid(packet);
        if (pid == 0U) readPat(packet);
        else if (pid == sourcePmtPid_) readPmt(packet);
        if (pid >= selectedPids_.size() || !selectedPids_[pid]) continue;
        const bool pusi = (packet[1] & 0x40U) != 0U;
        if (awaitStart_[pid] && !pusi) continue;
        if (pusi) awaitStart_[pid] = false;
        if (queued_.size() >= kMaxQueuedPackets) {
            // Recover at the next PES start rather than output a half caption.
            queued_.clear();
            for (const auto& track : sourceTracks_) awaitStart_[track.pid] = true;
            if (!pusi) continue;
            awaitStart_[pid] = false;
        }
        queued_.push_back(std::move(packet));
    }
}
void DvbSubtitlePassthrough::rewriteOutputPmt(Packet& packet) {
    if ((packet[1] & 0x40U) == 0U || sourceTracks_.empty()) return;
    const auto off = payloadOffset(packet);
    if (off >= kPacketSize) return;
    const std::size_t start = off + 1U + packet[off];
    if (start + 16U > kPacketSize || packet[start] != 0x02U) return;
    const std::size_t originalLength = 3U +
        (((packet[start + 1U] & 0x0fU) << 8U) | packet[start + 2U]);
    if (originalLength < 16U || start + originalLength > kPacketSize ||
        crc32(packet.data() + start, originalLength) != 0U) return;
    const std::size_t end = start + originalLength - 4U;
    const auto pcr = esPid(packet.data() + start + 8U);
    std::array<bool, 8192> occupied{};
    occupied[pcr] = true;
    occupied[packetPid(packet)] = true;
    occupied[0U] = true;
    occupied[0x11U] = true;
    occupied[0x12U] = true;
    std::size_t pos = start + 12U +
        (((packet[start + 10U] & 0x0fU) << 8U) | packet[start + 11U]);
    while (pos + 5U <= end) {
        const auto pid = esPid(packet.data() + pos + 1U);
        occupied[pid] = true;
        pos += 5U + (((packet[pos + 3U] & 0x0fU) << 8U) | packet[pos + 4U]);
        if (pos > end) return;
    }
    if (pos != end) return;
    std::vector<std::uint8_t> section(
        packet.begin() + start, packet.begin() + start + originalLength - 4U);
    // The native mux emits single-packet PMTs. Keep that exact behavior;
    // advertise only complete DVB subtitle descriptors that fit in the
    // remaining PMT bytes, never truncate a descriptor or overflow 188 bytes.
    std::array<bool, 8192> selected{};
    std::size_t accepted = 0;
    for (const auto& track : sourceTracks_) {
        if (occupied[track.pid] || selected[track.pid]) continue;
        if (track.descriptors.size() > 255U ||
            start + section.size() + 5U + track.descriptors.size() + 4U > kPacketSize)
            continue;
        section.push_back(0x06U);
        section.push_back(static_cast<std::uint8_t>(0xe0U | (track.pid >> 8U)));
        section.push_back(static_cast<std::uint8_t>(track.pid));
        section.push_back(static_cast<std::uint8_t>(0xf0U | (track.descriptors.size() >> 8U)));
        section.push_back(static_cast<std::uint8_t>(track.descriptors.size()));
        section.insert(section.end(), track.descriptors.begin(), track.descriptors.end());
        selected[track.pid] = true;
        ++accepted;
    }
    if (!accepted) {
        if (!warnedPmtCapacity_) {
            std::clog << "DVB SUBTITLES warning: no free subtitle PID/PMT capacity\n";
            warnedPmtCapacity_ = true;
        }
        return;
    }
    const auto sectionLength = section.size() - 3U + 4U;
    section[1] = static_cast<std::uint8_t>((section[1] & 0xf0U) |
                                           ((sectionLength >> 8U) & 15U));
    section[2] = static_cast<std::uint8_t>(sectionLength);
    // Set a different PMT version to reflect added DVB subtitle elementary
    // streams even if the native transcoder's A/V codec mapping is unchanged.
    section[5] = static_cast<std::uint8_t>(
        (section[5] & 0xc1U) | (((((section[5] >> 1U) & 31U) + 1U) & 31U) << 1U));
    appendCrc(section);
    const auto originalPid = packetPid(packet);
    const auto originalCc = packet[3] & 0x0fU;
    packet.fill(0xffU);
    packet[0] = 0x47U;
    packet[1] = static_cast<std::uint8_t>(0x40U | ((originalPid >> 8U) & 0x1fU));
    packet[2] = static_cast<std::uint8_t>(originalPid);
    packet[3] = static_cast<std::uint8_t>(0x10U | originalCc);
    packet[4] = 0x00U;
    std::memcpy(packet.data() + 5U, section.data(), section.size());
    selectedPids_ = selected;
}
void DvbSubtitlePassthrough::inject(std::vector<Packet>& output) {
    if (sourceTracks_.empty()) return;
    for (auto& packet : output) rewriteOutputPmt(packet);
    if (output.empty() || queued_.empty()) return;
    std::size_t count = 0;
    for (; count < kMaxFlushPackets && !queued_.empty(); ++count) {
        Packet packet = queued_.front();
        queued_.pop_front();
        const auto pid = packetPid(packet);
        if (!selectedPids_[pid]) continue;
        if ((packet[3] & 0x10U) != 0U) {
            packet[3] = static_cast<std::uint8_t>(
                (packet[3] & 0xf0U) | (continuity_[pid]++ & 0x0fU));
        }
        output.push_back(std::move(packet));
    }
}
