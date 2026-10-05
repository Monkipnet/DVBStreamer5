#include "media/NativeHlsSegmenter.h"
#include "media/NativeSampleAes.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

namespace {

constexpr std::uint64_t kPcrWrap = (1ULL << 33);

std::string programDateTime(std::chrono::system_clock::time_point value) {
    const std::time_t t = std::chrono::system_clock::to_time_t(value);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

bool atomicReplace(const std::filesystem::path& temp,
                   const std::filesystem::path& destination,
                   std::string& error) {
    std::error_code ec;
    std::filesystem::rename(temp, destination, ec);
    if (!ec) return true;
    std::filesystem::remove(destination, ec);
    ec.clear();
    std::filesystem::rename(temp, destination, ec);
    if (!ec) return true;
    error = "failed to replace HLS playlist: " + ec.message();
    return false;
}

bool collectPsiSection(
    const std::vector<dvbstreamer5::media::mpegts::Packet>& packets,
    std::uint8_t tableId,
    std::vector<std::uint8_t>& section) {
    section.clear();
    std::size_t sectionSize = 0;
    bool started = false;

    for (const auto& packet : packets) {
        dvbstreamer5::media::mpegts::PacketInfo info;
        if (!dvbstreamer5::media::mpegts::inspectPacket(packet.data(), packet.size(), info) ||
            !info.hasPayload || info.payloadOffset >= dvbstreamer5::media::mpegts::kPacketSize) {
            continue;
        }

        std::size_t offset = info.payloadOffset;
        if (!started) {
            if (!info.payloadUnitStart) continue;
            if (offset >= dvbstreamer5::media::mpegts::kPacketSize) return false;
            const std::size_t pointer = packet[offset];
            offset += 1U + pointer;
            if (offset + 3U > dvbstreamer5::media::mpegts::kPacketSize ||
                packet[offset] != tableId) return false;
            const std::size_t sectionLength =
                (static_cast<std::size_t>(packet[offset + 1U] & 0x0fU) << 8U) |
                static_cast<std::size_t>(packet[offset + 2U]);
            sectionSize = 3U + sectionLength;
            section.reserve(sectionSize);
            started = true;
        } else if (info.payloadUnitStart) {
            break;
        }

        const std::size_t available = dvbstreamer5::media::mpegts::kPacketSize - offset;
        const std::size_t remaining = sectionSize - section.size();
        const std::size_t copy = std::min(available, remaining);
        section.insert(section.end(), packet.begin() + static_cast<std::ptrdiff_t>(offset),
                       packet.begin() + static_cast<std::ptrdiff_t>(offset + copy));
        if (section.size() >= sectionSize) return true;
    }
    section.clear();
    return false;
}

bool parsePmtStreamTypes(
    const std::vector<dvbstreamer5::media::mpegts::Packet>& packets,
    std::array<std::uint8_t, 8192>& streamTypes) {
    std::vector<std::uint8_t> section;
    if (!collectPsiSection(packets, 0x02U, section) || section.size() < 16U) return false;

    const std::size_t programInfoLength =
        (static_cast<std::size_t>(section[10] & 0x0fU) << 8U) |
        static_cast<std::size_t>(section[11]);
    std::size_t pos = 12U + programInfoLength;
    if (pos > section.size() - 4U) return false;
    const std::size_t end = section.size() - 4U;

    std::array<std::uint8_t, 8192> parsed{};
    while (pos + 5U <= end) {
        const std::uint8_t streamType = section[pos];
        const std::uint16_t pid = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(section[pos + 1U] & 0x1fU) << 8U) |
            section[pos + 2U]);
        const std::size_t esInfoLength =
            (static_cast<std::size_t>(section[pos + 3U] & 0x0fU) << 8U) |
            static_cast<std::size_t>(section[pos + 4U]);
        if (pos + 5U + esInfoLength > end) return false;
        if (pid < parsed.size()) parsed[pid] = streamType;
        pos += 5U + esInfoLength;
    }
    streamTypes = parsed;
    return true;
}

bool psiCollectionComplete(const std::vector<dvbstreamer5::media::mpegts::Packet>& packets,
                           std::uint8_t tableId) {
    if (packets.empty()) return false;
    std::size_t sectionSize = 0;
    std::size_t collected = 0;
    bool firstPacket = true;

    for (const auto& packet : packets) {
        dvbstreamer5::media::mpegts::PacketInfo info;
        if (!dvbstreamer5::media::mpegts::inspectPacket(packet.data(), packet.size(), info) ||
            !info.hasPayload || info.payloadOffset >= dvbstreamer5::media::mpegts::kPacketSize) {
            continue;
        }

        std::size_t offset = info.payloadOffset;
        if (firstPacket) {
            if (!info.payloadUnitStart) return false;
            const std::size_t payloadSize = dvbstreamer5::media::mpegts::kPacketSize - offset;
            if (payloadSize < 1U) return false;
            const std::size_t pointer = packet[offset];
            offset += 1U + pointer;
            if (offset + 3U > dvbstreamer5::media::mpegts::kPacketSize || packet[offset] != tableId) {
                return false;
            }
            const std::size_t sectionLength =
                (static_cast<std::size_t>(packet[offset + 1U] & 0x0fU) << 8U) |
                static_cast<std::size_t>(packet[offset + 2U]);
            sectionSize = 3U + sectionLength;
            firstPacket = false;
        }

        if (offset < dvbstreamer5::media::mpegts::kPacketSize) {
            collected += dvbstreamer5::media::mpegts::kPacketSize - offset;
        }
        if (sectionSize > 0 && collected >= sectionSize) return true;
    }
    return false;
}

std::array<std::uint8_t,16> sequenceIv(std::uint64_t seq) {
    std::array<std::uint8_t,16> iv{};
    for(int i=15;i>=8;--i){iv[static_cast<std::size_t>(i)]=static_cast<std::uint8_t>(seq&0xffU);seq>>=8;}
    return iv;
}

