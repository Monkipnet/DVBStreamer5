#include "ca/backends/tsdecrypt/TsDecryptTelemetry.h"
#include <cassert>

using dvbstreamer5::ca::tsdecrypt_detail::parseCwAppliedReport;

int main() {
    const auto normal = parseCwAppliedReport(
        "2026-11-21 | DVB5 | CW_APPLIED mode=2 even=1 odd=0");
    assert(normal.valid && normal.mode == 2 && normal.even && !normal.odd);
    const auto changed = parseCwAppliedReport(
        "DVB5 | CW_APPLIED mode=0 even=0 odd=1");
    assert(changed.valid && changed.mode == 0 && !changed.even && changed.odd);
    const auto both = parseCwAppliedReport(
        "DVB5 | CW_APPLIED mode=255 even=1 odd=1");
    assert(both.valid && both.even && both.odd);
    assert(!parseCwAppliedReport(nullptr).valid);
    assert(!parseCwAppliedReport("CW  | SID 129 Data: not-a-marker").valid);
    assert(!parseCwAppliedReport("DVB5 | CW_APPLIED mode=256 even=1 odd=1").valid);
    assert(!parseCwAppliedReport("DVB5 | CW_APPLIED mode=1 even=0 odd=0").valid);
    assert(!parseCwAppliedReport("DVB5 | CW_APPLIED mode=1 even=2 odd=0").valid);
    assert(!parseCwAppliedReport("DVB5 | CW_APPLIED mode=1 even=1 odd=0 suffix").valid);
    assert(!parseCwAppliedReport("DVB5 | CW_APPLIED mode=1 even=-1 odd=0").valid);
    return 0;
}
