#include "media/NativeVideoPesPacer.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>

using dvbstreamer5::media::transcode::NativeVideoPesPacer;
using namespace std::chrono_literals;

int main() {
    using Clock = NativeVideoPesPacer::Clock;
    const Clock::time_point t{};
    NativeVideoPesPacer pacer;
    const auto first = pacer.plan(t, true, 90000);
    assert(first.deadline == t + 40ms);
    const auto second = pacer.plan(t + 1ms, true, 93600);
    assert(second.deadline == t + 80ms && second.usedDts);
    auto now = second.deadline;
    for (int i = 2; i < 30; ++i) {
        const auto p = pacer.plan(now, true, 90000 + i * 3600ULL);
        assert(p.deadline >= now);
        assert(p.deadline - now <= 200ms);
        assert(p.deadline == now + 40ms);
        now = p.deadline;
    }
    pacer.reset();
    (void)pacer.plan(t, true, (1ULL << 33U) - 1800);
    const auto wrap = pacer.plan(t + 1ms, true, 1800);
    assert(wrap.usedDts && !wrap.resynced && wrap.deadline == t + 80ms);
    pacer.reset();
    (void)pacer.plan(t, false, 40000);
    const auto noDts = pacer.plan(t + 1ms, false, 1000);
    assert(!noDts.usedDts && noDts.deadline == t + 80ms);
    const auto jump = pacer.plan(t + 2ms, true, 700000);
    assert(jump.deadline >= t + 2ms);
    const auto bad = pacer.plan(t + 3ms, true, 1400000);
    assert(bad.resynced && bad.deadline == t + 43ms);
    pacer.reset();
    bool capped = false;
    for (int i = 0; i < 30; ++i) {
        const auto p = pacer.plan(t, false, 0);
        assert(p.deadline <= t + 200ms);
        capped = capped || p.budgetCapped;
    }
    assert(capped);
    std::cout << "NativeVideoPesPacer PASS\n";
}