bool encryptAes128File(const std::filesystem::path& path,
                       const std::array<std::uint8_t,16>& key,
                       const std::array<std::uint8_t,16>& iv,
                       std::string& error) {
    std::ifstream in(path,std::ios::binary);
    if(!in){error="failed to reopen HLS segment for AES-128";return false;}
    std::vector<std::uint8_t> plain((std::istreambuf_iterator<char>(in)),{});
    EVP_CIPHER_CTX* ctx=EVP_CIPHER_CTX_new(); if(!ctx){error="AES context allocation failed";return false;}
    std::vector<std::uint8_t> enc(plain.size()+16); int n=0,t=0;
    const bool ok=EVP_EncryptInit_ex(ctx,EVP_aes_128_cbc(),nullptr,key.data(),iv.data())==1 &&
        EVP_EncryptUpdate(ctx,enc.data(),&n,plain.data(),static_cast<int>(plain.size()))==1 &&
        EVP_EncryptFinal_ex(ctx,enc.data()+n,&t)==1;
    EVP_CIPHER_CTX_free(ctx); if(!ok){error="HLS AES-128 encrypt failed";return false;} enc.resize(static_cast<std::size_t>(n+t));
    std::ofstream out(path,std::ios::binary|std::ios::trunc); out.write(reinterpret_cast<const char*>(enc.data()),static_cast<std::streamsize>(enc.size()));
    if(!out){error="failed to write encrypted HLS segment";return false;} return true;
}

bool sampleAesFile(const std::filesystem::path& path,
                   const std::array<std::uint8_t,16>& key,
                   const std::array<std::uint8_t,16>& iv,
                   std::string& error) {
    std::ifstream in(path,std::ios::binary); if(!in){error="failed to reopen HLS segment for SAMPLE-AES";return false;}
    std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),{}), enc;
    if(!dvbstreamer5::media::hls::transformSampleAesMpegTs(raw.data(),raw.size(),key,iv,true,enc,error)) return false;
    std::ofstream out(path,std::ios::binary|std::ios::trunc); out.write(reinterpret_cast<const char*>(enc.data()),static_cast<std::streamsize>(enc.size()));
    if(!out){error="failed to write SAMPLE-AES HLS segment";return false;} return true;
}

enum class H26xAccessUnitKind {
    Undetermined,
    RandomAccess,
    NonRandomAccess
};

H26xAccessUnitKind classifyH26xAccessUnit(
    const std::vector<std::uint8_t>& elementary,
    std::uint8_t streamType) {
    for (std::size_t pos = 0; pos + 4U <= elementary.size(); ++pos) {
        std::size_t nal = elementary.size();
        if (elementary[pos] == 0x00U && elementary[pos + 1U] == 0x00U &&
            elementary[pos + 2U] == 0x01U) {
            nal = pos + 3U;
        } else if (pos + 5U <= elementary.size() &&
                   elementary[pos] == 0x00U && elementary[pos + 1U] == 0x00U &&
                   elementary[pos + 2U] == 0x00U && elementary[pos + 3U] == 0x01U) {
            nal = pos + 4U;
        }
        if (nal >= elementary.size()) continue;

        if (streamType == 0x1bU) {
            const std::uint8_t nalType = elementary[nal] & 0x1fU;
            // AVC VCL NALs are types 1..5. Type 5 is IDR.
            if (nalType >= 1U && nalType <= 5U) {
                return nalType == 5U
                    ? H26xAccessUnitKind::RandomAccess
                    : H26xAccessUnitKind::NonRandomAccess;
            }
        } else if (streamType == 0x24U) {
            const std::uint8_t nalType = (elementary[nal] >> 1U) & 0x3fU;
            // HEVC VCL NALs are types 0..31; IRAP is 16..23.
            if (nalType <= 31U) {
                return (nalType >= 16U && nalType <= 23U)
                    ? H26xAccessUnitKind::RandomAccess
                    : H26xAccessUnitKind::NonRandomAccess;
            }
        }
    }
    return H26xAccessUnitKind::Undetermined;
}

std::vector<std::uint8_t> extractH26xParameterSets(
    const std::vector<std::uint8_t>& elementary,
    std::uint8_t streamType,
    bool terminalNalComplete) {
    std::vector<std::uint8_t> out;
    bool haveVps = false;
    bool haveSps = false;
    bool havePps = false;

    auto startCode = [&](std::size_t pos, std::size_t& prefixSize) -> bool {
        prefixSize = 0;
        if (pos + 3U <= elementary.size() &&
            elementary[pos] == 0x00U && elementary[pos + 1U] == 0x00U &&
            elementary[pos + 2U] == 0x01U) {
            prefixSize = 3U;
            return true;
        }
        if (pos + 4U <= elementary.size() &&
            elementary[pos] == 0x00U && elementary[pos + 1U] == 0x00U &&
            elementary[pos + 2U] == 0x00U && elementary[pos + 3U] == 0x01U) {
            prefixSize = 4U;
            return true;
        }
        return false;
    };

    std::size_t pos = 0;
    while (pos + 4U <= elementary.size()) {
        std::size_t prefix = 0;
        if (!startCode(pos, prefix)) {
            ++pos;
            continue;
        }
        const std::size_t nal = pos + prefix;
        if (nal >= elementary.size()) break;

        std::size_t next = nal + 1U;
        bool foundNext = false;
        for (; next + 3U <= elementary.size(); ++next) {
            std::size_t nextPrefix = 0;
            if (startCode(next, nextPrefix)) {
                foundNext = true;
                break;
            }
        }
        if (!foundNext) {
            // The caller may be probing a still-growing TS/PES buffer. The
            // terminal NAL is not cacheable until the PES boundary is known.
            if (!terminalNalComplete) break;
            next = elementary.size();
        }

        bool wanted = false;
        if (streamType == 0x1bU) {
            const std::uint8_t nalType = elementary[nal] & 0x1fU;
            if (nalType == 7U) { wanted = true; haveSps = true; }
            else if (nalType == 8U) { wanted = true; havePps = true; }
        } else if (streamType == 0x24U) {
            const std::uint8_t nalType = (elementary[nal] >> 1U) & 0x3fU;
            if (nalType == 32U) { wanted = true; haveVps = true; }
            else if (nalType == 33U) { wanted = true; haveSps = true; }
            else if (nalType == 34U) { wanted = true; havePps = true; }
        }
        if (wanted && next > pos) {
            out.insert(out.end(),
                       elementary.begin() + static_cast<std::ptrdiff_t>(pos),
                       elementary.begin() + static_cast<std::ptrdiff_t>(next));
        }
        pos = next;
    }

    const bool complete = streamType == 0x1bU
        ? (haveSps && havePps)
        : (haveVps && haveSps && havePps);
    if (!complete) out.clear();
    return out;
}

} // namespace

