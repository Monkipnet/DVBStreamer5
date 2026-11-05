#include "media/CbrTsPacer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>

using namespace dvbstreamer5::media::mpegts;
using Clock = std::chrono::steady_clock;
static constexpr std::uint64_t kHz = 27000000ULL;
static constexpr std::uint64_t kWrap = (1ULL << 33U) * 300ULL;
static constexpr std::uint64_t kBitrate = 3000000ULL;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

static Packet packet(std::uint64_t index, std::uint64_t pcr,
                     bool withPcr = true, bool discontinuity = false) {
    Packet p;
    p.fill(0xff);
    p[0] = 0x47; p[1] = 0x41; p[2] = 0; // PID 256, PES start
    p[3] = static_cast<std::uint8_t>((withPcr ? 0x30 : 0x10) | (index & 15U));
    if (withPcr) {
        p[4] = 7; p[5] = discontinuity ? 0x90 : 0x10;
        const auto base = (pcr % kWrap) / 300ULL, ext = pcr % 300ULL;
        p[6] = base >> 25U; p[7] = base >> 17U; p[8] = base >> 9U;
        p[9] = base >> 1U; p[10] = ((base & 1U) << 7U) | 0x7eU | (ext >> 8U);
        p[11] = ext;
    }
    const unsigned start = withPcr ? 12 : 4;
    p[start] = 0; p[start+1] = 0; p[start+2] = 1; p[start+3] = 0xe0;
    p[start+4] = 0; p[start+5] = 0; p[start+6] = 0x80;
    p[start+7] = 0xc0; p[start+8] = 10; // both PTS and DTS
    const auto timestamp = [&](unsigned at, std::uint64_t value, unsigned prefix) {
        p[at] = prefix | ((value >> 29U) & 14U) | 1U;
        p[at+1] = value >> 22U; p[at+2] = ((value >> 14U) & 0xfeU) | 1U;
        p[at+3] = value >> 7U; p[at+4] = ((value << 1U) & 0xfeU) | 1U;
    };
    timestamp(start+9, pcr/300ULL + 45000ULL, 0x30);
    timestamp(start+14, pcr/300ULL + 40000ULL, 0x10);
    return p;
}

static std::uint64_t readPcr(const Packet& p) {
    const auto base = (std::uint64_t(p[6]) << 25U) | (std::uint64_t(p[7]) << 17U) |
        (std::uint64_t(p[8]) << 9U) | (std::uint64_t(p[9]) << 1U) | (p[10] >> 7U);
    return base * 300ULL + ((p[10] & 1U) << 8U) + p[11];
}

