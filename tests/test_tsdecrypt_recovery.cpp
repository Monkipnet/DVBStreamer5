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
    ok &= check(recoveryReason(p) == RecoveryReason::CamdBlackout,
                "Prolonged CAMD blackout must eventually recover one child");
    // Before 180 seconds of no-clear-PES and uptime, do not reconnect
    // multiple services to the same overloaded reader.
    p.nowMs = 170000; p.lastEncryptedInputMs = 169000;
    p.lastChildOutputMs = 169000; p.lastCamdErrorMs = 169000;
    ok &= check(recoveryReason(p) == RecoveryReason::None,
                "Recent errors with a short blackout must stay connected");
    // A recent valid PES is stronger evidence than a CAMD error.
    p.nowMs = 210000; p.lastEncryptedInputMs = 209000;
    p.lastChildOutputMs = 209000; p.lastCamdErrorMs = 209000;
    p.lastClearMediaPesMs = 205000;
    ok &= check(recoveryReason(p) == RecoveryReason::None,
                "Recent clear PES must prevent blackout reset");
    // V558: a previously healthy stream should not remain frozen for
    // three minutes when CAMD errors continue and clear PES has stopped.
    p.startedMs = 1000;
    p.lastClearMediaPesMs = 100000;
    p.lastCamdErrorMs = 188000;
    p.lastEncryptedInputMs = 188500;
    p.lastChildOutputMs = 188500;
    p.nowMs = 189000;
    ok &= check(recoveryReason(p) == RecoveryReason::None,
                "Recent clear PES: do not restart before 90 seconds");
    p.lastEncryptedInputMs = 190000; p.lastChildOutputMs = 190000;
    p.lastCamdErrorMs = 190000; p.nowMs = 190000;
    ok &= check(recoveryReason(p) == RecoveryReason::CamdBlackout,
                "After 90 seconds of proven-media blackout restart only child");
    p.lastClearMediaPesMs = 189500;
    ok &= check(recoveryReason(p) == RecoveryReason::None,
                "Any renewed clear PES cancels early blackout recovery");
    p.lastClearMediaPesMs = 0;
    // Simulate a new channel that has never produced clear PES.
    p.nowMs = 170000; p.lastEncryptedInputMs = 169000;
    p.lastChildOutputMs = 169000; p.lastCamdErrorMs = 169000;
    ok &= check(recoveryReason(p) == RecoveryReason::None,
                "Cold service must retain 180-second start grace");
    // Immediately after 180 seconds a cold CAMD blackout may recover.
    p.nowMs = 182000; p.lastEncryptedInputMs = 181000;
    p.lastChildOutputMs = 181000; p.lastCamdErrorMs = 181000;
    ok &= check(recoveryReason(p) == RecoveryReason::CamdBlackout,
                "Cold CAMD blackout must eventually recover after 180 seconds");
    p.lastCamdErrorMs = 0;
    ok &= check(recoveryReason(p) == RecoveryReason::MediaStalled,
                "When CAMD errors cease, normal media recovery resumes");
    p.childExited = true;
    p.lastCamdErrorMs = 209000;
    ok &= check(recoveryReason(p) == RecoveryReason::ChildExited,
                "Child exit must always override CAMD blackout");
    p.childExited = false;
    p.lastEncryptedInputMs = 190000;
    ok &= check(recoveryReason(p) == RecoveryReason::None,
                "No incoming encrypted data: do not force CAMD recovery");
    p.lastEncryptedInputMs = 209000;
    ok &= check(camdBlackoutJitterMs("stream-105") ==
                    camdBlackoutJitterMs("stream-105") &&
                camdBlackoutJitterMs("stream-105") <= 27000,
                "Blackout reconnect jitter must be bounded and deterministic");
    ok &= check(recoveryReasonName(RecoveryReason::CamdBlackout) ==
                    std::string("camd_blackout"),
                "Blackout diagnostics should be key-free");
    // Synthetic five-channel incident: stagger reader reconnects instead
    // of immediately restarting all TSDecrypt child processes at once.
    const char* ids[] = {
        "sat-1794213237299-113-4", "sat-1794213237299-122-3",
        "sat-1794213237299-132-2", "sat-1794213401098-108-0",
        "sat-1794213530603-118-3"
    };
    uint64_t minJitter = 28000, maxJitter = 0;
    for (const char* id : ids) {
        const uint64_t jitter = camdBlackoutJitterMs(id);
        if (jitter < minJitter) minJitter = jitter;
        if (jitter > maxJitter) maxJitter = jitter;
        ok &= check(jitter <= 27000, "Per-SID CAMD jitter exceeded limit");
    }
    ok &= check(maxJitter > minJitter,
                "Five shared-reader services must not all reconnect together");
    ok &= check(retryJitterMs("stream-105") == retryJitterMs("stream-105") &&
                retryJitterMs("stream-105") <= 1800,
                "Per-stream retry jitter is deterministic and bounded");

    CamdReaderRestartGate gate;
    ok &= check(gate.tryAcquire("icam", 100000),
                "First blackout on reader must receive ticket");
    ok &= check(!gate.tryAcquire("icam", 100000) &&
                !gate.tryAcquire("icam", 129999),
                "Same-reader services must share 30s reconnect budget");
    ok &= check(gate.tryAcquire("other-reader", 100000),
                "Unrelated reader must not be blocked");
    ok &= check(gate.tryAcquire("icam", 130000),
                "Reader must recover a fresh slot after cooldown");
    if (ok) std::cout << "PASS TSDecrypt recovery policy\n";
    return ok ? 0 : 1;
}
