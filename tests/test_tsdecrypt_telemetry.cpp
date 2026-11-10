// V10.8.460: isolated TSDecrypt metrics regression (no CAMD or DVB required).
#include "../src/ca/backends/tsdecrypt/TsDecryptTelemetry.h"
#include <array>
#include <cassert>
#include <cstdio>

using namespace dvbstreamer5::ca::tsdecrypt_detail;

int main() {
    auto raw = std::array<uint8_t, 188>{};
    raw.fill(0xff);
    raw[0] = 0x47;
    raw[1] = 0x40 | 0x02; // PID 0x0200, payload unit start
    raw[2] = 0;
    raw[3] = 0x10; // clear, payload
    raw[4] = 0; raw[5] = 0; raw[6] = 1; raw[7] = 0xe0; // video PES
    auto x = inspectTsPacket(raw.data(), raw.size());
    assert(x.valid && x.pid == 0x0200 && !x.scrambled && x.clearMediaPesStart);

    raw[3] = 0x90; // scrambled
    x = inspectTsPacket(raw.data(), raw.size());
    assert(x.valid && x.scrambled && !x.clearMediaPesStart);

    raw[3] = 0x30; // clear adaptation + payload
    raw[4] = 2;
    raw[5] = raw[6] = 0;
    raw[7] = 0; raw[8] = 0; raw[9] = 1; raw[10] = 0xc0;
    x = inspectTsPacket(raw.data(), raw.size());
    assert(x.clearMediaPesStart);

    raw[4] = 183; // no PES bytes after adaptation
    assert(!inspectTsPacket(raw.data(), raw.size()).clearMediaPesStart);
    raw[0] = 0x00;
    assert(!inspectTsPacket(raw.data(), raw.size()).valid);

    const auto r = parseEcmReport(
        "2026-11-10 17:22:01 | ECM | Received 52 (4 dup) and processed 48 in 10 seconds.");
    assert(r.valid && r.received == 52 && r.duplicates == 4 && r.processed == 48);
    assert(!parseEcmReport("CW | SID 0x69 Data: SECRET").valid);
    assert(!parseEcmReport("ECM | Received 1 (4 dup) and processed 3 in 10 seconds.").valid);
    assert(hasSafeLogEvent("2026 | CW  | OK: Valid code word was received.",
                           "CW  | OK: Valid code word was received."));
    std::puts("TSDecrypt safe telemetry tests passed");
}
