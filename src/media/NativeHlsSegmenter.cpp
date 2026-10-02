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
    config_.targetDurationSeconds = std::clamp(config_.targetDurationSeconds, 1.0, 10.0);
    config_.liveWindowSegments = std::clamp<std::size_t>(config_.liveWindowSegments, 3, 30);
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
        if (entry.is_regular_file() && (name == "video.m3u8" || name == "video.m3u8.tmp" ||
            (name.rfind("segment", 0) == 0 && entry.path().extension() == ".ts"))) {
            if (!config_.archiveEnabled) std::filesystem::remove(entry.path(), ec);
            ec.clear();
        }
    }

    framer_.reset();
    liveSegments_.clear();
    nextSequence_ = 0;
    completedSegments_ = 0;
    haveFirstPcr_ = false;
    segmentHasPackets_ = false;
    waitingForIndependentStart_ = config_.independentSegments;
    lastError_.clear();
    programTime_ = std::chrono::system_clock::now();
    running_ = true;
    if (!writePlaylist(false)) { error = lastError_; running_ = false; return false; }
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

bool NativeHlsSegmenter::appendPacket(const mpegts::Packet& packet) {
    mpegts::PacketInfo info;
    if (!mpegts::inspectPacket(packet.data(), packet.size(), info)) return true;

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

    if (info.hasPcr) {
        lastPcr_ = info.pcrBase90k;
        if (!haveFirstPcr_) { firstPcr_ = lastPcr_; haveFirstPcr_ = true; }
        const double elapsed = pcrDeltaSeconds(firstPcr_, lastPcr_);
        const bool targetReached = segmentHasPackets_ && elapsed >= config_.targetDurationSeconds;
        const bool hardLimit = !config_.independentSegments &&
            segmentHasPackets_ && elapsed >= config_.targetDurationSeconds * 3.0;
        if (targetReached && (info.randomAccess || hardLimit)) {
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
            std::error_code ec;
            std::filesystem::remove(config_.directory / old.fileName, ec);
        }
    }
    if (!config_.archiveEnabled) return;
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