namespace dvbstreamer5::media::hls {

NativeHlsSegmenter::~NativeHlsSegmenter() { stop(); }

bool NativeHlsSegmenter::start(const NativeHlsSegmenterConfig& config, std::string& error) {
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    if (config_.encryption != "aes-128" && config_.encryption != "sample-aes") config_.encryption = "none";
    if (config_.encryption != "none" && !config_.hasKey) {
        if (RAND_bytes(config_.key.data(), static_cast<int>(config_.key.size())) != 1) { error = "cannot generate HLS encryption key"; return false; }
        config_.hasKey = true;
    }
    // V10.8.76: keep more media buffered in the live manifest and make each
    // MPEG-TS segment larger.  The StreamManager still supplies the historical
    // 2 s / 6 segment values, so enforce the new stability floor here for the
    // primary rendition and all ABR variants without touching HTTP/CAM paths.
    config_.targetDurationSeconds = std::clamp(
        std::max(config_.targetDurationSeconds, 4.0), 1.0, 10.0);
    config_.liveWindowSegments = std::clamp<std::size_t>(
        std::max<std::size_t>(config_.liveWindowSegments, 8U), 3U, 30U);
    std::error_code ec;
    std::filesystem::create_directories(config_.directory, ec);
    if (ec) { error = "cannot create HLS directory: " + ec.message(); return false; }
    if (config_.encryption != "none") {
        const std::string keyName = std::filesystem::path(config_.keyUri.empty()?"key.bin":config_.keyUri).filename().string();
        std::ofstream keyOut(config_.directory / keyName, std::ios::binary | std::ios::trunc);
        keyOut.write(reinterpret_cast<const char*>(config_.key.data()), static_cast<std::streamsize>(config_.key.size()));
        if (!keyOut) { error = "cannot write HLS encryption key"; return false; }
    }

    for (const auto& entry : std::filesystem::directory_iterator(config_.directory, ec)) {
        if (ec) break;
        const auto name = entry.path().filename().string();
        if (!entry.is_regular_file()) continue;
        const bool playlistFile = name == "video.m3u8" || name == "video.m3u8.tmp";
        const bool segmentFile = name.rfind("segment", 0) == 0 && entry.path().extension() == ".ts";
        // V10.8.78: a previous live playlist must never survive a new OnDemand
        // generation, even when archive retention keeps the old media files.
        // Otherwise the first request can observe stale sequence numbers.
        if (playlistFile || (segmentFile && !config_.archiveEnabled)) {
            std::filesystem::remove(entry.path(), ec);
            ec.clear();
        }
    }

    framer_.reset();
    liveSegments_.clear();
    retiredSegments_.clear();
    patCollecting_.clear();
    patPrefix_.clear();
    pmtCollecting_.clear();
    pmtPrefix_.clear();
    pmtPid_ = mpegts::kNullPid;
    elementaryStreamType_.fill(0);
    nextSequence_ = 0;
    completedSegments_ = 0;
    haveFirstPcr_ = false;
    segmentHasPackets_ = false;
    waitingForIndependentStart_ = config_.independentSegments;
    waitingForCleanStart_ = !config_.independentSegments;
    firstSegmentPidStarted_.fill(false);
    h26xCutArmed_ = false;
    h26xBoundaryPending_ = false;
    h26xBoundaryPid_ = mpegts::kNullPid;
    h26xBoundaryStreamType_ = 0;
    h26xBoundaryDuration_ = 0.0;
    h26xBoundaryPcr_ = 0;
    h26xBoundaryPackets_.clear();
    h26xBoundaryElementary_.clear();
    h26xConfigPid_ = mpegts::kNullPid;
    h26xConfigStreamType_ = 0;
    h26xParameterSets_.clear();
    h26xConfigProbeActive_ = false;
    h26xConfigProbePid_ = mpegts::kNullPid;
    h26xConfigProbeStreamType_ = 0;
    h26xConfigProbeElementary_.clear();
    lastError_.clear();
    programTime_ = std::chrono::system_clock::now();
    running_ = true;
    // V10.8.78: do not publish an empty 200-OK manifest here.  HttpServer's
    // OnDemand startup wait checks for video.m3u8; create it only after rotate()
    // has finalized the first real media segment so ffmpeg/VLC cannot cache an
    // empty playlist and abort before DVB tune/CAM startup completes.
    error.clear();
    return true;
}

bool NativeHlsSegmenter::push(const std::uint8_t* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !data || size == 0) return false;
    std::vector<mpegts::Packet> packets;
    framer_.push(data, size, packets);
    for (const auto& packet : packets) if (!appendPacket(packet)) return false;
    return true;
}

void NativeHlsSegmenter::observePsi(const mpegts::Packet& packet,
                                      const mpegts::PacketInfo& info) {
    constexpr std::size_t kMaxPsiPackets = 32;
    if (!info.hasPayload || info.payloadOffset >= mpegts::kPacketSize) return;

    if (info.pid == 0x0000U) {
        if (info.payloadUnitStart) {
            if (!patCollecting_.empty()) patPrefix_ = patCollecting_;
            patCollecting_.clear();

            const std::uint8_t* payload = packet.data() + info.payloadOffset;
            const std::size_t payloadSize = mpegts::kPacketSize - info.payloadOffset;
            if (payloadSize >= 1U) {
                const std::size_t sectionStart = 1U + static_cast<std::size_t>(payload[0]);
                if (sectionStart + 12U <= payloadSize && payload[sectionStart] == 0x00U) {
                    const std::size_t sectionLength =
                        (static_cast<std::size_t>(payload[sectionStart + 1U] & 0x0fU) << 8U) |
                        static_cast<std::size_t>(payload[sectionStart + 2U]);
                    const std::size_t sectionSize = 3U + sectionLength;
                    if (sectionSize >= 12U && sectionStart + sectionSize <= payloadSize) {
                        const std::size_t entriesEnd = sectionStart + sectionSize - 4U;
                        std::uint16_t discovered = mpegts::kNullPid;
                        for (std::size_t off = sectionStart + 8U; off + 4U <= entriesEnd; off += 4U) {
                            const std::uint16_t service = static_cast<std::uint16_t>(
                                (static_cast<std::uint16_t>(payload[off]) << 8U) |
                                payload[off + 1U]);
                            const std::uint16_t pid = static_cast<std::uint16_t>(
                                (static_cast<std::uint16_t>(payload[off + 2U] & 0x1fU) << 8U) |
                                payload[off + 3U]);
                            if (service != 0U && pid < mpegts::kNullPid) {
                                discovered = pid;
                                break;
                            }
                        }
                        if (discovered != mpegts::kNullPid && discovered != pmtPid_) {
                            pmtPid_ = discovered;
                            pmtCollecting_.clear();
                            pmtPrefix_.clear();
                            elementaryStreamType_.fill(0);
                        }
                    }
                }
            }
        }
        if ((info.payloadUnitStart || !patCollecting_.empty()) &&
            patCollecting_.size() < kMaxPsiPackets) {
            patCollecting_.push_back(packet);
        }
        return;
    }

    if (pmtPid_ != mpegts::kNullPid && info.pid == pmtPid_) {
        if (info.payloadUnitStart) {
            if (!pmtCollecting_.empty()) pmtPrefix_ = pmtCollecting_;
            pmtCollecting_.clear();
        }
        if ((info.payloadUnitStart || !pmtCollecting_.empty()) &&
            pmtCollecting_.size() < kMaxPsiPackets) {
            pmtCollecting_.push_back(packet);
            (void)parsePmtStreamTypes(pmtCollecting_, elementaryStreamType_);
        }
    }
}

