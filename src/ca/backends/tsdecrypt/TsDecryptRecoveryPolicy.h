#pragma once
#include <cstdint>

namespace dvbstreamer5::ca::tsdecrypt_detail {

enum class RecoveryReason : uint8_t {
    None = 0,
    ChildExited,
    OutputStalled,
    MediaStalled,
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
    const uint64_t mediaReferenceMs = p.lastClearMediaPesMs
        ? p.lastClearMediaPesMs : p.startedMs;
    const bool prolongedMediaLoss = p.nowMs >= mediaReferenceMs &&
        p.nowMs - mediaReferenceMs >= 180000;
    // If the CAMD server is already reconnecting, do not generate a second
    // simultaneous login storm. A prolonged failure still triggers recovery.
    if (recentCamdError && !prolongedMediaLoss)
        return RecoveryReason::None;
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

inline const char* recoveryReasonName(RecoveryReason reason) noexcept {
    switch (reason) {
        case RecoveryReason::ChildExited: return "child_exited";
        case RecoveryReason::OutputStalled: return "output_stalled";
        case RecoveryReason::MediaStalled: return "media_stalled";
        default: return "none";
    }
}

} // namespace dvbstreamer5::ca::tsdecrypt_detail
