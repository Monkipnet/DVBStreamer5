// V515: hardware-free PCR smoothing regression for a large H.264 P-frame.
// Tests the *actual* CbrTsPacer output, not just its timestamp helper.
#include "media/CbrTsPacer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>

using namespace dvbstreamer5::media::mpegts;

static constexpr std::uint64_t kHz = 27000000ULL;
static constexpr std::uint64_t kWrap = (1ULL << 33U) * 300ULL;
static constexpr std::uint64_t kRate = 8000000ULL;
static constexpr std::uint16_t kPid = 0x0102;

static void need(bool value, const char* what) {
    if (!value) { std::cerr << what << '\n'; std::exit(1); }
}
static void writePcr(Packet& p, std::uint64_t value) {
    value %= kWrap;
    const auto base = value / 300, ext = value % 300;
    p[6] = base >> 25; p[7] = base >> 17;
    p[8] = base >> 9; p[9] = base >> 1;
    p[10] = ((base & 1ULL) << 7) | 0x7eU | (ext >> 8);
    p[11] = ext;
}
static std::uint64_t readPcr(const Packet& p) {
    const auto base = (std::uint64_t(p[6]) << 25) |
        (std::uint64_t(p[7]) << 17) |
        (std::uint64_t(p[8]) << 9) |
        (std::uint64_t(p[9]) << 1) | (p[10] >> 7);
    return base * 300ULL + ((p[10] & 1U) << 8) + p[11];
}
static void writeTimestamp(Packet& p, unsigned at, std::uint64_t pts) {
    p[at] = 0x21 | ((pts >> 29) & 0x0e);
    p[at+1] = pts >> 22;
    p[at+2] = ((pts >> 14) & 0xfe) | 1;
    p[at+3] = pts >> 7;
    p[at+4] = ((pts << 1) & 0xfe) | 1;
}
static Packet makeVideoPacket(unsigned frame, unsigned packetIndex,
                              unsigned cc, std::uint64_t pcr) {
    Packet p {};
    p.fill(static_cast<std::uint8_t>(0x40U + (frame & 0x3fU)));
    p[0] = 0x47;
    p[1] = static_cast<std::uint8_t>((kPid >> 8) | (packetIndex == 0 ? 0x40 : 0));
    p[2] = static_cast<std::uint8_t>(kPid);
    p[3] = static_cast<std::uint8_t>((packetIndex == 0 ? 0x30U : 0x10U) | (cc & 15));
    if (packetIndex == 0) {
        p[4] = 7; p[5] = 0x10; writePcr(p, pcr);
        // PES header with PTS, kept byte-for-byte by the new output policy.
        p[12] = 0; p[13] = 0; p[14] = 1; p[15] = 0xe0;
        p[16] = 0; p[17] = 0; p[18] = 0x80; p[19] = 0x80;
        p[20] = 5; writeTimestamp(p, 21, (pcr / 300 + 45000ULL) & ((1ULL<<33)-1));
    }
    return p;
}

static void checkPolicy() {
    need(!CbrOutputPcrSmoothingPolicy::selected("A", nullptr), "default selector off");
    need(!CbrOutputPcrSmoothingPolicy::selected("A", "B"), "other stream off");
    need(CbrOutputPcrSmoothingPolicy::selected("A", "A"), "selected stream on");
    need(!CbrTsPacer(8000000ULL, true, true).outputPcrSmoothing(),
         "Fixed-PCR mode must remain unchanged");
    need(!CbrTsPacer(8000799ULL, true, true).outputPcrSmoothing(),
         "zero-buffer profile must remain unchanged");
    need(CbrTsPacer(8000809ULL, true, true).outputPcrSmoothing(),
         "transcoded buffered Source-PCR test enabled");
    need(!CbrTsPacer(8000809ULL, true).outputPcrSmoothing(),
         "unselected source-PCR retains V513 bytes");
    need(CbrOutputPcrSmoothingPolicy::due(107, 0, kRate),
         "20 ms at 8M needs ~107 TS slots");
    need(!CbrOutputPcrSmoothingPolicy::due(100, 0, kRate),
         "20 ms threshold not reached");
}