// Ten virtual minutes: no sleeping and no hardware. Queue the actual packet
// bytes so any reordering, loss, PCR/PTS/DTS/CC modification fails the test.
static long double simulate(bool precise, bool resetClock) {
    CbrTsPacer pacer(3000808ULL, precise); // user's exact 4500-ms source profile
    std::deque<Packet> expected;
    const auto epoch = Clock::time_point(std::chrono::seconds(1));
    std::uint64_t nextInput = 0, nextPcr = 0, index = 0, sent = 0;
    std::uint64_t outputSlots = 0, firstPcr = 0, previousPcr = 0;
    long double firstOutput = 0, elapsedSource = 0;
    long double minimum = std::numeric_limits<long double>::max();
    long double maximum = -minimum;
    bool havePcr = false, resetDone = false;
    std::uint64_t resetOffset = 0;
    std::uint64_t pcrSamples = 0;
    unsigned nullCc = 0;
    for (std::uint64_t datagramIndex = 0;; ++datagramIndex) {
        const auto wall = datagramIndex * 1316ULL * 8ULL * 1000000000ULL / kBitrate;
        if (wall > 600000000000ULL) break;
        while (nextInput <= wall) {
            const auto rate = resetClock ? 2500000ULL :
                ((nextInput / 15000000000ULL) & 1U ? 2800000ULL : 2000000ULL);
            const bool withPcr = nextInput >= nextPcr;
            const bool reset = resetClock && withPcr && !resetDone && nextInput > 7000000000ULL;
            if (reset) { resetDone = true; resetOffset = 5ULL * kHz; }
            const auto ticks = (kWrap - kHz + nextInput * kHz / 1000000000ULL - resetOffset) % kWrap;
            auto p = packet(index++, ticks, withPcr, reset);
            require(pacer.enqueueAt(p, epoch + std::chrono::nanoseconds(nextInput)), "queue overflow below target");
            expected.push_back(p);
            if (withPcr) nextPcr = nextInput + 20000000ULL;
            nextInput += 188ULL * 8ULL * 1000000000ULL / rate;
        }
        if (wall < 4500000000ULL) continue;
        CbrDatagram datagram;
        require(pacer.emitDatagram(epoch + std::chrono::nanoseconds(wall), datagram), "missing CBR datagram");
        static_assert(sizeof(datagram) == 1316, "WISI datagram format");
        for (const auto& p : datagram) {
            const auto slot = outputSlots++;
            const auto pid = ((p[1] & 31U) << 8U) | p[2];
            if (pid == 0x1fffU) {
                require((p[3] & 15U) == (nullCc++ & 15U), "NULL continuity changed");
                continue;
            }
            require(!expected.empty() && p == expected.front(), "media bytes/order/CC/PCR/PTS/DTS changed");
            expected.pop_front(); ++sent;
            if ((p[3] & 0x20U) == 0) continue;
            const auto ticks = readPcr(p);
            const auto outputTime = static_cast<long double>(slot) * 188ULL * 8ULL * kHz / kBitrate;
            if (!havePcr) {
                havePcr = true; firstPcr = previousPcr = ticks; firstOutput = outputTime;
            } else {
                // Discontinuities are legal but must not discard queued data.
                if ((p[5] & 0x80U) == 0) elapsedSource += (ticks + kWrap - previousPcr) % kWrap;
                else elapsedSource += 20000000.0L * kHz / 1000000000.0L;
                previousPcr = ticks;
            }
            (void)firstPcr;
            const auto phase = (outputTime - firstOutput - elapsedSource) * 1000.0L / kHz;
            minimum = std::min(minimum, phase); maximum = std::max(maximum, phase);
            ++pcrSamples;
        }
    }
    require(pcrSamples > 29000 && sent > 800000, "long-duration test did not exercise media");
    require(pacer.sourceOverTargetWindows() == 0, "fixture exceeds configured target");
    std::cout << (precise ? "PCR intervals" : "legacy average")
              << (resetClock ? " wrap/reset" : " VBR") << ": phase range "
              << static_cast<double>(minimum) << ".." << static_cast<double>(maximum) << " ms\n";
    return maximum - minimum;
}

static void legacyAndFallback() {
    for (const auto target : {3000000ULL, 3000699ULL, 3000799ULL, 3000808ULL}) {
        CbrTsPacer original(target), disabled(target, false), enabled(target, true);
        const auto time = Clock::time_point(std::chrono::seconds(1));
        // With no PCR both paths must use the original rate fallback.
        for (unsigned i = 0; i < 400; ++i) {
            const auto p = packet(i, 0, false);
            require(original.enqueueAt(p,time) && disabled.enqueueAt(p,time) && enabled.enqueueAt(p,time), "fallback enqueue");
        }
        for (unsigned i = 0; i < 150; ++i) {
            CbrDatagram a,b,c;
            const auto now = time + std::chrono::milliseconds(i * 4);
            require(original.emitDatagram(now,a) && disabled.emitDatagram(now,b) && enabled.emitDatagram(now,c), "fallback emit");
            require(a == b && a == c, "legacy/no-PCR fallback changed");
        }
        if (target != 3000808ULL) require(!enabled.sourceTimelinePacing(), "fixed/zero-buffer mode changed");
    }
    // Even with real PCRs, the opt-in must never change Fixed PCR behavior.
    CbrTsPacer oldFixed(3000000), optInFixed(3000000,true);
    for (unsigned i = 0; i < 400; ++i) {
        auto p = packet(i, kHz + i * 27000ULL);
        require(oldFixed.enqueue(p) && optInFixed.enqueue(p), "fixed enqueue");
    }
    for (unsigned i = 0; i < 150; ++i) {
        CbrDatagram a,b;
        const auto now = Clock::time_point(std::chrono::seconds(20));
        require(oldFixed.emitDatagram(now,a) && optInFixed.emitDatagram(now,b), "fixed emit");
        require(a == b, "Fixed PCR packet changes");
    }
}

int main() {
    legacyAndFallback();
    require(simulate(false,false) > 500.0L, "fixture no longer reproduces the legacy VBR phase error");
    // One TS slot is 0.5014 ms at 3 Mbit/s. PCRs stay on that timeline even
    // while the useful bitrate changes; UDP packet grouping remains 7x188.
    require(simulate(true,false) < 0.502L, "source PCR delivery exceeds one TS slot");
    require(simulate(true,true) < 2.0L, "wrap/discontinuity caused a timeline jump");
    std::cout << "Source PCR packet pacing: OK\n";
}