bool NativeHlsSegmenter::writePsiPrefix() {
    auto writePackets = [this](const std::vector<mpegts::Packet>& packets) {
        for (const auto& packet : packets) {
            segment_.write(reinterpret_cast<const char*>(packet.data()),
                           static_cast<std::streamsize>(packet.size()));
            if (!segment_) return false;
        }
        return true;
    };

    // V10.8.80: do not wait for the next PSI repetition just to promote the
    // current collection into *Prefix_.  If the current section is already
    // complete, it is just as valid and avoids pushing cold-start beyond the
    // HTTP startup window.
    const auto& pat = !patPrefix_.empty() ? patPrefix_ : patCollecting_;
    const auto& pmt = !pmtPrefix_.empty() ? pmtPrefix_ : pmtCollecting_;
    if (!pat.empty() && !writePackets(pat)) return false;
    if (!pmt.empty() && !writePackets(pmt)) return false;
    return true;
}

bool NativeHlsSegmenter::appendPacket(const mpegts::Packet& packet) {
    mpegts::PacketInfo info;
    if (!mpegts::inspectPacket(packet.data(), packet.size(), info)) return true;
    observePsi(packet, info);

    if (waitingForIndependentStart_) {
        // For ABR, do not publish a first segment that begins between IDRs.
        // The native mux marks the first TS packet of every random-access video
        // PES with both PCR and random_access_indicator.
        if (!(info.hasPcr && info.randomAccess)) return true;
        waitingForIndependentStart_ = false;
        firstPcr_ = info.pcrBase90k;
        lastPcr_ = info.pcrBase90k;
        haveFirstPcr_ = true;
        programTime_ = std::chrono::system_clock::now();
    }

    // V10.8.80: keep V10.8.79's clean PES start, but recognize a complete PSI
    // section as soon as its bytes have arrived. V10.8.79 waited for the next
    // PAT/PMT PUSI to copy the collection into *Prefix_, which could exceed the
    // HTTP cold-start wait and return 404 even though usable PSI was present.
    if (waitingForCleanStart_) {
        const bool havePat = !patPrefix_.empty() || psiCollectionComplete(patCollecting_, 0x00U);
        const bool havePmt = !pmtPrefix_.empty() || psiCollectionComplete(pmtCollecting_, 0x02U);
        if (!havePat || !havePmt) return true;
        if (!(info.hasPcr && info.payloadUnitStart)) return true;
        waitingForCleanStart_ = false;
        firstPcr_ = info.pcrBase90k;
        lastPcr_ = info.pcrBase90k;
        haveFirstPcr_ = true;
        programTime_ = std::chrono::system_clock::now();
    }

    // V10.8.82: keep each elementary PID muted until it reaches a decoder-safe
    // PES start. V10.8.81 already required MPEG audio to begin on a real frame.
    // MPEG-2 video now additionally waits for a sequence_header_code (0x000001B3)
    // in the first TS packet of its PES so a fresh decoder knows width/height
    // before it sees picture data.
    if (!waitingForCleanStart_) {
        const bool psi = info.pid == 0x0000U || info.pid == pmtPid_;
        const bool nullPacket = info.pid == mpegts::kNullPid;
        if (!psi && !nullPacket && info.hasPayload &&
            info.pid < firstSegmentPidStarted_.size() &&
            !firstSegmentPidStarted_[info.pid]) {
            if (!info.payloadUnitStart || info.scrambled) return true;

            const std::size_t off = info.payloadOffset;
            if (off + 6U <= packet.size() &&
                packet[off] == 0x00U && packet[off + 1U] == 0x00U && packet[off + 2U] == 0x01U) {
                const std::uint8_t streamId = packet[off + 3U];
                if (streamId >= 0xc0U && streamId <= 0xdfU) {
                    if (off + 9U > packet.size()) return true;
                    const std::size_t elementary =
                        off + 9U + static_cast<std::size_t>(packet[off + 8U]);
                    if (elementary + 3U > packet.size()) return true;

                    const std::uint8_t b0 = packet[elementary];
                    const std::uint8_t b1 = packet[elementary + 1U];
                    const std::uint8_t b2 = packet[elementary + 2U];
                    const bool sync = b0 == 0xffU && (b1 & 0xe0U) == 0xe0U;
                    const bool versionOk = (b1 & 0x18U) != 0x08U;
                    const bool layerOk = (b1 & 0x06U) != 0x00U;
                    const bool bitrateOk = (b2 & 0xf0U) != 0x00U && (b2 & 0xf0U) != 0xf0U;
                    const bool sampleRateOk = (b2 & 0x0cU) != 0x0cU;
                    if (!(sync && versionOk && layerOk && bitrateOk && sampleRateOk)) return true;
                } else if (streamId >= 0xe0U && streamId <= 0xefU) {
                    // V10.8.85: PES stream_id alone does not identify the video
                    // codec. AVC and HEVC also use 0xE0..0xEF, so requiring the
                    // MPEG-2 sequence_header_code for every video PID silently
                    // dropped all H.264/H.265 pictures. Consult PMT stream_type.
                    const std::uint8_t streamType = info.pid < elementaryStreamType_.size()
                        ? elementaryStreamType_[info.pid]
                        : 0U;
                    const bool mpeg12Video = streamType == 0x01U || streamType == 0x02U;
                    if (mpeg12Video) {
                        if (off + 9U > packet.size()) return true;
                        const std::size_t elementary =
                            off + 9U + static_cast<std::size_t>(packet[off + 8U]);
                        if (elementary + 4U > packet.size()) return true;

                        bool sequenceHeader = false;
                        for (std::size_t pos = elementary; pos + 4U <= packet.size(); ++pos) {
                            if (packet[pos] == 0x00U && packet[pos + 1U] == 0x00U &&
                                packet[pos + 2U] == 0x01U && packet[pos + 3U] == 0xb3U) {
                                sequenceHeader = true;
                                break;
                            }
                        }
                        if (!sequenceHeader) return true;
                    } else if (streamType == 0x1bU || streamType == 0x24U) {
                        // V10.8.87: AVC/HEVC must not start the first HLS segment
                        // on slices that reference parameter sets the fresh decoder
                        // has never seen. Admit the first PES only when its first
                        // TS packet carries the codec parameter sets in Annex-B form.
                        if (off + 9U > packet.size()) return true;
                        const std::size_t elementary =
                            off + 9U + static_cast<std::size_t>(packet[off + 8U]);
                        if (elementary + 4U > packet.size()) return true;

                        bool haveVps = false;
                        bool haveSps = false;
                        bool havePps = false;
                        bool haveRandomAccess = info.randomAccess;
                        for (std::size_t pos = elementary; pos + 4U <= packet.size(); ++pos) {
                            std::size_t nal = packet.size();
                            if (packet[pos] == 0x00U && packet[pos + 1U] == 0x00U &&
                                packet[pos + 2U] == 0x01U) {
                                nal = pos + 3U;
                            } else if (pos + 5U <= packet.size() &&
                                       packet[pos] == 0x00U && packet[pos + 1U] == 0x00U &&
                                       packet[pos + 2U] == 0x00U && packet[pos + 3U] == 0x01U) {
                                nal = pos + 4U;
                            }
                            if (nal >= packet.size()) continue;

                            if (streamType == 0x1bU) {
                                const std::uint8_t nalType = packet[nal] & 0x1fU;
                                if (nalType == 7U) haveSps = true;
                                else if (nalType == 8U) havePps = true;
                                else if (nalType == 5U) haveRandomAccess = true;
                            } else {
                                const std::uint8_t nalType = (packet[nal] >> 1U) & 0x3fU;
                                if (nalType == 32U) haveVps = true;
                                else if (nalType == 33U) haveSps = true;
                                else if (nalType == 34U) havePps = true;
                                else if (nalType >= 16U && nalType <= 23U) haveRandomAccess = true;
                            }
                        }

                        // V10.8.88: parameter sets alone are not enough for a fresh
                        // decoder.  The first admitted AVC/HEVC PES must also be a
                        // random-access access unit (IDR for AVC, IRAP for HEVC).
                        // Prefer an explicit Annex-B NAL, while also accepting the
                        // TS random_access_indicator when the key NAL starts in a
                        // later TS packet of the same PES.
                        const bool decoderConfigReady = streamType == 0x1bU
                            ? (haveSps && havePps && haveRandomAccess)
                            : (haveVps && haveSps && havePps && haveRandomAccess);
                        if (!decoderConfigReady) return true;

                    }
                }
            }
            firstSegmentPidStarted_[info.pid] = true;
        }
    }

    // V10.8.96: cache decoder configuration only from a complete H.26x PES.
    // SPS/PPS/VPS NAL units frequently cross a 188-byte TS packet boundary;
    // V10.8.95 could therefore replay a truncated PPS even though its NAL type
    // byte had already been visible in the first packet.
    const std::uint8_t observedStreamType = info.pid < elementaryStreamType_.size()
        ? elementaryStreamType_[info.pid]
        : 0U;
    const bool observedH26x = observedStreamType == 0x1bU || observedStreamType == 0x24U;
    if (!config_.independentSegments && observedH26x && info.hasPayload && !info.scrambled) {
        constexpr std::size_t kMaxConfigProbeBytes = 128U * 1024U;

        auto commitConfigProbe = [this]() {
            if (!h26xConfigProbeActive_ || h26xConfigProbeElementary_.empty()) return;
            auto parameterSets = extractH26xParameterSets(
                h26xConfigProbeElementary_, h26xConfigProbeStreamType_, true);
            if (!parameterSets.empty()) {
                h26xConfigPid_ = h26xConfigProbePid_;
                h26xConfigStreamType_ = h26xConfigProbeStreamType_;
                h26xParameterSets_ = std::move(parameterSets);
            }
        };

        if (info.payloadUnitStart) {
            if (h26xConfigProbeActive_ && info.pid == h26xConfigProbePid_) {
                commitConfigProbe();
            }
            h26xConfigProbeActive_ = false;
            h26xConfigProbePid_ = mpegts::kNullPid;
            h26xConfigProbeStreamType_ = 0;
            h26xConfigProbeElementary_.clear();

            const std::size_t off = info.payloadOffset;
            if (off + 9U <= packet.size() &&
                packet[off] == 0x00U && packet[off + 1U] == 0x00U &&
                packet[off + 2U] == 0x01U &&
                packet[off + 3U] >= 0xe0U && packet[off + 3U] <= 0xefU) {
                const std::size_t begin =
                    off + 9U + static_cast<std::size_t>(packet[off + 8U]);
                if (begin <= packet.size()) {
                    h26xConfigProbeActive_ = true;
                    h26xConfigProbePid_ = info.pid;
                    h26xConfigProbeStreamType_ = observedStreamType;
                    const std::size_t count = std::min(
                        kMaxConfigProbeBytes, packet.size() - begin);
                    h26xConfigProbeElementary_.insert(
                        h26xConfigProbeElementary_.end(),
                        packet.begin() + static_cast<std::ptrdiff_t>(begin),
                        packet.begin() + static_cast<std::ptrdiff_t>(begin + count));
                }
            }
        } else if (h26xConfigProbeActive_ && info.pid == h26xConfigProbePid_ &&
                   info.payloadOffset < packet.size() &&
                   h26xConfigProbeElementary_.size() < kMaxConfigProbeBytes) {
            const std::size_t remaining =
                kMaxConfigProbeBytes - h26xConfigProbeElementary_.size();
            const std::size_t count = std::min(
                remaining, packet.size() - info.payloadOffset);
            h26xConfigProbeElementary_.insert(
                h26xConfigProbeElementary_.end(),
                packet.begin() + static_cast<std::ptrdiff_t>(info.payloadOffset),
                packet.begin() + static_cast<std::ptrdiff_t>(info.payloadOffset + count));
        }
    }

    // Track PCR continuously, including while a candidate H.26x PES is held
    // in the boundary buffer. If that candidate becomes the next segment, its
    // first PCR becomes the new segment clock origin.
    if (info.hasPcr) {
        lastPcr_ = info.pcrBase90k;
        if (!haveFirstPcr_) {
            firstPcr_ = lastPcr_;
            haveFirstPcr_ = true;
        }
    }

    const bool hasH26xVideo = std::any_of(
        elementaryStreamType_.begin(), elementaryStreamType_.end(),
        [](std::uint8_t type) { return type == 0x1bU || type == 0x24U; });
    const double segmentTarget = completedSegments_ == 0
        ? std::min(config_.targetDurationSeconds, 1.0)
        : config_.targetDurationSeconds;
    const double elapsed = haveFirstPcr_
        ? pcrDeltaSeconds(firstPcr_, lastPcr_)
        : 0.0;
    const bool targetReached = segmentHasPackets_ && haveFirstPcr_ &&
        elapsed >= segmentTarget;

    auto writeBufferedPacket = [this](const mpegts::Packet& buffered) -> bool {
        if (!segment_.is_open() && !openSegment()) return false;
        segment_.write(reinterpret_cast<const char*>(buffered.data()),
                       static_cast<std::streamsize>(buffered.size()));
        if (!segment_) {
            fail("failed to write buffered H.26x HLS packet");
            return false;
        }
        segmentHasPackets_ = true;
        return true;
    };

    auto writeH26xConfigPrefix = [&]() -> bool {
        if (h26xParameterSets_.empty() ||
            h26xConfigPid_ != h26xBoundaryPid_ ||
            h26xConfigStreamType_ != h26xBoundaryStreamType_ ||
            h26xBoundaryPackets_.empty()) {
            return true;
        }

        mpegts::PacketInfo firstInfo;
        if (!mpegts::inspectPacket(h26xBoundaryPackets_.front().data(),
                                   h26xBoundaryPackets_.front().size(), firstInfo) ||
            firstInfo.pid != h26xBoundaryPid_) {
            return true;
        }

        // Build a short video PES containing only decoder configuration NALs.
        // It carries no PTS/DTS; the following real IDR/IRAP PES keeps the
        // source timestamps. This avoids duplicating an old picture.
        std::vector<std::uint8_t> pes;
        pes.reserve(9U + h26xParameterSets_.size());
        pes.push_back(0x00U);
        pes.push_back(0x00U);
        pes.push_back(0x01U);
        pes.push_back(0xe0U);
        const std::size_t afterLength = 3U + h26xParameterSets_.size();
        const std::uint16_t pesLength = afterLength <= 0xffffU
            ? static_cast<std::uint16_t>(afterLength)
            : 0U;
        pes.push_back(static_cast<std::uint8_t>(pesLength >> 8U));
        pes.push_back(static_cast<std::uint8_t>(pesLength));
        pes.push_back(0x80U);
        pes.push_back(0x00U);
        pes.push_back(0x00U);
        pes.insert(pes.end(), h26xParameterSets_.begin(), h26xParameterSets_.end());

        constexpr std::size_t kPayloadMax = mpegts::kPacketSize - 4U;
        // V10.8.97: the synthetic decoder-config PES consumes continuity
        // counters that do not exist in the source stream. A player that keeps
        // MPEG-TS PID state across HLS segments can otherwise reject these
        // packets as duplicate/out-of-order when the counter appears to move
        // backwards at the segment boundary. Force an adaptation field on the
        // first synthetic packet and set discontinuity_indicator so the demuxer
        // explicitly resets continuity before accepting SPS/PPS/VPS.
        constexpr std::size_t kFirstPayloadMax = mpegts::kPacketSize - 6U;
        const std::size_t firstPayload = std::min(kFirstPayloadMax, pes.size());
        const std::size_t remainingPayload = pes.size() - firstPayload;
        const std::size_t packetCount = 1U +
            (remainingPayload + kPayloadMax - 1U) / kPayloadMax;
        std::uint8_t continuity = static_cast<std::uint8_t>(
            (static_cast<unsigned>(firstInfo.continuityCounter) + 16U -
             static_cast<unsigned>(packetCount & 0x0fU)) & 0x0fU);

        std::size_t offset = 0;
        for (std::size_t index = 0; index < packetCount; ++index) {
            const bool discontinuity = index == 0U;
            const std::size_t capacity = discontinuity ? kFirstPayloadMax : kPayloadMax;
            const std::size_t payload = std::min(capacity, pes.size() - offset);
            mpegts::Packet prefix{};
            prefix.fill(0xffU);
            prefix[0] = mpegts::kSyncByte;
            prefix[1] = static_cast<std::uint8_t>(
                (index == 0U ? 0x40U : 0x00U) |
                ((h26xBoundaryPid_ >> 8U) & 0x1fU));
            prefix[2] = static_cast<std::uint8_t>(h26xBoundaryPid_);

            std::size_t payloadOffset = 4U;
            if (!discontinuity && payload == kPayloadMax) {
                prefix[3] = static_cast<std::uint8_t>(0x10U | continuity);
            } else {
                prefix[3] = static_cast<std::uint8_t>(0x30U | continuity);
                const std::size_t adaptationLength = 183U - payload;
                prefix[4] = static_cast<std::uint8_t>(adaptationLength);
                if (adaptationLength > 0U) {
                    prefix[5] = discontinuity ? 0x80U : 0x00U;
                }
                payloadOffset = 5U + adaptationLength;
            }
            continuity = static_cast<std::uint8_t>((continuity + 1U) & 0x0fU);
            std::copy_n(pes.begin() + static_cast<std::ptrdiff_t>(offset),
                        payload,
                        prefix.begin() + static_cast<std::ptrdiff_t>(payloadOffset));
            offset += payload;
            if (!writeBufferedPacket(prefix)) return false;
        }
        return true;
    };

    auto clearBoundary = [this]() {
        h26xBoundaryPending_ = false;
        h26xBoundaryPid_ = mpegts::kNullPid;
        h26xBoundaryStreamType_ = 0;
        h26xBoundaryDuration_ = 0.0;
        h26xBoundaryPcr_ = 0;
        h26xBoundaryPackets_.clear();
        h26xBoundaryElementary_.clear();
    };

    auto flushBoundary = [&](bool randomAccess) -> bool {
        bool candidateCarriesConfig = false;
        if (randomAccess) {
            auto currentConfig = extractH26xParameterSets(
                h26xBoundaryElementary_, h26xBoundaryStreamType_, false);
            if (!currentConfig.empty()) {
                h26xConfigPid_ = h26xBoundaryPid_;
                h26xConfigStreamType_ = h26xBoundaryStreamType_;
                h26xParameterSets_ = std::move(currentConfig);
                candidateCarriesConfig = true;
            }
            if (!rotate(std::max(0.001, h26xBoundaryDuration_))) return false;
            firstPcr_ = h26xBoundaryPcr_;
            haveFirstPcr_ = true;
            if (!candidateCarriesConfig && !writeH26xConfigPrefix()) return false;
        }
        for (const auto& buffered : h26xBoundaryPackets_) {
            if (!writeBufferedPacket(buffered)) return false;
        }
        clearBoundary();
        if (randomAccess) h26xCutArmed_ = false;
        return true;
    };

    auto appendBoundaryElementary = [this](const mpegts::Packet& candidate,
                                            const mpegts::PacketInfo& candidateInfo,
                                            bool pesStart) {
        if (candidateInfo.pid != h26xBoundaryPid_ || !candidateInfo.hasPayload ||
            candidateInfo.payloadOffset >= mpegts::kPacketSize) {
            return;
        }
        std::size_t begin = candidateInfo.payloadOffset;
        if (pesStart) {
            if (begin + 9U > candidate.size() ||
                candidate[begin] != 0x00U || candidate[begin + 1U] != 0x00U ||
                candidate[begin + 2U] != 0x01U ||
                candidate[begin + 3U] < 0xe0U || candidate[begin + 3U] > 0xefU) {
                return;
            }
            begin += 9U + static_cast<std::size_t>(candidate[begin + 8U]);
            if (begin > candidate.size()) return;
        }
        constexpr std::size_t kMaxProbeBytes = 512U * 1024U;
        if (begin < candidate.size() && h26xBoundaryElementary_.size() < kMaxProbeBytes) {
            const std::size_t remaining = kMaxProbeBytes - h26xBoundaryElementary_.size();
            const std::size_t count = std::min(remaining, candidate.size() - begin);
            h26xBoundaryElementary_.insert(
                h26xBoundaryElementary_.end(),
                candidate.begin() + static_cast<std::ptrdiff_t>(begin),
                candidate.begin() + static_cast<std::ptrdiff_t>(begin + count));
        }
    };

    if (!config_.independentSegments && hasH26xVideo) {
        if (targetReached) h26xCutArmed_ = true;

        // A pending candidate owns every TS packet from its video PES start
        // until the first VCL NAL is identified. Buffering interleaved audio/PSI
        // preserves packet order if this PES becomes the start of the next segment.
        if (h26xBoundaryPending_) {
            const bool nextVideoPes = info.pid == h26xBoundaryPid_ &&
                info.payloadUnitStart && !info.scrambled;
            if (nextVideoPes) {
                // No VCL decision was found in the previous PES. Keep it in the
                // current segment and immediately evaluate this new PES instead.
                if (!flushBoundary(false)) return false;
                h26xCutArmed_ = true;
            } else {
                h26xBoundaryPackets_.push_back(packet);
                appendBoundaryElementary(packet, info, false);
                const auto kind = classifyH26xAccessUnit(
                    h26xBoundaryElementary_, h26xBoundaryStreamType_);
                if (kind == H26xAccessUnitKind::RandomAccess) {
                    return flushBoundary(true);
                }
                if (kind == H26xAccessUnitKind::NonRandomAccess ||
                    h26xBoundaryPackets_.size() >= 2048U ||
                    h26xBoundaryElementary_.size() >= 512U * 1024U) {
                    return flushBoundary(false);
                }
                return true;
            }
        }

        const std::uint8_t streamType = info.pid < elementaryStreamType_.size()
            ? elementaryStreamType_[info.pid]
            : 0U;
        const bool h26xVideoPesStart = h26xCutArmed_ && info.payloadUnitStart &&
            info.hasPayload && !info.scrambled &&
            (streamType == 0x1bU || streamType == 0x24U);
        if (h26xVideoPesStart) {
            const std::size_t off = info.payloadOffset;
            const bool videoPes = off + 9U <= packet.size() &&
                packet[off] == 0x00U && packet[off + 1U] == 0x00U &&
                packet[off + 2U] == 0x01U &&
                packet[off + 3U] >= 0xe0U && packet[off + 3U] <= 0xefU;
            if (videoPes) {
                h26xBoundaryPending_ = true;
                h26xBoundaryPid_ = info.pid;
                h26xBoundaryStreamType_ = streamType;
                h26xBoundaryDuration_ = std::max(0.001, elapsed);
                h26xBoundaryPcr_ = info.hasPcr ? info.pcrBase90k : lastPcr_;
                h26xBoundaryPackets_.clear();
                h26xBoundaryElementary_.clear();
                h26xBoundaryPackets_.push_back(packet);
                appendBoundaryElementary(packet, info, true);

                if (info.randomAccess) {
                    return flushBoundary(true);
                }
                const auto kind = classifyH26xAccessUnit(
                    h26xBoundaryElementary_, h26xBoundaryStreamType_);
                if (kind == H26xAccessUnitKind::RandomAccess) {
                    return flushBoundary(true);
                }
                if (kind == H26xAccessUnitKind::NonRandomAccess) {
                    return flushBoundary(false);
                }
                return true;
            }
        }
    } else if (info.hasPcr) {
        // ABR/transcoded HLS already carries random_access_indicator. Legacy
        // non-H.26x passthrough keeps its established PES-boundary fallback.
        const bool hardLimit = !config_.independentSegments &&
            segmentHasPackets_ && elapsed >= segmentTarget * 3.0;
        const bool rotateBoundary = config_.independentSegments
            ? info.randomAccess
            : (info.payloadUnitStart || hardLimit);
        if (targetReached && rotateBoundary) {
            if (!rotate(std::max(0.001, elapsed))) return false;
            firstPcr_ = lastPcr_;
            haveFirstPcr_ = true;
        }
    }
    if (!segment_.is_open() && !openSegment()) return false;
    segment_.write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(packet.size()));
    if (!segment_) { fail("failed to write HLS MPEG-TS segment"); return false; }
    segmentHasPackets_ = true;
    return true;
}