static void simulateLargePes() {
    CbrTsPacer pcr(8000809ULL, true, true); // 8Mbps + Source-PCR 5s
    std::deque<Packet> expected;
    const auto epoch = std::chrono::steady_clock::time_point(std::chrono::seconds(1));
    constexpr unsigned kFrames = 120;
    unsigned cc = 0;
    for (unsigned frame = 0; frame < kFrames; ++frame) {
        // 40-ms PCRs; a single 900-packet P-frame is impossible in 40 ms
        // at 8 Mbps. The CBR output will transmit it over ~169 ms.
        const unsigned packets = frame == 42 ? 900U : 80U;
        const auto pcrTicks = (kHz + frame * (kHz / 25ULL)) % kWrap;
        for (unsigned n = 0; n < packets; ++n) {
            const auto src = makeVideoPacket(frame, n, cc++, pcrTicks);
            need(pcr.enqueueAt(src, epoch), "enqueue source PES");
            expected.push_back(src);
        }
    }
    std::uint64_t slot = 0, lastPcrSlot = 0, firstPcrSlot = 0;
    std::uint64_t firstPcr = 0, previousPcr = 0;
    std::uint64_t originalPackets = 0, synthetic = 0, pcrs = 0;
    std::uint8_t lastPayloadCc = 0;
    bool havePcr = false, havePayload = false;

    // Sender owns physical 8Mbps datagram clock. The virtual loop makes
    // enough slots to empty 120 frames (4.8 s) with no wall-clock sleeps.
    for (unsigned dg = 0; dg < 6500; ++dg) {
        CbrDatagram out;
        need(pcr.emitDatagram(epoch + std::chrono::milliseconds(dg), out),
             "emit WISI CBR packet");
        for (const auto& pkt : out) {
            const auto pid = ((pkt[1] & 31U) << 8U) | pkt[2];
            const auto afc = (pkt[3] >> 4U) & 3U;
            if (pid == kPid) {
                const bool clockOnly = afc == 2U;
                if (clockOnly) {
                    ++synthetic;
                    need(pkt[4] == 183 && pkt[5] == 0x10,
                         "clock-only PCR must be adaptation-only");
                    need(havePayload && (pkt[3] & 15U) == lastPayloadCc,
                         "PCR-only adaptation packet changed payload CC");
                } else {
                    need(!expected.empty(), "unexpected video payload");
                    auto src = expected.front();
                    expected.pop_front();
                    ++originalPackets;
                    // Only PCR fields can change; PES, PTS, CC and H.264 data
                    // remain identical. Never discard/alter useful input.
                    if (src[3] & 0x20) {
                        for (unsigned b = 6; b <= 11; ++b) src[b] = pkt[b];
                    }
                    need(pkt == src, "video/PES bytes or CC changed");
                    need((pkt[3] & 0x10U) != 0, "lost video payload");
                    lastPayloadCc = pkt[3] & 15U;
                    havePayload = true;
                }
                if (pkt[3] & 0x20U && pkt[4] >= 7U && (pkt[5] & 0x10U)) {
                    const auto ticks = readPcr(pkt);
                    if (!havePcr) {
                        firstPcr = ticks;
                        firstPcrSlot = slot;
                        havePcr = true;
                    } else {
                        const auto expectedTicks =
                            CbrOutputPcrSmoothingPolicy::outputPcr(
                                firstPcr, slot - firstPcrSlot, kRate);
                        const auto delta = (ticks + kWrap - expectedTicks) % kWrap;
                        need(delta < 2ULL || delta > kWrap - 2ULL,
                             "PCR not synchronized with physical output slot");
                        const auto gapTicks = (ticks + kWrap - previousPcr) % kWrap;
                        need(gapTicks <= kHz * 21ULL / 1000ULL,
                             "active CBR PCR gap exceeded 21ms");
                    }
                    lastPcrSlot = slot;
                    previousPcr = ticks;
                    ++pcrs;
                }
            } else {
                need(pid == 0x1fffU, "unexpected PID");
            }
            ++slot;
        }
        if (expected.empty()) break;
    }
    need(expected.empty(), "lost or excessively delayed source PES payload");
    need(originalPackets == 80ULL * kFrames + 820ULL, "incorrect payload count");
    need(synthetic > 100 && pcrs > 200,
         "did not generate 20ms PCR during real media transmission");
    need(pcr.clockOnlyPcrPackets() == synthetic, "clock-only counter mismatch");
    need(pcr.sourceOverTargetWindows() < 10,
         "source bitrate fixture outside WISI target budget");
    std::cout << "V515 20ms PCR, 900-packet P-PES, byte-exact A/V: PASS"
              << " original=" << originalPackets
              << " clock_only=" << synthetic << " pcr=" << pcrs << '\n';
    (void)lastPcrSlot;
}

int main() {
    checkPolicy();
    simulateLargePes();
    std::cout << "CBR output PCR smoothing: OK\n";
}
