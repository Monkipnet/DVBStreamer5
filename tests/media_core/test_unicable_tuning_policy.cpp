#include "media/DvbUnicableTunePolicy.h"

#include <cstdlib>
#include <iostream>

using dvbstreamer5::media::network::selectDvbUnicableTuneDecision;

static void check(bool value, const char* description) {
    if (!value) {
        std::cerr << "Unicable tuning policy test failed: " << description << '\n';
        std::exit(1);
    }
}

int main() {
    // All non-TBS/cx frontends keep the proven initial tune unchanged.
    for (const auto* driver : {"dvb_usb_technisat_usb2", "dvb_usb_dib0700",
                               "stv090x", "technisat_usb2", "dvb_usb_v2",
                               "unknown"}) {
        const auto first = selectDvbUnicableTuneDecision(driver, 1);
        check(!first.earlyPrime && !first.optionalPrime && !first.repeatScr,
              "generic DVB PCIe/USB first tune preserved");
        const auto second = selectDvbUnicableTuneDecision(driver, 2);
        check(second.earlyPrime && second.optionalPrime && second.repeatScr,
              "generic DVB PCIe/USB recovery after no LOCK");
        check(second.minimumLockTimeoutMs == 8000, "recovery lock timeout floor");
    }

    // Existing verified hardware compatibility stays enabled on attempt 1.
    const auto tbs = selectDvbUnicableTuneDecision("saa716x_tbs_dvb", 1);
    check(tbs.earlyPrime && !tbs.optionalPrime && tbs.repeatScr,
          "TBS saa716x cold start preserved");
    const auto cx = selectDvbUnicableTuneDecision("cx23885", 1);
    check(cx.earlyPrime && !cx.optionalPrime && !cx.repeatScr,
          "cx23885 STV090x initial SEC order preserved");

    // STiD135 mode=2 remains solely controlled by the driver, with existing
    // bounded frontend reopen/tune retry and no generic priming.
    for (int attempt=1; attempt<=3; ++attempt) {
        const auto stid = selectDvbUnicableTuneDecision("stid135", attempt);
        check(!stid.earlyPrime && !stid.repeatScr,
              "STiD135 native Unicable path unchanged");
    }
    std::cout << "Unicable tuning policy tests OK\n";
    return 0;
}
