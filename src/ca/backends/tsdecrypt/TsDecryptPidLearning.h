#pragma once
#include <cstdint>

namespace dvbstreamer5::ca::tsdecrypt_detail {

// Key-free, deterministic decision logic shared by the watchdog and tests.
// A CW signal or a cleared scrambling bit alone must never train the PID cache.
struct PidLearningProbe {
    uint64_t nowMs = 0;
    uint64_t childStartedMs = 0;
    uint64_t pidSelectedSinceMs = 0;
    uint64_t lastEncryptedInputMs = 0;
    uint64_t lastClearMediaPesMs = 0;
    uint64_t lastCamdErrorMs = 0;
    uint64_t clearVideoPesSincePidSelection = 0;
    uint64_t cardDenials = 0;
    uint32_t selectedPid = 0;
    uint32_t forcedPid = 0;
    bool cwSignaled = false;
};

inline bool canRememberEcmPid(const PidLearningProbe& p) noexcept {
    if (!p.selectedPid || p.selectedPid >= 8191 || !p.cwSignaled ||
        (p.forcedPid && p.forcedPid != p.selectedPid) ||
        !p.childStartedMs || !p.pidSelectedSinceMs ||
        p.nowMs < p.pidSelectedSinceMs ||
        p.nowMs - p.pidSelectedSinceMs < 15000 ||
        p.clearVideoPesSincePidSelection < 24 ||
        !p.lastClearMediaPesMs || p.nowMs < p.lastClearMediaPesMs ||
        p.nowMs - p.lastClearMediaPesMs > 5000)
        return false;
    // A genuinely decoded picture must be associated with the *current*
    // ECM PID, after the last switch, and without a recent CAMD denial.
    if (p.lastCamdErrorMs && p.nowMs >= p.lastCamdErrorMs &&
        p.nowMs - p.lastCamdErrorMs <= 12000)
        return false;
    return true;
}

// Only an automatically cached -X hint may fall back to auto. A user-specified
// -X PID remains strictly manual. This transition is one-shot per child start.
inline bool shouldAbandonCachedEcmPid(const PidLearningProbe& p,
                                     bool usingCachedHint) noexcept {
    if (!usingCachedHint || !p.childStartedMs ||
        p.nowMs < p.childStartedMs ||
        p.nowMs - p.childStartedMs < 45000 ||
        !p.lastEncryptedInputMs || p.nowMs < p.lastEncryptedInputMs ||
        p.nowMs - p.lastEncryptedInputMs > 5000)
        return false;
    if (p.lastClearMediaPesMs && p.nowMs >= p.lastClearMediaPesMs &&
        p.nowMs - p.lastClearMediaPesMs <= 30000)
        return false;
    // Do not restart on a short upstream outage. Without any clear PES,
    // retry auto after sustained reader rejection or a full 70s grace.
    return p.cardDenials >= 3 ||
           p.nowMs - p.childStartedMs >= 70000;
}

} // namespace dvbstreamer5::ca::tsdecrypt_detail
