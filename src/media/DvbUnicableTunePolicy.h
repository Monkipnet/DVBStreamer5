#pragma once

#include <string>

namespace dvbstreamer5::media::network {

// V10.8.332: keep the first successful tune sequence unchanged. When a
// frontend fails to lock through EN50494/EN50607, offer a second-stage SEC
// priming/repeat to PCIe, USB and other Linux DVB-S/S2 frontends. The specific
// STiD135 mode=2 path keeps its own driver-owned tuning sequence.
struct DvbUnicableTuneDecision {
    bool earlyPrime = false;
    bool optionalPrime = false;
    bool repeatScr = false;
    int minimumLockTimeoutMs = 0;
};

inline DvbUnicableTuneDecision selectDvbUnicableTuneDecision(
    const std::string& module, int attempt) noexcept {
    const bool knownColdStart =
        module == "cx23885" || module == "saa716x_tbs_dvb";
    // A non-lock attempt can leave the frontend/SEC logic cold. Probe with
    // S2API priming only on retries; never perturb the proven first tune on
    // drivers such as stv090x, Technisat SkyStar PCI/USB, or generic DVB USB.
    const bool genericRecovery = attempt > 1 && module != "stid135";
    DvbUnicableTuneDecision d;
    d.earlyPrime = knownColdStart || genericRecovery;
    d.optionalPrime = genericRecovery && !knownColdStart;
    d.repeatScr = module == "saa716x_tbs_dvb" || genericRecovery;
    d.minimumLockTimeoutMs = d.earlyPrime ? 8000 : 0;
    return d;
}

} // namespace dvbstreamer5::media::network
