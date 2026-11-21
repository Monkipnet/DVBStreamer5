#pragma once

// V10.8.614: passive, explicitly targeted audio/video PES diagnostics.
// Does not store, decrypt, rewrite or log media payloads, ECMs or keys.
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace dvbstreamer5::media::mpegts {

class TsAvStageTrace {
public:
    struct Track {
        std::uint16_t pid = 0x1fff;
        std::uint64_t packets = 0;
        std::uint64_t scrambled = 0;
        std::uint64_t tei = 0;
        std::uint64_t pusi = 0;
        std::uint64_t clearPes = 0;
        std::uint64_t badPes = 0;
        std::uint64_t private1Pes = 0;
        std::uint64_t pts = 0;
        std::uint64_t ptsBackward = 0;
        std::uint64_t ptsForwardOver200 = 0;
        std::uint64_t ptsMaxForwardMs = 0;
        std::uint64_t wallGapOver200 = 0;
        std::uint64_t wallGapMaxMs = 0;
        std::uint64_t lastPts90k = 0;
        std::uint64_t lastWallMs = 0;
        bool hasPts = false;
        bool hasWall = false;
    };

    TsAvStageTrace(std::string streamId, std::string stage,
                   std::uint16_t videoPid, std::uint16_t audioPid)
        : streamId_(std::move(streamId)),
          stage_(std::move(stage)) {
        tracks_[0].pid = videoPid;
        tracks_[1].pid = audioPid;
    }

    // Inspect only complete 188-byte packets (as provided by NativeUdpRelay).
    // Invalid PES starts are counted only on clear target media PIDs; PAT/PMT
    // and encrypted PUSI packets never count as failed media PES.
    void inspect(const std::uint8_t* data, std::size_t size) {
        if (!data || size < 188U) return;
        const auto time = monotonicMs();
        for (std::size_t pos = 0; pos + 188U <= size; pos += 188U) {
            const std::uint8_t* p = data + pos;
            if (p[0] != 0x47U) continue;
            const std::uint16_t pid = static_cast<std::uint16_t>(
                ((p[1] & 0x1fU) << 8) | p[2]);
            for (std::size_t index = 0; index < tracks_.size(); ++index) {
                auto& t = tracks_[index];
                if (t.pid != pid) continue;
                ++t.packets;
                if ((p[1] & 0x80U) != 0) { ++t.tei; break; }
                const std::uint8_t afc = (p[3] >> 4) & 0x03U;
                if (afc == 0U) break;
                if ((p[3] & 0xc0U) != 0U) ++t.scrambled;
                if (!t.hasWall) {
                    t.hasWall = true;
                } else if (time >= t.lastWallMs) {
                    const std::uint64_t gap = time - t.lastWallMs;
                    if (gap > 200U) ++t.wallGapOver200;
                    if (gap > t.wallGapMaxMs) t.wallGapMaxMs = gap;
                }
                t.lastWallMs = time;
                const bool pusi = (p[1] & 0x40U) != 0U;
                if (!pusi) break;
                ++t.pusi;
                if ((p[3] & 0xc0U) != 0U || afc == 2U) break;
                std::size_t start = 4;
                if (afc == 3U) {
                    if (p[4] > 182U) { ++t.badPes; break; }
                    start = 5U + p[4];
                }
                if (start + 9U > 188U ||
                    p[start] != 0 || p[start+1] != 0 ||
                    p[start+2] != 1) {
                    ++t.badPes;
                    break;
                }
                const std::uint8_t stream = p[start+3];
                const bool expected = index == 0
                    ? (stream >= 0xe0U && stream <= 0xefU)
                    : ((stream >= 0xc0U && stream <= 0xdfU) || stream == 0xbdU);
                if (!expected) { ++t.badPes; break; }
                ++t.clearPes;
                if (stream == 0xbdU) ++t.private1Pes;
                if ((p[start+7] & 0x80U) == 0U ||
                    p[start+8] < 5U || start + 14U > 188U) break;
                const auto* v = p + start + 9U;
                if ((v[0] & 1U) == 0U || (v[2] & 1U) == 0U ||
                    (v[4] & 1U) == 0U) break;
                const std::uint64_t pts =
                    ((static_cast<std::uint64_t>(v[0] >> 1U) & 7U) << 30U) |
                    (static_cast<std::uint64_t>(v[1]) << 22U) |
                    ((static_cast<std::uint64_t>(v[2] >> 1U) & 0x7fU) << 15U) |
                    (static_cast<std::uint64_t>(v[3]) << 7U) |
                    (static_cast<std::uint64_t>(v[4] >> 1U) & 0x7fU);
                if (t.hasPts) {
                    constexpr std::uint64_t kMask = (1ULL << 33U)-1U;
                    const std::uint64_t forward = (pts - t.lastPts90k) & kMask;
                    if (forward > (1ULL << 32U)) {
                        ++t.ptsBackward; // B-frame video PTS reordering is normal.
                    } else {
                        const auto ms = (forward * 1000ULL) / 90000ULL;
                        if (ms > 200U) ++t.ptsForwardOver200;
                        if (ms > t.ptsMaxForwardMs) t.ptsMaxForwardMs = ms;
                    }
                }
                t.lastPts90k = pts;
                t.hasPts = true;
                ++t.pts;
                break;
            }
        }
        if (lastReportMs_ == 0U) lastReportMs_ = time;
        if (time - lastReportMs_ >= 5000U) {
            report();
            lastReportMs_ = time;
        }
    }

    const Track& stats(bool audio) const noexcept {
        return tracks_[audio ? 1U : 0U];
    }

    void report() {
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            const auto& t = tracks_[i];
            auto& previous = previous_[i];
            std::cerr << "TS AV STAGE DIAG stream=" << streamId_
                      << " stage=" << stage_
                      << " kind=" << (i == 0U ? "video" : "audio")
                      << " pid=" << t.pid
                      << " packets=" << t.packets
                      << " packets_delta=" << (t.packets - previous.packets)
                      << " scrambled=" << t.scrambled
                      << " scrambled_delta=" << (t.scrambled - previous.scrambled)
                      << " tei=" << t.tei
                      << " pusi=" << t.pusi
                      << " clear_pes=" << t.clearPes
                      << " clear_pes_delta=" << (t.clearPes - previous.clearPes)
                      << " bad_pes=" << t.badPes
                      << " bad_pes_delta=" << (t.badPes - previous.badPes)
                      << " private1_pes=" << t.private1Pes
                      << " pts=" << t.pts
                      << " pts_backward=" << t.ptsBackward
                      << " pts_forward_over200=" << t.ptsForwardOver200
                      << " pts_max_forward_ms=" << t.ptsMaxForwardMs
                      << " wall_gap_over200=" << t.wallGapOver200
                      << " wall_gap_max_ms=" << t.wallGapMaxMs
                      << " diagnostic_only=1" << std::endl;
            previous = t;
        }
    }

private:
    static std::uint64_t monotonicMs() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    std::string streamId_;
    std::string stage_;
    std::array<Track, 2> tracks_{};
    std::array<Track, 2> previous_{};
    std::uint64_t lastReportMs_ = 0;
};

} // namespace dvbstreamer5::media::mpegts
