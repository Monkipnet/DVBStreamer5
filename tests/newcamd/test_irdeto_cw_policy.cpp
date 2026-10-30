#include "IrdetoCwBufferPolicy.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

using Policy = dvbstreamer5::ca::newcamd::IrdetoCwBufferPolicy;
using Decision = Policy::Decision;

struct KeySlot {
    bool valid = false;
    bool pendingValid = false;
    int active = 0;
    int pending = 0;
};

struct FakeService {
    std::string name;
    std::uint8_t observedTsc = 0;
    KeySlot even;
    KeySlot odd;
    int zeroSkips = 0;
    int activations = 0;
    int stages = 0;

    explicit FakeService(std::string id) : name(std::move(id)) {}

    KeySlot& slot(std::uint8_t tsc) { return tsc == 3 ? odd : even; }

    Decision receive(std::uint8_t tsc, int cw) {
        auto& s = slot(tsc);
        const auto decision = Policy::decide(
            cw == 0, cw != s.active && (!s.pendingValid || cw != s.pending),
            s.valid, observedTsc, tsc);
        switch (decision) {
        case Decision::SkipZero:
            ++zeroSkips;
            break;
        case Decision::SkipUnchanged:
            break;
        case Decision::Stage:
            s.pendingValid = true;
            s.pending = cw;
            ++stages;
            break;
        case Decision::Install:
            s.valid = true;
            s.active = cw;
            s.pendingValid = false;
            s.pending = 0;
            break;
        }
        return decision;
    }

    void ts(std::uint8_t tsc) {
        if (!Policy::isScrambledParity(tsc)) return;
        KeySlot& s = slot(tsc);
        const bool activate = Policy::shouldActivate(s.pendingValid, observedTsc, tsc);
        observedTsc = tsc;
        if (activate) {
            s.active = s.pending;
            s.valid = true;
            s.pending = 0;
            s.pendingValid = false;
            ++activations;
        }
    }
};

int main() {
    static_assert(Policy::decide(true, true, true, 2, 2) == Decision::SkipZero);
    static_assert(Policy::decide(false, false, true, 2, 2) == Decision::SkipUnchanged);
    static_assert(Policy::decide(false, true, true, 2, 2) == Decision::Stage);
    static_assert(Policy::decide(false, true, true, 3, 2) == Decision::Install);
    static_assert(Policy::decide(false, true, false, 2, 2) == Decision::Install);
    static_assert(!Policy::shouldActivate(true, 2, 2));
    static_assert(!Policy::shouldActivate(false, 3, 2));
    static_assert(Policy::shouldActivate(true, 3, 2));
    static_assert(!Policy::shouldActivate(true, 3, 0));

    FakeService domKino("Dom Kino");
    FakeService pobeda("Pobeda");
    assert(domKino.receive(2, 101) == Decision::Install);
    assert(domKino.receive(3, 201) == Decision::Install);
    assert(pobeda.receive(2, 102) == Decision::Install);
    assert(pobeda.receive(3, 202) == Decision::Install);
    domKino.ts(2);
    pobeda.ts(2);

    assert(domKino.receive(2, 0) == Decision::SkipZero);
    assert(domKino.even.active == 101);
    assert(domKino.receive(2, 101) == Decision::SkipUnchanged);
    assert(domKino.receive(2, 103) == Decision::Stage);
    assert(domKino.even.active == 101 && domKino.even.pending == 103);
    assert(domKino.even.pendingValid);
    assert(pobeda.even.active == 102 && !pobeda.even.pendingValid);

    domKino.ts(2);
    assert(domKino.even.active == 101 && domKino.activations == 0);
    domKino.ts(3);
    assert(domKino.even.active == 101 && domKino.activations == 0);
    domKino.ts(2);
    assert(domKino.even.active == 103 && domKino.activations == 1);
    assert(!domKino.even.pendingValid);
    assert(pobeda.even.active == 102 && pobeda.activations == 0);

    assert(domKino.receive(3, 203) == Decision::Install);
    assert(domKino.odd.active == 203);
    assert(domKino.receive(2, 104) == Decision::Stage);
    assert(domKino.even.active == 103);
    domKino.ts(3);
    domKino.ts(2);
    assert(domKino.even.active == 104 && domKino.activations == 2);
    assert(pobeda.odd.active == 202 && pobeda.even.active == 102);
    assert(domKino.zeroSkips == 1 && domKino.stages == 2);

    std::cout << "PASS: isolated Irdeto EVEN/ODD policy, zero CW, duplicate CW, "
                 "staged activation, separate services, repeated rollover\n";
}