bool NativeHlsSegmenter::openSegment() {
    std::ostringstream name;
    name << "segment" << std::setw(10) << std::setfill('0') << nextSequence_ << ".ts";
    segmentPath_ = config_.directory / name.str();
    segment_.open(segmentPath_, std::ios::binary | std::ios::trunc);
    if (!segment_.is_open()) { fail("failed to create HLS segment " + segmentPath_.string()); return false; }
    // The clean-start gate discards pre-boundary PSI/media. Prefix every
    // segment with the newest usable PAT/PMT so a fresh demuxer is self-contained.
    if ((!patPrefix_.empty() || !patCollecting_.empty() ||
         !pmtPrefix_.empty() || !pmtCollecting_.empty()) && !writePsiPrefix()) {
        fail("failed to write HLS PAT/PMT prefix");
        return false;
    }
    return true;
}

bool NativeHlsSegmenter::rotate(double durationSeconds) {
    if (!segmentHasPackets_ || !segment_.is_open()) return true;
    segment_.flush();
    segment_.close();
    if (!segment_) { fail("failed to finalize HLS segment"); return false; }
    if (config_.encryption != "none") {
        const auto iv = sequenceIv(nextSequence_);
        std::string cryptError;
        const bool ok = config_.encryption == "sample-aes"
            ? sampleAesFile(segmentPath_, config_.key, iv, cryptError)
            : encryptAes128File(segmentPath_, config_.key, iv, cryptError);
        if (!ok) { fail(cryptError); return false; }
    }
    SegmentInfo info;
    info.sequence = nextSequence_++;
    info.fileName = segmentPath_.filename().string();
    info.duration = std::clamp(durationSeconds, 0.05, config_.targetDurationSeconds * 3.0);
    info.wallTime = programTime_;
    programTime_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::duration<double>(info.duration));
    liveSegments_.push_back(info);
    ++completedSegments_;
    segmentHasPackets_ = false;
    segmentPath_.clear();
    prune();

    // V10.8.99: do not publish the initial live manifest for passthrough
    // H.264/H.265 after only one completed segment. FFmpeg's live HLS stream
    // probe can lock codec parameters from that first startup segment before
    // the next decoder-safe segment exists, even though the same two files are
    // decoded correctly when they are already listed together in a static HLS
    // playlist. Wait for two finalized H.26x segments before making video.m3u8
    // visible; later rotations keep the normal rolling playlist cadence.
    const bool passthroughH26x = !config_.independentSegments &&
        std::any_of(elementaryStreamType_.begin(), elementaryStreamType_.end(),
                    [](std::uint8_t type) { return type == 0x1bU || type == 0x24U; });
    if (passthroughH26x && completedSegments_ < 2U) return true;

    if (!writePlaylist(false)) return false;
    return true;
}

