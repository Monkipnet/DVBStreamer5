#include "../src/ca/backends/tsdecrypt/TsDecryptRecoveryPolicy.h"
#include <iostream>
using namespace dvbstreamer5::ca::tsdecrypt_detail;

static bool check(bool test, const char* label) {
    if (!test) std::cerr << "FAIL: " << label << '\n';
    return test;
}

int main() {
    bool ok = true;
    RecoveryProbe p{};
    ok &= check(recoveryReason(p) == RecoveryReason::None, "idle startup");
    p.childExited = true;
    ok &= check(recoveryReason(p) == RecoveryReason::ChildExited, "crashed child");
    p.childExited = false;
    p.nowMs = 40000; p.startedMs = 1000;
    p.lastEncryptedInputMs = 39000; p.lastChildOutputMs = 39000;
    ok &= check(recoveryReason(p) == RecoveryReason::None, "active stream");
    p.nowMs = 80000; p.lastEncryptedInputMs = 79000; p.lastChildOutputMs = 50000;
    ok &= check(recoveryReason(p) == RecoveryReason::OutputStalled, "hung stdout");
    p.nowMs = 85000; p.lastEncryptedInputMs = 84500; p.lastChildOutputMs = 84000;
    ok &= check(recoveryReason(p) == RecoveryReason::MediaStalled, "never valid PES");
    p.lastClearMediaPesMs = 80000;
    ok &= check(recoveryReason(p) == RecoveryReason::None, "fresh PES");
    p.nowMs = 127000; p.lastEncryptedInputMs = 126000; p.lastChildOutputMs = 126000;
    ok &= check(recoveryReason(p) == RecoveryReason::MediaStalled, "PES lost");
    p.lastEncryptedInputMs = 110000;
    ok &= check(recoveryReason(p) == RecoveryReason::None, "idle input no restart");
    p.childExited = true;
    ok &= check(recoveryReason(p) == RecoveryReason::ChildExited, "exit always wins");
    ok &= check(retryDelayMs(0) == 1000 && retryDelayMs(1) == 2000 &&
        retryDelayMs(5) == 30000 && retryDelayMs(999) == 30000, "bounded retry");
    p.childExited = false;
    p.nowMs = 85000; p.startedMs = 1000; p.lastEncryptedInputMs = 84500;
    p.lastChildOutputMs = 84000; p.lastClearMediaPesMs = 0;
    p.lastCamdErrorMs = 82000;
    ok &= check(recoveryReason(p) == RecoveryReason::None,
                "CAMD is already reconnecting; avoid host restart storm");
    p.lastCamdErrorMs = 0;
    ok &= check(recoveryReason(p) == RecoveryReason::MediaStalled,
                "Media stalled without CAMD errors");
    p.nowMs = 210000; p.lastEncryptedInputMs = 209000;
    p.lastChildOutputMs = 209000; p.lastCamdErrorMs = 209000;
    ok &= check(recoveryReason(p) == RecoveryReason::MediaStalled,
                "Prolonged outage eventually escalates");
    ok &= check(retryJitterMs("stream-105") == retryJitterMs("stream-105") &&
                retryJitterMs("stream-105") <= 1800,
                "Per-stream retry jitter is deterministic and bounded");
    if (ok) std::cout << "PASS TSDecrypt recovery policy\n";
    return ok ? 0 : 1;
}
