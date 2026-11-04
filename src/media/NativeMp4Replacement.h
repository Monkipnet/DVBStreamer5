#pragma once
// Native ISO-BMFF MP4/MOV replacement input, no FFmpeg/libavformat.
// H264/H265 in avcC/hvcC -> Annex B, AAC-LC -> ADTS; native MPEG-TS mux.
#include "media/NativeMpegTsMux.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace dvbstreamer5::media::mp4 {
class NativeMp4Replacement {
public:
    bool open(const std::string& path, bool loop, std::uint16_t serviceId,
              const std::string& serviceName, const std::string& serviceProvider,
              std::string& error) {
        file_.open(path, std::ios::binary);
        if (!file_) { error = "cannot open replacement MP4: " + path; return false; }
        file_.seekg(0, std::ios::end);
        const auto end = file_.tellg();
        if (end <= 0) { error = "replacement MP4 is empty"; return false; }
        fileSize_ = static_cast<std::uint64_t>(end);
        std::vector<std::uint8_t> moov;
        for (std::uint64_t offset = 0; offset + 8 <= fileSize_;) {
            std::uint8_t header[16]{};
            if (!readAt(offset, header, 8)) break;
            std::uint64_t size = be32(header);
            const auto type = be32(header + 4);
            unsigned headerSize = 8;
            if (size == 1) {
                if (!readAt(offset + 8, header + 8, 8)) break;
                size = be64(header + 8); headerSize = 16;
            } else if (size == 0) size = fileSize_ - offset;
            if (size < headerSize || size > fileSize_ - offset) {
                error = "invalid MP4 atom size"; return false;
            }
            if (type == tag("moov")) {
                if (size > (64ULL << 20)) { error = "MP4 moov exceeds 64 MiB"; return false; }
                moov.resize(static_cast<std::size_t>(size));
                if (!readAt(offset, moov.data(), moov.size())) {
                    error = "cannot read MP4 moov metadata"; return false;
                }
                break;
            }
            offset += size;
        }
        if (moov.empty()) { error = "MP4 moov metadata not found (fragmented MP4 unsupported)"; return false; }
        Box root{};
        if (!boxAt(moov, 0, moov.size(), root)) { error = "invalid MP4 moov atom"; return false; }
        const auto children = boxes(moov, root);
        bool videoSeen = false, audioSeen = false;
        for (const auto& trak : children) {
            if (trak.type != tag("trak")) continue;
            const auto mdia = child(moov, trak, tag("mdia"));
            if (!mdia.valid()) continue;
            const auto hdlr = child(moov, mdia, tag("hdlr"));
            if (!hdlr.valid() || hdlr.end - hdlr.body < 12) continue;
            const auto handler = be32(moov.data() + hdlr.body + 8);
            if (handler != tag("vide") && handler != tag("soun")) continue;
            const bool isVideo = handler == tag("vide");
            if (isVideo && videoSeen) continue;
            if (!isVideo && audioSeen) continue;
            Track track;
            track.video = isVideo;
            if (!parseTrack(moov, mdia, track, error)) return false;
            if (track.samples.empty()) continue;
            if (isVideo) { video_ = std::move(track); videoSeen = true; }
            else { audio_ = std::move(track); audioSeen = true; }
        }
        if (!videoSeen && !audioSeen) {
            error = "MP4 contains no supported H.264/H.265/AAC-LC track"; return false;
        }
        mpegts::NativeMuxConfig cfg;
        cfg.serviceId = serviceId ? serviceId : 1;
        cfg.serviceName = serviceName.empty() ? "Backup MP4" : serviceName;
        cfg.serviceProvider = serviceProvider.empty() ? "DVBStreamer5" : serviceProvider;
        if (!mux_.initialize(cfg, error)) return false;
        if (videoSeen && !mux_.setCodec(mpegts::ElementaryKind::Video, video_.codec, error)) return false;
        if (audioSeen && !mux_.setCodec(mpegts::ElementaryKind::Audio, audio_.codec, error)) return false;
        loop_ = loop;
        duration90k_ = std::max(video_.duration90k, audio_.duration90k);
        if (duration90k_ == 0) duration90k_ = 90000;
        epoch90k_ = 90000; // first PCR remains positive with 500-ms mux preroll
        return true;
    }