bool NativeHlsSegmenter::writePlaylist(bool endList) {
    const auto temp = config_.directory / "video.m3u8.tmp";
    const auto destination = config_.directory / "video.m3u8";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) { fail("failed to create HLS playlist"); return false; }
    double maxDuration = config_.targetDurationSeconds;
    for (const auto& item : liveSegments_) maxDuration = std::max(maxDuration, item.duration);
    const std::uint64_t mediaSequence = liveSegments_.empty() ? nextSequence_ : liveSegments_.front().sequence;
    out << "#EXTM3U\n#EXT-X-VERSION:3\n";
    if (config_.independentSegments) out << "#EXT-X-INDEPENDENT-SEGMENTS\n";
    out << "#EXT-X-TARGETDURATION:" << static_cast<unsigned>(std::ceil(maxDuration)) << "\n";
    out << "#EXT-X-MEDIA-SEQUENCE:" << mediaSequence << "\n";
    if (config_.encryption != "none") {
        out << "#EXT-X-KEY:METHOD=" << (config_.encryption == "sample-aes" ? "SAMPLE-AES" : "AES-128")
            << ",URI=\"" << (config_.keyUri.empty()?"key.bin":config_.keyUri) << "\"\n";
    }
    for (const auto& item : liveSegments_) {
        out << "#EXT-X-PROGRAM-DATE-TIME:" << programDateTime(item.wallTime) << "\n";
        out << "#EXTINF:" << std::fixed << std::setprecision(3) << item.duration << ",\n";
        out << item.fileName << "\n";
    }
    if (endList) out << "#EXT-X-ENDLIST\n";
    out.flush();
    if (!out) { fail("failed to write HLS playlist"); return false; }
    out.close();
    std::string error;
    if (!atomicReplace(temp, destination, error)) { fail(error); return false; }
    return true;
}

