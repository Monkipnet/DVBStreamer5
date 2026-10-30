#include "IrdetoChidPreferenceCache.h"
#include <cassert>
#include <cstdint>
#include <iostream>

using Cache = dvbstreamer5::ca::newcamd::IrdetoChidPreferenceCache;

int main() {
    Cache cache;
    const Cache::Key domKino{"Pervy1", "sat-a-470-2", 470, 0x0652, 0x0400DC};
    const Cache::Key pobeda{"Pervy1", "sat-a-650-6", 650, 0x0652, 0x0400DC};
    const Cache::Key otherReader{"OtherReader", "sat-a-470-2", 470, 0x0652, 0x0400DC};
    const Cache::Key otherService{"Pervy1", "sat-b-470-2", 470, 0x0652, 0x0400DC};
    const Cache::Key otherCaid{"Pervy1", "sat-a-470-2", 470, 0x0604, 0x0400DC};
    std::uint16_t chid = 0;
    const std::uint64_t start = 10000;

    assert(!cache.recover(domKino, start, chid));
    cache.remember(domKino, 0, start);
    assert(!cache.recover(domKino, start, chid));

    cache.remember(domKino, 0x0134, start);
    cache.remember(pobeda, 0x0140, start);
    assert(cache.recover(domKino, start + 100, chid) && chid == 0x0134);
    assert(cache.recover(pobeda, start + 100, chid) && chid == 0x0140);
    assert(!cache.recover(otherReader, start + 100, chid));
    assert(!cache.recover(otherService, start + 100, chid));
    assert(!cache.recover(otherCaid, start + 100, chid));

    // A failed unrelated CHID cannot invalidate the successful preference.
    cache.forgetIfMatches(domKino, 0x002E);
    assert(cache.recover(domKino, start + 300, chid) && chid == 0x0134);
    cache.forgetIfMatches(domKino, 0x0134);
    assert(!cache.recover(domKino, start + 300, chid));
    assert(cache.recover(pobeda, start + 300, chid) && chid == 0x0140);

    cache.remember(domKino, 0x0134, start);
    assert(cache.recover(domKino, start + Cache::kTtlMs, chid));
    assert(!cache.recover(domKino, start + Cache::kTtlMs + 1, chid));

    cache.remember(domKino, 0x0134, start);
    assert(!cache.recover(domKino, start - 1, chid));

    static_assert(!Cache::shouldFallback(false, 9000, 4000));
    static_assert(!Cache::shouldFallback(true, 3999, 4000));
    static_assert(Cache::shouldFallback(true, 4000, 4000));
    static_assert(Cache::shouldFallback(true, 4001, 4000));

    // Bounded storage: an old entry is evicted as new unique services arrive.
    cache.remember(domKino, 0x0134, start);
    for (std::size_t n = 0; n < Cache::kMaxEntries + 5; ++n) {
        cache.remember({"Pervy1", "extra-" + std::to_string(n),
                       static_cast<std::uint16_t>(n + 1),
                       0x0652, 0x0400DC},
                       0x0131, start + n + 1);
    }
    assert(!cache.recover(domKino, start + 500, chid));
    std::cout << "PASS: successful CHID preference survives session renewal, "
                 "reader/SID separation, bounded lifetime, rejection, and safe fallback\n";
}
