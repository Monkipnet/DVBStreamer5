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

DvbSubtitlePassthrough::DvbSubtitlePassthrough(
    std::string language, std::string selectedTrack)
    : language_(std::move(language)), selectedTrack_(std::move(selectedTrack)) {
    if (language_ != "auto" &&
        (language_.size() != 3U ||
         !std::all_of(language_.begin(), language_.end(),
                      [](unsigned char c) { return c >= 'a' && c <= 'z'; }))) {
        language_ = "auto";
    }
}

std::vector<DvbSubtitlePassthrough::AvailableTrack>
DvbSubtitlePassthrough::availableTracks() const {
    std::lock_guard<std::mutex> lock(availableMutex_);
    return availableTracks_;
}
bool DvbSubtitlePassthrough::hasParsedPmt() const {
    std::lock_guard<std::mutex> lock(availableMutex_);
    return availablePmtParsed_;
}

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
            sourcePmtParsed_ = false;
            passthroughExcludedPids_.fill(false);
            passthroughFilterActive_ = false;
            {
                std::lock_guard<std::mutex> lock(availableMutex_);
                availableTracks_.clear();
                availablePmtParsed_ = false;
            }
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
    std::vector<AvailableTrack> available;
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
                if (tag == 0x59U && n > 0U && n % 8U == 0U) {
                    // The actual PMT is the source of dropdown choices. A
                    // subtitling_descriptor entry is language/type/page IDs.
                    std::vector<std::uint8_t> chosen;
                    for (std::size_t entry = off + 2U;
                         entry + 8U <= off + 2U + n; entry += 8U) {
                        const std::string code(
                            reinterpret_cast<const char*>(section.data() + entry), 3U);
                        const std::uint16_t page = u16(section.data() + entry + 4U);
                        const std::string key = std::to_string(pid) + ":" +
                            code + ":" + std::to_string(page);
                        if (available.size() < 64U &&
                            std::none_of(available.begin(), available.end(),
                                [&](const AvailableTrack& item) { return item.id == key; })) {
                            available.push_back({key, pid, code,
                                section[entry + 3U], page,
                                u16(section.data() + entry + 6U)});
                        }
                        const bool selected = selectedTrack_ != "auto"
                            ? key == selectedTrack_
                            : (language_ == "auto" || language_ == code);
                        if (selected) chosen.insert(chosen.end(),
                            section.begin() + entry, section.begin() + entry + 8U);
                    }
                    if (!chosen.empty()) {
                        desc.push_back(0x59U);
                        desc.push_back(static_cast<std::uint8_t>(chosen.size()));
                        desc.insert(desc.end(), chosen.begin(), chosen.end());
                    }
                }
                off += n + 2U;
            }
            if (!desc.empty()) tracks.push_back({pid, std::move(desc)});
        }
        at += 5U + length;
    }
    if (at != end) return;
    {
        std::lock_guard<std::mutex> lock(availableMutex_);
        availableTracks_ = std::move(available);
        availablePmtParsed_ = true;
    }
    bool same = tracks.size() == sourceTracks_.size();
    if (same) for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].pid != sourceTracks_[i].pid ||
            tracks[i].descriptors != sourceTracks_[i].descriptors) {
            same = false;
            break;
        }
    if (!same || !sourcePmtParsed_) {
        sourcePmtParsed_ = true;
        sourceTracks_ = std::move(tracks);
        selectedPids_.fill(false);
        queued_.clear();
        awaitStart_.fill(false);
        for (const auto& track : sourceTracks_) awaitStart_[track.pid] = true;
        std::clog << "DVB SUBTITLES source tracks=" << sourceTracks_.size()
                  << " selected_language=" << language_
                  << " selected_track=" << selectedTrack_
                  << (sourceTracks_.empty() && language_ != "auto"
                      ? " (no matching DVB subtitle track)" : "")
                  << '\n';
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
        if ((packet[3] & 0x10U) == 0U) continue; // no subtitle payload
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
    auto nextMapping = outputPids_;
    std::array<bool, 8192> taken = occupied;
    // Reserve all original source subtitle PIDs; don't inadvertently remap
    // a colliding track onto another track's original transport PID.
    for (const auto& track : sourceTracks_) taken[track.pid] = true;
    std::size_t accepted = 0;
    for (const auto& track : sourceTracks_) {
        if (selected[track.pid]) continue;
        if (track.descriptors.size() > 255U ||
            start + section.size() + 5U + track.descriptors.size() + 4U > kPacketSize)
            continue;
        auto outputPid = track.pid;
        if (occupied[outputPid]) {
            // A native transcoder typically uses 0x100/0x101. A broadcaster
            // may have placed its subtitles on either PID. Remap subtitles
            // into a reserved high ES range rather than clashing with A/V.
            outputPid = 0;
            for (std::uint16_t candidate = 0x1a00U; candidate < 0x1b00U; ++candidate) {
                if (!taken[candidate]) { outputPid = candidate; break; }
            }
            if (!outputPid) continue;
        }
        taken[outputPid] = true;
        section.push_back(0x06U);
        section.push_back(static_cast<std::uint8_t>(0xe0U | (outputPid >> 8U)));
        section.push_back(static_cast<std::uint8_t>(outputPid));
        section.push_back(static_cast<std::uint8_t>(0xf0U | (track.descriptors.size() >> 8U)));
        section.push_back(static_cast<std::uint8_t>(track.descriptors.size()));
        section.insert(section.end(), track.descriptors.begin(), track.descriptors.end());
        selected[track.pid] = true;
        if (!selectedPids_[track.pid] || outputPids_[track.pid] != outputPid) {
            continuity_[outputPid] = 0;
            awaitStart_[track.pid] = true;
        }
        nextMapping[track.pid] = outputPid;
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
    outputPids_ = nextMapping;
}
void DvbSubtitlePassthrough::filterPassthrough(std::vector<Packet>& output) {
    // In an untranscoded TS the remapper already carries subtitle PES and
    // the corresponding 0x59 descriptors. Never inject a duplicate PES.
    // A specific selection instead filters the original PMT and unwanted PIDs.
    if (selectedTrack_ == "auto" && language_ == "auto") return;
    for (auto& packet : output) {
        if (packet[0] != 0x47U || packetPid(packet) != sourcePmtPid_ ||
            (packet[1] & 0x40U) == 0U) continue;
        const auto off = payloadOffset(packet);
        // Only edit a complete single-packet PMT. Multisection tables are
        // preserved rather than producing invalid PSI or dropping PES.
        if (off != 4U || packet[off] != 0U) continue;
        const std::size_t start = off + 1U;
        if (start + 16U > kPacketSize || packet[start] != 0x02U) continue;
        const std::size_t originalSize = 3U +
            (((packet[start + 1U] & 0x0fU) << 8U) | packet[start + 2U]);
        if (originalSize < 16U || start + originalSize > kPacketSize ||
            crc32(packet.data() + start, originalSize) != 0U) continue;
        const std::size_t end = start + originalSize - 4U;
        const std::size_t programInfo = 12U +
            (((packet[start + 10U] & 0x0fU) << 8U) | packet[start + 11U]);
        if (start + programInfo > end) continue;
        std::vector<std::uint8_t> section(
            packet.begin() + start, packet.begin() + start + programInfo);
        std::array<bool, 8192> subtitlePids{};
        std::array<bool, 8192> retainedPids{};
        bool malformed = false;
        bool subtitleFound = false;
        for (std::size_t pos = start + programInfo; pos < end;) {
            if (pos + 5U > end) { malformed = true; break; }
            const auto pid = esPid(packet.data() + pos + 1U);
            const auto length = static_cast<std::size_t>(
                ((packet[pos + 3U] & 0x0fU) << 8U) | packet[pos + 4U]);
            const std::size_t esEnd = pos + 5U + length;
            if (esEnd > end) { malformed = true; break; }
            std::vector<std::uint8_t> desc;
            bool subtitleEs = false;
            bool selected = false;
            for (std::size_t d = pos + 5U; d < esEnd;) {
                if (d + 2U > esEnd) { malformed = true; break; }
                const std::uint8_t tag = packet[d];
                const std::size_t size = packet[d + 1U];
                if (d + 2U + size > esEnd) { malformed = true; break; }
                if (packet[pos] == 0x06U && tag == 0x59U &&
                    size != 0U && size % 8U == 0U) {
                    subtitleEs = true;
                    std::vector<std::uint8_t> matches;
                    for (std::size_t at = d + 2U; at + 8U <= d + 2U + size;
                         at += 8U) {
                        const std::string code(
                            reinterpret_cast<const char*>(packet.data() + at), 3U);
                        const auto page = u16(packet.data() + at + 4U);
                        const auto key = std::to_string(pid) + ":" + code +
                            ":" + std::to_string(page);
                        const bool use = selectedTrack_ != "auto"
                            ? selectedTrack_ == key
                            : language_ == code;
                        if (use) {
                            matches.insert(matches.end(),
                                packet.begin() + at, packet.begin() + at + 8U);
                            selected = true;
                        }
                    }
                    if (!matches.empty()) {
                        desc.push_back(0x59U);
                        desc.push_back(static_cast<std::uint8_t>(matches.size()));
                        desc.insert(desc.end(), matches.begin(), matches.end());
                    }
                } else {
                    desc.insert(desc.end(), packet.begin() + d,
                                packet.begin() + d + 2U + size);
                }
                d += 2U + size;
            }
            if (malformed) break;
            if (subtitleEs) {
                subtitleFound = true;
                subtitlePids[pid] = true;
                if (selected) retainedPids[pid] = true;
            }
            if (!subtitleEs || selected) {
                section.insert(section.end(), packet.begin() + pos,
                               packet.begin() + pos + 5U);
                const std::size_t header = section.size() - 5U;
                section[header + 3U] = static_cast<std::uint8_t>(
                    0xf0U | ((desc.size() >> 8U) & 0x0fU));
                section[header + 4U] = static_cast<std::uint8_t>(desc.size());
                section.insert(section.end(), desc.begin(), desc.end());
            }
            pos = esEnd;
        }
        if (malformed || !subtitleFound ||
            start + section.size() + 4U > kPacketSize) {
            // A new or unsupported PMT must never inherit exclusion flags
            // from a previous PMT revision or selected service.
            passthroughExcludedPids_.fill(false);
            passthroughFilterActive_ = false;
            continue;
        }
        const auto newLength = section.size() - 3U + 4U;
        section[1] = static_cast<std::uint8_t>((section[1] & 0xf0U) |
                                                ((newLength >> 8U) & 0x0fU));
        section[2] = static_cast<std::uint8_t>(newLength);
        section[5] = static_cast<std::uint8_t>(
            (section[5] & 0xc1U) |
            (((((section[5] >> 1U) & 31U) + 1U) & 31U) << 1U));
        appendCrc(section);
        std::fill(packet.begin() + start, packet.end(), 0xffU);
        std::memcpy(packet.data() + start, section.data(), section.size());
        for (std::size_t pid = 0; pid < passthroughExcludedPids_.size(); ++pid)
            passthroughExcludedPids_[pid] = subtitlePids[pid] && !retainedPids[pid];
        passthroughFilterActive_ = true;
    }
    if (!passthroughFilterActive_) return;
    output.erase(std::remove_if(output.begin(), output.end(),
        [this](const Packet& packet) {
            return packet[0] == 0x47U &&
                   passthroughExcludedPids_[packetPid(packet)];
        }), output.end());
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
        if (!selectedPids_[pid] || awaitStart_[pid]) continue;
        const auto dest = outputPids_[pid];
        if (dest == 0U || dest >= 0x1fffU) continue;
        if (dest != pid) {
            packet[1] = static_cast<std::uint8_t>(
                (packet[1] & 0xe0U) | ((dest >> 8U) & 0x1fU));
            packet[2] = static_cast<std::uint8_t>(dest);
        }
        packet[3] = static_cast<std::uint8_t>(
            (packet[3] & 0xf0U) | (continuity_[dest]++ & 0x0fU));
        output.push_back(std::move(packet));
    }
}
