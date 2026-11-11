#pragma once
#include <cstdint>

namespace dvbstreamer5::ca::tsdecrypt_detail {

// Xaccel release notes separately highlight waiting for a valid CW and
// loss of decoded frames. This independent policy does NOT infer successful
// decoding from a Newcamd response: only a recent clear PES is evidence.
enum class CwAcquisitionState : uint8_t {
    Starting = 0, ClearPesRecent, ClearPesStalled,
    ReaderRejected, CwSignaledNoPes, AwaitingCw
};
struct CwAcquisitionProbe {
    uint64_t nowMs = 0;
    uint64_t startedMs = 0;
    uint64_t lastClearPesMs = 0;
    uint64_t lastCamdErrorMs = 0;
    bool validCwSignal = false;
};
inline CwAcquisitionState cwAcquisitionState(const CwAcquisitionProbe& p) noexcept {
    if (!p.startedMs || p.nowMs < p.startedMs) return CwAcquisitionState::Starting;
    if (p.lastClearPesMs && p.lastClearPesMs <= p.nowMs) {
        return p.nowMs - p.lastClearPesMs <= 10000
            ? CwAcquisitionState::ClearPesRecent
            : CwAcquisitionState::ClearPesStalled;
    }
    // A missed first ECM cycle should not immediately mark the decoder as
    // failed. The grace window is a display/telemetry classification only;
    // it never extends the lifetime of any CW or suppresses the watchdog.
    if (p.nowMs - p.startedMs <= 45000) return CwAcquisitionState::Starting;
    if (p.lastCamdErrorMs && p.lastCamdErrorMs <= p.nowMs &&
        p.nowMs - p.lastCamdErrorMs <= 60000)
        return CwAcquisitionState::ReaderRejected;
    return p.validCwSignal
        ? CwAcquisitionState::CwSignaledNoPes
        : CwAcquisitionState::AwaitingCw;
}
inline const char* cwAcquisitionStateName(CwAcquisitionState state) noexcept {
    switch (state) {
        case CwAcquisitionState::ClearPesRecent: return "clear_pes_recent";
        case CwAcquisitionState::ClearPesStalled: return "clear_pes_stalled";
        case CwAcquisitionState::ReaderRejected: return "reader_rejected";
        case CwAcquisitionState::CwSignaledNoPes: return "cw_signaled_no_pes";
        case CwAcquisitionState::AwaitingCw: return "awaiting_cw";
        default: return "starting";
    }
}
} // namespace dvbstreamer5::ca::tsdecrypt_detail
