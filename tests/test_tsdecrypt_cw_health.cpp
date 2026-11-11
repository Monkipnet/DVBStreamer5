#include "../src/ca/backends/tsdecrypt/TsDecryptCwHealthPolicy.h"
#include <iostream>
#include <string>

using namespace dvbstreamer5::ca::tsdecrypt_detail;
int main() {
    bool ok = true;
    auto check = [&](bool b, const char* name) {
        if (!b) { std::cerr << "FAIL: " << name << "\n"; ok = false; }
    };
    CwAcquisitionProbe p{};
    check(cwAcquisitionState(p) == CwAcquisitionState::Starting,
          "no child started");
    p.startedMs = 1000;
    p.nowMs = 40000;
    p.lastCamdErrorMs = 38000;
    check(cwAcquisitionState(p) == CwAcquisitionState::Starting,
          "grace window does not flag reader failed");
    p.nowMs = 47000;
    check(cwAcquisitionState(p) == CwAcquisitionState::ReaderRejected,
          "reader rejection after grace");
    p.lastCamdErrorMs = 0;
    p.validCwSignal = true;
    check(cwAcquisitionState(p) == CwAcquisitionState::CwSignaledNoPes,
          "a valid CW report is not decoded PES evidence");
    p.validCwSignal = false;
    check(cwAcquisitionState(p) == CwAcquisitionState::AwaitingCw,
          "no CW and no PES");
    p.lastClearPesMs = 46000;
    check(cwAcquisitionState(p) == CwAcquisitionState::ClearPesRecent,
          "fresh PES preferred over CW flags");
    p.nowMs = 60001;
    check(cwAcquisitionState(p) == CwAcquisitionState::ClearPesStalled,
          "no recent PES means stale decoding");
    p.lastClearPesMs = 60000;
    check(cwAcquisitionState(p) == CwAcquisitionState::ClearPesRecent,
          "PES resumed");
    check(std::string(cwAcquisitionStateName(cwAcquisitionState(p))) ==
          "clear_pes_recent", "stable machine-readable name");
    if (ok) std::cout << "PASS TSDecrypt CW acquisition health policy\n";
    return ok ? 0 : 1;
}
