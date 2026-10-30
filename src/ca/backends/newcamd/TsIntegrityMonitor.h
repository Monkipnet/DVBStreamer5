#pragma once

// V10.8.238: read-only MPEG-TS / decrypted-video diagnostics.
// No CW, ECM, decrypted payload, or subscriber data is retained or logged.
// Each service has its own tracker, independently of other services on the card.
#include <array>
#include <cstddef>
#include <cstdint>

namespace dvbstreamer5::ca::newcamd {

class TsIntegrityMonitor {
public:
    struct Counters {
        std::uint64_t in_packets = 0;
        std::uint64_t in_tei = 0;
        std::uint64_t in_bad_afc = 0;
        std::uint64_t in_cc_gaps = 0;
        std::uint64_t in_cc_repeats = 0;
        std::uint64_t in_discontinuities = 0;
        std::uint64_t in_pcr_backwards = 0;
        std::uint64_t in_pcr_jump_gt_1s = 0;
        std::uint64_t in_scrambled = 0;
        std::uint64_t parity_changes = 0;
        std::uint64_t missing_key_packets = 0;
        std::uint64_t post_packets = 0;
        std::uint64_t post_still_scrambled = 0;
        std::uint64_t post_pes_ok = 0;
        std::uint64_t post_pes_bad = 0;
        std::uint64_t post_video_nal_headers = 0;
        std::uint64_t post_video_nal_bad = 0;
        std::uint64_t cw_responses = 0;
        std::uint64_t ecm_not_found = 0;
        std::uint64_t ecm_latency_max_ms = 0;
        std::uint64_t ecm_latency_slow = 0;
    };

    // MPEG-TS continuity semantics: payload advances CC; adaptation-only does
    // not. A repeated payload CC is reported separately (may be duplicate TS).
    // Discontinuity_indicator suppresses a discontinuity as an error.
    void before(const std::uint8_t* packet, std::size_t size = 188) {
        if (!packet || size != 188 || packet[0] != 0x47) return;
        const auto pid = pidOf(packet);
        if (pid >= 8191) return;
        auto& p = pids_[pid];
        if (!p.selected) return;
        ++totals_.in_packets;
        if (packet[1] & 0x80) {
            ++totals_.in_tei;
            p.cc_valid = false;
        }
        const unsigned afc = (packet[3] >> 4) & 3;
        if (afc == 0) { ++totals_.in_bad_afc; p.cc_valid = false; return; }
        const bool hasAdaptation = afc == 2 || afc == 3;
        if (hasAdaptation && (5U + packet[4]) > size) {
            ++totals_.in_bad_afc; p.cc_valid = false; return;
        }
        const bool discontinuity = hasAdaptation && packet[4] > 0 &&
            (packet[5] & 0x80) != 0;
        if (discontinuity) {
            ++totals_.in_discontinuities;
            p.cc_valid = false;
            p.pcr_valid = false;
        }
        if (hasAdaptation && packet[4] >= 7 && (packet[5] & 0x10)) {
            // 33-bit PCR_base + 9-bit PCR_extension, 27 MHz timebase.
            // Counter wraps after roughly 26.5 hours. No wall-clock
            // synchronisation is assumed; >1s PCR gaps are a *heuristic*.
            const std::uint64_t base =
                (static_cast<std::uint64_t>(packet[6]) << 25) |
                (static_cast<std::uint64_t>(packet[7]) << 17) |
                (static_cast<std::uint64_t>(packet[8]) << 9) |
                (static_cast<std::uint64_t>(packet[9]) << 1) |
                (static_cast<std::uint64_t>(packet[10]) >> 7);
            const std::uint64_t ext =
                (static_cast<std::uint64_t>(packet[10] & 1) << 8) |
                packet[11];
            const std::uint64_t current = base * 300 + ext;
            constexpr std::uint64_t kWrap = (std::uint64_t{1} << 33) * 300;
            if (p.pcr_valid) {
                const bool backwards = p.last_pcr > current &&
                    p.last_pcr - current < kWrap / 2;
                if (backwards) {
                    ++totals_.in_pcr_backwards;
                } else {
                    const auto delta = current >= p.last_pcr
                        ? current - p.last_pcr : kWrap - p.last_pcr + current;
                    if (delta > 27000000ULL) ++totals_.in_pcr_jump_gt_1s;
                }
            }
            p.last_pcr = current;
            p.pcr_valid = true;
        }
        const bool hasPayload = afc == 1 ||
            (afc == 3 && (5U + packet[4]) < size);
        if (hasPayload) {
            const unsigned cc = packet[3] & 15;
            if (p.cc_valid) {
                if (cc == p.last_cc) ++totals_.in_cc_repeats;
                else if (cc != ((p.last_cc + 1U) & 15U)) ++totals_.in_cc_gaps;
            }
            p.last_cc = static_cast<std::uint8_t>(cc);
            p.cc_valid = true;
        }
        const unsigned tsc = (packet[3] >> 6) & 3;
        if (tsc == 2 || tsc == 3) {
            ++totals_.in_scrambled;
            if (p.last_tsc != 0 && p.last_tsc != tsc)
                ++totals_.parity_changes;
            p.last_tsc = static_cast<std::uint8_t>(tsc);
        }
    }

