#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace dvbstreamer5::ca::tsdecrypt_detail {

enum class RecoveryReason : uint8_t {
    None = 0,
    ChildExited,
    OutputStalled,
    MediaStalled,
    CamdBlackout,
};

struct RecoveryProbe {
    uint64_t nowMs = 0;
    uint64_t startedMs = 0;
    uint64_t lastEncryptedInputMs = 0;
    uint64_t lastChildOutputMs = 0;
    uint64_t lastClearMediaPesMs = 0;
    bool childExited = false;
    // Recent upstream CAMD reconnect: allow its own connection manager to
    // recover before killing and respawning all TSDecrypt processes.
    uint64_t lastCamdErrorMs = 0;
};

// Cumulative timestamps are monotonic. No ECM/CW data or wall time required.
inline RecoveryReason recoveryReason(const RecoveryProbe& p) noexcept {
    if (p.childExited) return RecoveryReason::ChildExited;
    if (p.startedMs == 0 || p.nowMs < p.startedMs || p.nowMs - p.startedMs < 30000)
        return RecoveryReason::None;
    if (p.lastEncryptedInputMs == 0 || p.lastEncryptedInputMs > p.nowMs ||
        p.nowMs - p.lastEncryptedInputMs > 5000)
        return RecoveryReason::None;
    if (p.lastChildOutputMs == 0 || p.lastChildOutputMs > p.nowMs ||
        p.nowMs - p.lastChildOutputMs > 20000)
        return RecoveryReason::OutputStalled;
    const bool recentCamdError =
        p.lastCamdErrorMs != 0 && p.lastCamdErrorMs <= p.nowMs &&
        p.nowMs - p.lastCamdErrorMs <= 60000;
    // When the upstream CAMD reader is still reporting protocol/ECM errors,
    // killing our process cannot repair reader capacity or entitlements.
    // Avoid repeatedly creating fresh Newcamd sessions for nine streams.
    // Most CAMD errors are upstream; do not churn Newcamd sessions on
    // short outages. However, an alive child can emit encrypted output
    // forever without a single clear PES. After three minutes of continuous
    // no-clear-media evidence, permit a bounded *single-child* reconnect.
    // A new child must itself survive three minutes before another retry.
    if (recentCamdError) {
        const uint64_t lastEvidence = p.lastClearMediaPesMs ?
            p.lastClearMediaPesMs : p.startedMs;
        // V558: after proven clear media, a 90-second sustained blackout
        // is enough to re-probe this ONE child. Never-proven/cold channels
        // keep the conservative 180-second grace. The source must still
        // supply scrambled media and the child must still produce TS.
        const bool hadClearMedia =
            p.lastClearMediaPesMs >= p.startedMs &&
            p.lastClearMediaPesMs != 0;
        const uint64_t graceMs = hadClearMedia ? 90000ULL : 180000ULL;
        if (p.nowMs - p.startedMs >= graceMs &&
            lastEvidence <= p.nowMs &&
            p.nowMs - lastEvidence >= graceMs)
            return RecoveryReason::CamdBlackout;
        return RecoveryReason::None;
    }
    if (p.lastClearMediaPesMs != 0) {
        if (p.lastClearMediaPesMs <= p.nowMs &&
            p.nowMs - p.lastClearMediaPesMs > 45000)
            return RecoveryReason::MediaStalled;
    } else if (p.nowMs - p.startedMs > 70000) {
        return RecoveryReason::MediaStalled;
    }
    return RecoveryReason::None;
}

inline uint64_t retryDelayMs(uint64_t previousRestarts) noexcept {
    const uint64_t power = previousRestarts > 5 ? 5 : previousRestarts;
    const uint64_t delay = 1000ULL << power;
    return delay > 30000 ? 30000 : delay;
}

// Stable per-stream staggering avoids a thundering herd when many
// simultaneously started services hit the same temporary CAMD outage.
inline uint64_t retryJitterMs(const char* streamId) noexcept {
    uint32_t hash = 2166136261u;
    if (streamId) {
        for (const unsigned char* p =
                 reinterpret_cast<const unsigned char*>(streamId); *p; ++p) {
            hash ^= *p;
            hash *= 16777619u;
        }
    }
    return (hash % 13u) * 150ULL; // 0 ... 1800 milliseconds
}

// A prolonged CAMD blackout can affect many services sharing one remote
// reader. Stagger their occasional reconnects over 0..27 seconds instead of
// initiating simultaneous logins after the same three-minute timeout.
inline uint64_t camdBlackoutJitterMs(const char* streamId) noexcept {
    return retryJitterMs(streamId) * 15ULL;
}

// V558: a shared OSCam reader may be serving multiple TSDecrypt children.
// During a reader-wide failure, let at most one child per configured reader
 // initiate a CAMD blackout reconnect in each 30-second window, without
 // blocking unrelated readers. This is in-process metadata only; neither
 // ECM, CW nor credentials are retained.
class CamdReaderRestartGate {
public:
    bool tryAcquire(const std::string& readerKey, uint64_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t& nextAllowed = nextAllowedMs_[readerKey];
        if (nowMs < nextAllowed) return false;
        nextAllowed = nowMs + 30000ULL;
        return true;
    }
private:
    std::mutex mutex_;
    std::map<std::string, uint64_t> nextAllowedMs_;
};

inline const char* recoveryReasonName(RecoveryReason reason) noexcept {
    switch (reason) {
        case RecoveryReason::ChildExited: return "child_exited";
        case RecoveryReason::OutputStalled: return "output_stalled";
        case RecoveryReason::MediaStalled: return "media_stalled";
        case RecoveryReason::CamdBlackout: return "camd_blackout";
        default: return "none";
    }
}

} // namespace dvbstreamer5::ca::tsdecrypt_detail