    bool next(std::vector<mpegts::Packet>& output, std::uint64_t& mediaClock90k,
              std::string& error) {
        if (video_.index >= video_.samples.size() &&
            audio_.index >= audio_.samples.size()) {
            if (!loop_) { error = "replacement MP4 reached end of file"; return false; }
            video_.index = audio_.index = 0;
            epoch90k_ += duration90k_ + 3600;
            ++loops_;
        }
        Track* track = nullptr;
        if (video_.index >= video_.samples.size()) track = &audio_;
        else if (audio_.index >= audio_.samples.size()) track = &video_;
        else track = video_.samples[video_.index].dts90k <=
                         audio_.samples[audio_.index].dts90k ? &video_ : &audio_;
        if (!track || track->index >= track->samples.size()) {
            error = "MP4 has no playable samples"; return false;
        }
        const auto& sample = track->samples[track->index++];
        if (!sample.size || sample.size > (16U << 20) ||
            sample.offset > fileSize_ || sample.size > fileSize_ - sample.offset) {
            error = "invalid or oversized MP4 sample"; return false;
        }
        std::vector<std::uint8_t> bytes(sample.size);
        if (!readAt(sample.offset, bytes.data(), bytes.size())) {
            error = "could not read MP4 media sample"; return false;
        }
        std::vector<std::uint8_t> accessUnit;
        if (track->video) {
            if (!annexB(*track, sample, bytes, accessUnit)) {
                error = "invalid length-prefixed MP4 video sample"; return false;
            }
        } else if (!adts(*track, bytes, accessUnit)) {
            error = "invalid AAC-LC access unit"; return false;
        }
        mediaClock90k = epoch90k_ + sample.dts90k;
        const std::int64_t signedPts = static_cast<std::int64_t>(mediaClock90k) + sample.cts90k;
        mpegts::ElementarySample elementary;
        elementary.data = accessUnit.data();
        elementary.size = accessUnit.size();
        elementary.dts90k = mediaClock90k;
        elementary.pts90k = signedPts >= 0 ? static_cast<std::uint64_t>(signedPts) : 0;
        elementary.duration90k = sample.duration90k;
        elementary.hasDts = elementary.hasPts = true;
        elementary.randomAccess = sample.key;
        return mux_.write(track->video ? mpegts::ElementaryKind::Video :
            mpegts::ElementaryKind::Audio, elementary, output, error);
    }
    std::uint64_t loops() const noexcept { return loops_; }
    std::size_t videoSamples() const noexcept { return video_.samples.size(); }
    std::size_t audioSamples() const noexcept { return audio_.samples.size(); }

private:
    struct Box {
        std::uint32_t type = 0;
        std::size_t body = 0;
        std::size_t end = 0;
        bool valid() const noexcept { return end > body; }
    };
    struct Sample {
        std::uint64_t offset = 0;
        std::uint64_t dts90k = 0;
        std::int64_t cts90k = 0;
        std::uint64_t duration90k = 0;
        std::uint32_t size = 0;
        bool key = true;
    };
    struct Track {
        bool video = false;
        std::uint32_t timescale = 0;
        mpegts::ElementaryCodec codec = mpegts::ElementaryCodec::Unknown;
        unsigned nalBytes = 0;
        unsigned aacObject = 0, aacFreqIdx = 0, aacChannels = 0;
        std::vector<std::uint8_t> parameterSets;
        std::vector<Sample> samples;
        std::uint64_t duration90k = 0;
        std::size_t index = 0;
    };
    static constexpr std::uint32_t tag(const char* s) noexcept {
        return (static_cast<std::uint32_t>(s[0]) << 24) |
            (static_cast<std::uint32_t>(s[1]) << 16) |
            (static_cast<std::uint32_t>(s[2]) << 8) |
            static_cast<std::uint8_t>(s[3]);
    }
    static std::uint16_t be16(const std::uint8_t* p) noexcept {
        return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
    }
    static std::uint32_t be32(const std::uint8_t* p) noexcept {
        return (static_cast<std::uint32_t>(p[0]) << 24) |
            (static_cast<std::uint32_t>(p[1]) << 16) |
            (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    }
    static std::uint64_t be64(const std::uint8_t* p) noexcept {
        return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
    }
    static bool boxAt(const std::vector<std::uint8_t>& data,
                      std::size_t pos, std::size_t limit, Box& box) {
        if (limit > data.size() || pos > limit || limit - pos < 8) return false;
        std::uint64_t size = be32(data.data() + pos);
        const auto type = be32(data.data() + pos + 4);
        std::size_t header = 8;
        if (size == 1) {
            if (limit - pos < 16) return false;
            size = be64(data.data() + pos + 8); header = 16;
        } else if (size == 0) size = limit - pos;
        if (size < header || size > limit - pos) return false;
        box = {type, pos + header, pos + static_cast<std::size_t>(size)};
        return true;
    }
    static std::vector<Box> boxes(const std::vector<std::uint8_t>& b,
                                  const Box& parent, std::size_t skip = 0) {
        std::vector<Box> result;
        for (std::size_t pos = parent.body + skip; pos < parent.end;) {
            Box cur;
            if (!boxAt(b, pos, parent.end, cur)) break;
            result.push_back(cur);
            pos = cur.end;
        }
        return result;
    }
    static Box child(const std::vector<std::uint8_t>& b, const Box& parent,
                     std::uint32_t type, std::size_t skip = 0) {
        for (const auto& box : boxes(b, parent, skip)) if (box.type == type) return box;
        return {};
    }
    bool readAt(std::uint64_t pos, void* data, std::size_t size) {
        if (pos > fileSize_ || size > fileSize_ - pos ||
            pos > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
            return false;
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
        file_.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
        return file_.good() || (file_.eof() &&
            file_.gcount() == static_cast<std::streamsize>(size));
    }
    static std::uint64_t to90k(std::uint64_t raw, std::uint32_t scale) {
        return raw / scale * 90000ULL + (raw % scale) * 90000ULL / scale;
    }
    static bool descriptorAudioConfig(const std::uint8_t* p, std::size_t n,
                                      Track& track) {
        for (std::size_t i = 0; i + 4 < n; ++i) {
            if (p[i] != 0x05) continue;
            std::size_t at = i + 1;
            unsigned len = 0;
            bool ended = false;
            for (int j = 0; j < 4 && at < n; ++j) {
                const auto b = p[at++];
                len = (len << 7U) | (b & 0x7fU);
                if (!(b & 0x80U)) { ended = true; break; }
            }
            if (!ended || len < 2 || len > n - at) continue;
            const auto object = p[at] >> 3;
            const auto freq = ((p[at] & 7U) << 1) | (p[at + 1] >> 7);
            const auto channels = (p[at + 1] >> 3) & 15U;
            if (object == 2 && freq <= 12 && channels >= 1 && channels <= 7) {
                track.aacObject = object;
                track.aacFreqIdx = freq;
                track.aacChannels = channels;
                return true;
            }
        }
        return false;
    }
    static bool parseVideoConfig(const std::vector<std::uint8_t>& b, const Box& cfg,
                                 Track& t) {
        const auto* p = b.data() + cfg.body;
        const auto n = cfg.end - cfg.body;
        if (cfg.type == tag("avcC")) {
            if (n < 7 || p[0] != 1) return false;
            t.nalBytes = (p[4] & 3U) + 1U;
            std::size_t at = 6;
            const auto spsCount = p[5] & 31U;
            for (unsigned i = 0; i < spsCount; ++i) {
                if (at + 2 > n) return false;
                const auto len = be16(p + at); at += 2;
                if (len == 0 || len > n - at) return false;
                t.parameterSets.insert(t.parameterSets.end(), {0,0,0,1});
                t.parameterSets.insert(t.parameterSets.end(), p + at, p + at + len);
                at += len;
            }
            if (at >= n) return false;
            const auto ppsCount = p[at++];
            for (unsigned i = 0; i < ppsCount; ++i) {
                if (at + 2 > n) return false;
                const auto len = be16(p + at); at += 2;
                if (len == 0 || len > n - at) return false;
                t.parameterSets.insert(t.parameterSets.end(), {0,0,0,1});
                t.parameterSets.insert(t.parameterSets.end(), p + at, p + at + len);
                at += len;
            }
            return !t.parameterSets.empty();
        }
        if (cfg.type == tag("hvcC")) {
            if (n < 23 || p[0] != 1) return false;
            t.nalBytes = (p[21] & 3U) + 1U;
            std::size_t at = 23;
            for (unsigned i = 0; i < p[22]; ++i) {
                if (at + 3 > n) return false;
                const auto type = p[at++] & 0x3fU;
                const auto count = be16(p + at); at += 2;
                for (unsigned j = 0; j < count; ++j) {
                    if (at + 2 > n) return false;
                    const auto len = be16(p + at); at += 2;
                    if (!len || len > n - at) return false;
                    if (type == 32 || type == 33 || type == 34) {
                        t.parameterSets.insert(t.parameterSets.end(), {0,0,0,1});
                        t.parameterSets.insert(t.parameterSets.end(), p + at, p + at + len);
                    }
                    at += len;
                }
            }
            return !t.parameterSets.empty();
        }
        return false;
    }
    static bool parseTrack(const std::vector<std::uint8_t>& b, const Box& mdia,
                           Track& track, std::string& error) {
        const auto mdhd = child(b, mdia, tag("mdhd"));
        const auto minf = child(b, mdia, tag("minf"));
        const auto stbl = child(b, minf, tag("stbl"));
        if (!mdhd.valid() || !stbl.valid()) { error = "invalid MP4 media metadata"; return false; }
        const auto mdhdSize = mdhd.end - mdhd.body;
        if (mdhdSize < 20) { error = "short MP4 mdhd"; return false; }
        const bool mdhdV1 = b[mdhd.body] == 1;
        const auto scalePos = mdhd.body + (mdhdV1 ? 20U : 12U);
        if (scalePos + 4 > mdhd.end || !(track.timescale = be32(b.data() + scalePos))) {
            error = "invalid MP4 timescale"; return false;
        }
        const auto stsd = child(b, stbl, tag("stsd"));
        const auto stsz = child(b, stbl, tag("stsz"));
        const auto stts = child(b, stbl, tag("stts"));
        const auto stsc = child(b, stbl, tag("stsc"));
        auto stco = child(b, stbl, tag("stco"));
        if (!stco.valid()) stco = child(b, stbl, tag("co64"));
        if (!stsd.valid() || !stsz.valid() || !stts.valid() ||
            !stsc.valid() || !stco.valid()) {
            error = "MP4 sample tables incomplete (fragmented MP4 unsupported)"; return false;
        }
        if (stsd.end - stsd.body < 16 || be32(b.data() + stsd.body + 4) == 0) {
            error = "MP4 track has no sample description"; return false;
        }
        Box entry;
        if (!boxAt(b, stsd.body + 8, stsd.end, entry)) {
            error = "invalid MP4 sample description"; return false;
        }
        if (track.video) {
            if (entry.type == tag("avc1") || entry.type == tag("avc3"))
                track.codec = mpegts::ElementaryCodec::H264;
            else if (entry.type == tag("hvc1") || entry.type == tag("hev1"))
                track.codec = mpegts::ElementaryCodec::H265;
            else { error = "unsupported MP4 video codec (need H.264 or H.265)"; return false; }
            if (entry.end - entry.body < 78) { error = "invalid MP4 video description"; return false; }
            const auto codecBox = child(b, entry,
                track.codec == mpegts::ElementaryCodec::H264 ? tag("avcC") : tag("hvcC"), 78);
            if (!codecBox.valid() || !parseVideoConfig(b, codecBox, track)) {
                error = "invalid AVC/HEVC decoder configuration in MP4"; return false;
            }
        } else {
            if (entry.type != tag("mp4a")) {
                error = "unsupported MP4 audio codec (need AAC-LC)"; return false;
            }
            if (entry.end - entry.body < 28) { error = "invalid MP4 audio description"; return false; }
            const auto esds = child(b, entry, tag("esds"), 28);
            if (!esds.valid() || esds.end - esds.body <= 6 ||
                !descriptorAudioConfig(b.data() + esds.body + 4,
                                       esds.end - esds.body - 4, track)) {
                error = "MP4 audio must be AAC-LC with valid esds configuration"; return false;
            }
            track.codec = mpegts::ElementaryCodec::AacAdts;
        }
        if (stsz.end - stsz.body < 12) { error = "invalid MP4 stsz"; return false; }
        const auto fixedSize = be32(b.data() + stsz.body + 4);
        const auto sampleCount = be32(b.data() + stsz.body + 8);
        if (sampleCount == 0 || sampleCount > 2000000U ||
            (!fixedSize && static_cast<std::uint64_t>(sampleCount) * 4U >
                stsz.end - stsz.body - 12)) {
            error = "invalid MP4 sample size table"; return false;
        }
        track.samples.resize(sampleCount);
        for (std::size_t i = 0; i < track.samples.size(); ++i)
            track.samples[i].size = fixedSize ? fixedSize :
                be32(b.data() + stsz.body + 12 + 4 * i);
        if (stts.end - stts.body < 8) { error = "invalid MP4 stts"; return false; }
        const auto times = be32(b.data() + stts.body + 4);
        if (times > (stts.end - stts.body - 8) / 8) {
            error = "invalid MP4 time-to-sample table"; return false;
        }
        std::size_t sampleIndex = 0;
        std::uint64_t rawDts = 0;
        for (std::uint32_t i = 0; i < times; ++i) {
            const auto at = stts.body + 8 + i * 8;
            const auto count = be32(b.data() + at);
            const auto delta = be32(b.data() + at + 4);
            if (count > track.samples.size() - sampleIndex || delta == 0) {
                error = "invalid MP4 sample duration"; return false;
            }
            for (std::uint32_t j = 0; j < count; ++j) {
                auto& s = track.samples[sampleIndex++];
                s.dts90k = to90k(rawDts, track.timescale);
                s.duration90k = std::max<std::uint64_t>(1, to90k(delta, track.timescale));
                rawDts += delta;
            }
        }
        if (sampleIndex != track.samples.size()) { error = "incomplete MP4 DTS table"; return false; }
        track.duration90k = to90k(rawDts, track.timescale);
        const auto ctts = child(b, stbl, tag("ctts"));
        if (ctts.valid()) {
            if (ctts.end - ctts.body < 8) { error = "invalid MP4 ctts"; return false; }
            const auto offsets = be32(b.data() + ctts.body + 4);
            if (offsets > (ctts.end - ctts.body - 8) / 8) {
                error = "invalid MP4 composition offsets"; return false;
            }
            sampleIndex = 0;
            for (std::uint32_t i = 0; i < offsets; ++i) {
                const auto at = ctts.body + 8 + i * 8;
                const auto count = be32(b.data() + at);
                const auto val = be32(b.data() + at + 4);
                if (count > track.samples.size() - sampleIndex) {
                    error = "MP4 composition offset count mismatch"; return false;
                }
                const auto ticks = b[ctts.body] == 1 ?
                    static_cast<std::int64_t>(static_cast<std::int32_t>(val)) :
                    static_cast<std::int64_t>(val);
                const auto offset = ticks < 0 ?
                    -static_cast<std::int64_t>(to90k(static_cast<std::uint64_t>(-ticks),track.timescale)) :
                    static_cast<std::int64_t>(to90k(static_cast<std::uint64_t>(ticks),track.timescale));
                for (std::uint32_t j = 0; j < count; ++j)
                    track.samples[sampleIndex++].cts90k = offset;
            }
            if (sampleIndex != track.samples.size()) { error = "incomplete MP4 CTS table"; return false; }
        }
        if (track.video) {
            const auto stss = child(b, stbl, tag("stss"));
            if (stss.valid()) {
                if (stss.end - stss.body < 8) { error = "invalid MP4 stss"; return false; }
                const auto count = be32(b.data() + stss.body + 4);
                if (count > (stss.end - stss.body - 8) / 4) {
                    error = "invalid MP4 sync sample list"; return false;
                }
                for (auto& sample : track.samples) sample.key = false;
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto id = be32(b.data() + stss.body + 8 + i * 4);
                    if (id && id <= track.samples.size()) track.samples[id-1].key = true;
                }
            }
        }
        if (stco.end - stco.body < 8) { error = "invalid MP4 chunk table"; return false; }
        const auto chunkCount = be32(b.data() + stco.body + 4);
        const auto chunkWidth = stco.type == tag("co64") ? 8U : 4U;
        if (chunkCount == 0 || chunkCount > (stco.end - stco.body - 8) / chunkWidth ||
            stsc.end - stsc.body < 8) {
            error = "invalid MP4 chunk offsets"; return false;
        }
        const auto entryCount = be32(b.data() + stsc.body + 4);
        if (entryCount == 0 || entryCount > (stsc.end - stsc.body - 8) / 12) {
            error = "invalid MP4 sample-to-chunk table"; return false;
        }
        struct Entry { std::uint32_t first, samples, desc; };
        std::vector<Entry> entries;
        for (std::uint32_t i = 0; i < entryCount; ++i) {
            const auto at = stsc.body + 8 + i * 12;
            entries.push_back({be32(b.data() + at), be32(b.data() + at + 4),
                               be32(b.data() + at + 8)});
            if (!entries.back().samples || entries.back().desc != 1 ||
                (i == 0 && entries.back().first != 1) ||
                (i && entries.back().first <= entries[i-1].first)) {
                error = "unsupported MP4 sample-to-chunk layout"; return false;
            }
        }
        sampleIndex = 0;
        std::size_t e = 0;
        for (std::uint32_t chunk = 1; chunk <= chunkCount; ++chunk) {
            while (e + 1 < entries.size() && chunk >= entries[e+1].first) ++e;
            const auto at = stco.body + 8 + (chunk - 1U) * chunkWidth;
            std::uint64_t pos = chunkWidth == 8 ? be64(b.data() + at) : be32(b.data() + at);
            if (entries[e].samples > track.samples.size() - sampleIndex) {
                error = "MP4 sample/chunk count mismatch"; return false;
            }
            for (std::uint32_t i = 0; i < entries[e].samples; ++i) {
                auto& sample = track.samples[sampleIndex++];
                sample.offset = pos;
                pos += sample.size;
            }
        }
        if (sampleIndex != track.samples.size()) {
            error = "MP4 sample offsets incomplete"; return false;
        }
        return true;
    }
    static bool annexB(const Track& t, const Sample& sample,
                       const std::vector<std::uint8_t>& bytes,
                       std::vector<std::uint8_t>& out) {
        if (t.nalBytes < 1 || t.nalBytes > 4) return false;
        if (t.codec == mpegts::ElementaryCodec::H264)
            out.insert(out.end(), {0,0,0,1,9,0xf0});
        else out.insert(out.end(), {0,0,0,1,0x46,0x01,0x50});
        if (sample.key) out.insert(out.end(), t.parameterSets.begin(), t.parameterSets.end());
        for (std::size_t at = 0; at < bytes.size();) {
            if (bytes.size() - at < t.nalBytes) return false;
            std::uint32_t len = 0;
            for (unsigned i = 0; i < t.nalBytes; ++i) len = (len << 8U) | bytes[at++];
            if (len > bytes.size() - at) return false;
            if (len) {
                out.insert(out.end(), {0,0,0,1});
                out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at),
                           bytes.begin() + static_cast<std::ptrdiff_t>(at + len));
            }
            at += len;
        }
        return out.size() > 10;
    }
    static bool adts(const Track& t, const std::vector<std::uint8_t>& bytes,
                     std::vector<std::uint8_t>& out) {
        if (!t.aacObject || !t.aacChannels || bytes.empty() ||
            bytes.size() > 8191 - 7) return false;
        const std::uint16_t total = static_cast<std::uint16_t>(bytes.size() + 7);
        out = {0xff,0xf1,
               static_cast<std::uint8_t>(((t.aacObject - 1) << 6) | (t.aacFreqIdx << 2) | (t.aacChannels >> 2)),
               static_cast<std::uint8_t>(((t.aacChannels & 3) << 6) | (total >> 11)),
               static_cast<std::uint8_t>(total >> 3),
               static_cast<std::uint8_t>(((total & 7) << 5) | 0x1f), 0xfc};
        out.insert(out.end(), bytes.begin(), bytes.end());
        return true;
    }
    std::ifstream file_;
    std::uint64_t fileSize_ = 0;
    Track video_, audio_;
    mpegts::NativeMpegTsMux mux_;
    std::uint64_t epoch90k_ = 0;
    std::uint64_t duration90k_ = 0;
    std::uint64_t loops_ = 0;
    bool loop_ = false;
};
} // namespace dvbstreamer5::media::mp4