    // Called only for payload PIDs selected by the service PMT.
    void select(std::uint16_t pid, std::uint8_t streamType = 0) {
        if (pid >= 8191) return;
        auto& p = pids_[pid];
        p.selected = true;
        p.video_codec = streamType == 0x1B || streamType == 0x24
            ? streamType : 0;
    }

    void missingKey() { ++totals_.missing_key_packets; }

    // Called after the existing CSA implementation has completed in-place.
    // This is BEFORE downstream mux/UDP/HTTP output: do not call it a network
    // output CRC. Annex-B NAL checks are syntactic heuristics, not video decode.
    void after(const std::uint8_t* packet, std::size_t size = 188) {
        if (!packet || size != 188 || packet[0] != 0x47) return;
        const auto pid = pidOf(packet);
        if (pid >= 8191) return;
        auto& p = pids_[pid];
        if (!p.selected) return;
        ++totals_.post_packets;
        const unsigned tsc = (packet[3] >> 6) & 3;
        if (tsc != 0) {
            ++totals_.post_still_scrambled;
            p.nal_zeroes = 0;
            p.expect_nal_header = 0;
            return;
        }
        if (!p.video_codec) return;
        const unsigned afc = (packet[3] >> 4) & 3;
        if (afc != 1 && afc != 3) return;
        const std::size_t offset = afc == 1 ? 4U : 5U + packet[4];
        if (offset >= size) return;
        const auto* payload = packet + offset;
        const std::size_t length = size - offset;
        std::size_t videoOffset = 0;
        if (packet[1] & 0x40) {
            // PUSI video payloads start with an MPEG PES header. Do not scan
            // start codes in its metadata (PTS/DTS).
            p.nal_zeroes = 0;
            p.expect_nal_header = 0;
            if (length < 9 || payload[0] != 0 || payload[1] != 0 ||
                payload[2] != 1 || (payload[3] & 0xF0) != 0xE0 ||
                (payload[6] & 0xC0) != 0x80) {
                ++totals_.post_pes_bad;
                return;
            }
            ++totals_.post_pes_ok;
            videoOffset = 9U + payload[8];
            if (videoOffset > length) return; // PES header spans packets.
        }
        for (std::size_t i = videoOffset; i < length; ++i) {
            const std::uint8_t b = payload[i];
            if (p.expect_nal_header != 0) {
                if (p.expect_nal_header == 2) {
                    // First HEVC NAL byte carries forbidden_zero_bit; second
                    // byte must have temporal_id_plus1 != 0.
                    if (b & 0x80) ++totals_.post_video_nal_bad;
                    p.expect_nal_header = 3;
                } else if (p.expect_nal_header == 3) {
                    if ((b & 0x07) == 0) ++totals_.post_video_nal_bad;
                    ++totals_.post_video_nal_headers;
                    p.expect_nal_header = 0;
                } else {
                    if ((b & 0x80) || (b & 0x1F) == 0)
                        ++totals_.post_video_nal_bad;
                    ++totals_.post_video_nal_headers;
                    p.expect_nal_header = 0;
                }
                p.nal_zeroes = 0;
                continue;
            }
            if (b == 1 && p.nal_zeroes >= 2) {
                p.expect_nal_header = p.video_codec == 0x24 ? 2 : 1;
                p.nal_zeroes = 0;
            } else {
                p.nal_zeroes = b == 0
                    ? static_cast<std::uint8_t>(p.nal_zeroes < 3
                        ? p.nal_zeroes + 1 : 3)
                    : 0;
            }
        }
    }

    void onEcmResult(bool found, std::uint64_t latencyMs) {
        if (!found) { ++totals_.ecm_not_found; return; }
        ++totals_.cw_responses;
        if (latencyMs > totals_.ecm_latency_max_ms)
            totals_.ecm_latency_max_ms = latencyMs;
        if (latencyMs > 500) ++totals_.ecm_latency_slow;
    }

    const Counters& counters() const { return totals_; }

private:
    struct PidState {
        bool selected = false;
        bool cc_valid = false;
        bool pcr_valid = false;
        std::uint64_t last_pcr = 0;
        std::uint8_t last_cc = 0;
        std::uint8_t last_tsc = 0;
        std::uint8_t video_codec = 0;
        std::uint8_t nal_zeroes = 0;
        std::uint8_t expect_nal_header = 0;
    };
    static std::uint16_t pidOf(const std::uint8_t* p) {
        return static_cast<std::uint16_t>(((p[1] & 0x1F) << 8) | p[2]);
    }

    std::array<PidState, 8192> pids_{};
    Counters totals_{};
};

} // namespace dvbstreamer5::ca::newcamd
