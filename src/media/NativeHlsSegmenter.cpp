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
    nextSequence_ = 0;
    completedSegments_ = 0;
    haveFirstPcr_ = false;
    segmentHasPackets_ = false;
    waitingForIndependentStart_ = config_.independentSegments;
    waitingForCleanStart_ = !config_.independentSegments;
    firstSegmentPidStarted_.fill(false);
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

    // V10.8.81: keep each elementary PID muted until it reaches a clear PES
    // start. MPEG audio PES (stream_id 0xC0..0xDF) is accepted only when the
    // elementary payload in that PES begins with a valid MPEG-audio frame sync.
    // This removes the one truncated MP2 access unit seen at HLS cold start.
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
                    // DVB MPEG-1/2 audio normally uses the MPEG-2 PES optional
                    // header layout. If the first frame header is not completely
                    // inside this TS packet, reject this PES and try the next one.
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
                }
            }
            firstSegmentPidStarted_[info.pid] = true;
        }
    }

    if (info.hasPcr) {
        lastPcr_ = info.pcrBase90k;
        if (!haveFirstPcr_) { firstPcr_ = lastPcr_; haveFirstPcr_ = true; }
        const double elapsed = pcrDeltaSeconds(firstPcr_, lastPcr_);
        // V10.8.80: publish only the first cold-start segment at ~1 second.
        // Steady-state live HLS remains at the V10.8.76 4-second target.
        const double segmentTarget = completedSegments_ == 0
            ? std::min(config_.targetDurationSeconds, 1.0)
            : config_.targetDurationSeconds;
        const bool targetReached = segmentHasPackets_ && elapsed >= segmentTarget;
        // DVB passthrough encoders do not always set random_access_indicator.
        // Prefer a PCR-bearing PES boundary instead of waiting six seconds
        // and then cutting at an arbitrary PCR in the middle of transport.
        const bool passthroughPesBoundary = !config_.independentSegments && info.payloadUnitStart;
        const bool hardLimit = !config_.independentSegments &&
            segmentHasPackets_ && elapsed >= segmentTarget * 3.0;
        if (targetReached && (info.randomAccess || passthroughPesBoundary || hardLimit)) {
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