void NativeHlsSegmenter::prune() {
    while (liveSegments_.size() > config_.liveWindowSegments) {
        const SegmentInfo old = liveSegments_.front();
        liveSegments_.pop_front();
        if (!config_.archiveEnabled) {
            retiredSegments_.push_back(old);
        }
    }
    if (!config_.archiveEnabled) {
        // Do not remove a segment at the exact instant it disappears from the
        // newest playlist. A player may still be working from the immediately
        // previous manifest and request it a moment later. Retaining one extra
        // playlist window keeps storage bounded while eliminating that 404 race.
        const std::size_t graceSegments = std::max<std::size_t>(3, config_.liveWindowSegments);
        while (retiredSegments_.size() > graceSegments) {
            const SegmentInfo old = retiredSegments_.front();
            retiredSegments_.pop_front();
            std::error_code ec;
            std::filesystem::remove(config_.directory / old.fileName, ec);
        }
        return;
    }
    const auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(config_.archiveHours);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(config_.directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != ".ts") continue;
        const auto name = entry.path().filename().string();
        if (name.rfind("segment", 0) != 0) continue;
        const auto ft = entry.last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        const auto st = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        if (st < cutoff) std::filesystem::remove(entry.path(), ec);
        ec.clear();
    }
}

void NativeHlsSegmenter::stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    // Do not discard a partially probed H.26x PES on shutdown. It belongs to
    // the current segment unless a keyframe decision already rotated it.
    if (h26xBoundaryPending_ && !h26xBoundaryPackets_.empty()) {
        if (!segment_.is_open()) (void)openSegment();
        if (segment_.is_open()) {
            for (const auto& buffered : h26xBoundaryPackets_) {
                segment_.write(reinterpret_cast<const char*>(buffered.data()),
                               static_cast<std::streamsize>(buffered.size()));
                if (!segment_) break;
                segmentHasPackets_ = true;
            }
        }
        h26xBoundaryPending_ = false;
        h26xBoundaryPackets_.clear();
        h26xBoundaryElementary_.clear();
    }
    if (segmentHasPackets_ && segment_.is_open()) {
        double duration = config_.targetDurationSeconds;
        if (haveFirstPcr_) duration = std::max(0.05, pcrDeltaSeconds(firstPcr_, lastPcr_));
        rotate(duration);
    }
    if (segment_.is_open()) segment_.close();
    writePlaylist(true);
    running_ = false;
}

bool NativeHlsSegmenter::isRunning() const noexcept { std::lock_guard<std::mutex> lock(mutex_); return running_; }
std::string NativeHlsSegmenter::lastError() const { std::lock_guard<std::mutex> lock(mutex_); return lastError_; }
std::uint64_t NativeHlsSegmenter::segmentCount() const noexcept { std::lock_guard<std::mutex> lock(mutex_); return completedSegments_; }
void NativeHlsSegmenter::fail(const std::string& error) { lastError_ = error; running_ = false; }

double NativeHlsSegmenter::pcrDeltaSeconds(std::uint64_t first, std::uint64_t last) {
    const std::uint64_t delta = last >= first ? last - first : (kPcrWrap - first) + last;
    return static_cast<double>(delta) / 90000.0;
}

} // namespace dvbstreamer5::media::hls
