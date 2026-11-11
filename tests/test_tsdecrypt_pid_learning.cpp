#include "../src/ca/backends/tsdecrypt/TsDecryptPidLearning.h"
#include <cassert>
#include <cstdio>
using namespace dvbstreamer5::ca::tsdecrypt_detail;
int main() {
    PidLearningProbe p{};
    p.nowMs = 100000;
    p.childStartedMs = 60000;
    p.pidSelectedSinceMs = 80000;
    p.selectedPid = 7254;
    p.lastClearMediaPesMs = 99500;
    p.clearVideoPesSincePidSelection = 40;
    p.cwSignaled = true;
    assert(canRememberEcmPid(p));
    p.lastCamdErrorMs = 95000;
    assert(!canRememberEcmPid(p)); // reader denied recently
    p.lastCamdErrorMs = 0;
    p.clearVideoPesSincePidSelection = 0;
    assert(!canRememberEcmPid(p)); // CW alone is not evidence
    p.clearVideoPesSincePidSelection = 40;
    p.pidSelectedSinceMs = 99000;
    assert(!canRememberEcmPid(p)); // recently changed PID
    p.pidSelectedSinceMs = 80000;
    p.forcedPid = 7169;
    assert(!canRememberEcmPid(p)); // not the forced PID
    p.forcedPid = 7254;
    assert(canRememberEcmPid(p)); // manual working PID can be learned
    p.selectedPid = 0;
    assert(!canRememberEcmPid(p));
    p.selectedPid = 7254;

    assert(!shouldAbandonCachedEcmPid(p, false)); // manual user -X
    p.cardDenials = 4;
    p.lastEncryptedInputMs = 99000;
    p.lastClearMediaPesMs = 0;
    assert(!shouldAbandonCachedEcmPid(p, true)); // uptime under 45s
    p.childStartedMs = 50000;
    assert(shouldAbandonCachedEcmPid(p, true));
    p.lastClearMediaPesMs = 97000;
    assert(!shouldAbandonCachedEcmPid(p, true)); // recently clear
    p.lastClearMediaPesMs = 60000;
    assert(shouldAbandonCachedEcmPid(p, true)); // now stalled
    p.lastEncryptedInputMs = 90000;
    assert(!shouldAbandonCachedEcmPid(p, true)); // no encrypted input
    p.lastEncryptedInputMs = 99000;
    p.cardDenials = 0;
    assert(!shouldAbandonCachedEcmPid(p, true));
    p.childStartedMs = 20000;
    assert(shouldAbandonCachedEcmPid(p, true)); // max grace exceeded
    std::puts("PASS TSDecrypt ECM PID learning and hint fallback");
}
