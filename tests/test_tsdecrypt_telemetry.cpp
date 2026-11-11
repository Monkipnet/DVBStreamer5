// V10.8.460: isolated TSDecrypt metrics regression (no CAMD or DVB required).
#include "../src/ca/backends/tsdecrypt/TsDecryptTelemetry.h"
#include <array>
#include <cstdlib>
#include <cstdio>

using namespace dvbstreamer5::ca::tsdecrypt_detail;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "TSDecrypt telemetry test failed: %s\n", what);
        std::exit(1);
    }
}

int main() {
    auto raw = std::array<uint8_t, 188>{};
    raw.fill(0xff);
    raw[0] = 0x47;
    raw[1] = 0x40 | 0x02; // PID 0x0200, payload unit start
    raw[2] = 0;
    raw[3] = 0x10; // clear, payload
    raw[4] = 0; raw[5] = 0; raw[6] = 1; raw[7] = 0xe0; // video PES
    auto x = inspectTsPacket(raw.data(), raw.size());
    check(x.valid && x.pid == 0x0200 && !x.scrambled && x.clearMediaPesStart && x.clearVideoPesStart && !x.clearAudioPesStart, "video PES");

    raw[3] = 0x90; // scrambled
    x = inspectTsPacket(raw.data(), raw.size());
    check(x.valid && x.scrambled && !x.clearMediaPesStart, "x.valid && x.scrambled && !x.clearMediaPesStart");

    raw[3] = 0x30; // clear adaptation + payload
    raw[4] = 2;
    raw[5] = raw[6] = 0;
    raw[7] = 0; raw[8] = 0; raw[9] = 1; raw[10] = 0xc0;
    x = inspectTsPacket(raw.data(), raw.size());
    check(x.clearMediaPesStart && x.clearAudioPesStart && !x.clearVideoPesStart, "audio PES");

    raw[4] = 183; // no PES bytes after adaptation
    check(!inspectTsPacket(raw.data(), raw.size()).clearMediaPesStart, "!inspectTsPacket(raw.data(), raw.size()).clearMediaPesStart");
    raw[0] = 0x00;
    check(!inspectTsPacket(raw.data(), raw.size()).valid, "!inspectTsPacket(raw.data(), raw.size()).valid");

    const auto r = parseEcmReport(
        "2026-11-10 17:22:01 | ECM | Received 52 (4 dup) and processed 48 in 10 seconds.");
    check(r.valid && r.received == 52 && r.duplicates == 4 && r.processed == 48, "r.valid && r.received == 52 && r.duplicates == 4 && r.processed == 48");
    check(!parseEcmReport("CW | SID 0x69 Data: SECRET").valid, "!parseEcmReport(\"CW | SID 0x69 Data: SECRET\").valid");
    // Upstream counts repeated ECM sections independently; duplicates can
    // exceed unique ECMs. Async CAMD can also finish an earlier request in
    // the next report interval, so processed may exceed received.
    const auto repeated = parseEcmReport(
        "2026-11-10 17:22:11 | ECM | Received 1 (41 dup) and processed 3 in 10 seconds.");
    check(repeated.valid && repeated.received == 1 &&
          repeated.duplicates == 41 && repeated.processed == 3,
          "duplicate/processed ECM counters may exceed received");
    check(!parseEcmReport("ECM | Received 1 (4 dup) and processed 3 in 0 seconds.").valid,
          "zero-length interval should be rejected");
    check(hasSafeLogEvent("2026 | CW  | OK: Valid code word was received.",
                           "CW  | OK: Valid code word was received."), "CW safe event");
    check(classifyCamdEvent("CAM | Connecting to server private-user port 12121") ==
              CamdEvent::Connecting, "CAMD connect attempt (no secrets retained)");
    check(classifyCamdEvent("CAM | Connected to server 127.0.0.1 port 12121") ==
              CamdEvent::Connected, "CAMD connected");
    check(classifyCamdEvent("CAM | Error connecting to server x port y") ==
              CamdEvent::ConnectionError, "CAMD connect failure");
    check(classifyCamdEvent("ERR | Error sending ecm packet, reconnecting to camd.") ==
              CamdEvent::ReconnectRequested, "CAMD ECM reconnect");
    check(classifyCamdEvent("ERR | No code word has been received (ret = -1)") ==
              CamdEvent::NoCodeWord, "CAMD no CW");
    check(classifyCamdEvent("WRN | Too many items (12) in ECM queue, dropping the oldest.") ==
              CamdEvent::EcmQueueOverflow, "CAMD ECM backlog");
    check(classifyCamdEvent("ERR | [newcamd] Failed to read message.") ==
              CamdEvent::NewcamdReadError, "Newcamd read error");
    check(classifyCamdEvent("ERR | [newcamd] Bad msg_id 000A != 0009") ==
              CamdEvent::NewcamdBadMsgId, "Newcamd delayed reply");
    check(classifyCamdEvent("ERR | [newcamd] Card was not able to decode the channel.") ==
              CamdEvent::NewcamdCardDenied, "Newcamd card denied");
    check(classifyCamdEvent("ERR | [newcamd] Checksum error.") ==
              CamdEvent::NewcamdChecksumError, "Newcamd checksum");
    check(classifyCamdEvent("ERR | [newcamd] Unexpected CAMD server response (code 0).") ==
              CamdEvent::NewcamdUnexpectedResponse, "Newcamd unexpected reply");
    check(classifyCamdEvent("ERR | [newcamd] Login failed. Check user/pass/des-key.") ==
              CamdEvent::NewcamdLoginError, "Newcamd login");
    check(classifyCamdEvent("ERR | [newcamd] protocol unknown") ==
              CamdEvent::ProtocolError, "Newcamd fallback counter");
    check(classifyCamdEvent("CW  | SID 0x01 Data: SENSITIVE_CW") ==
              CamdEvent::None, "raw CW not classified");
    const auto pidSelected = parseEcmPidReport("--- | ECM pid : 0x0D67 (3431)");
    check(pidSelected.valid && !pidSelected.switched && pidSelected.pid == 0x0D67,
          "Selected ECM PID should be parsed without payload bytes");
    const auto pidSwitch = parseEcmPidReport(
          "ECM | Switching ECM pid to 0x0123 (291) idx:1");
    check(pidSwitch.valid && pidSwitch.switched && pidSwitch.pid == 0x0123,
          "Switching ECM PID should be parsed");
    check(!parseEcmPidReport("--- | ECM pid : 0x0D67 (3431) idx:0").valid,
          "Alternative ECM PID list must not overwrite current selection");
    check(!parseEcmPidReport("--- | ECM pid : 0x1FFF").valid,
          "Null PID must never be accepted as ECM PID");
    check(!parseEcmPidReport("--- | ECM pid : 0x01234").valid,
          "Reject oversized ECM PID hexadecimal");
    check(!parseEcmPidReport("CW | SID 0x0105 0x0123 Secret").valid,
          "Never interpret code-word logs as ECM PID");
    check(isEcmPidHoldEvent(
        "ECM | Holding recently valid ECM pid after transient card denial."),
        "PID hold event is safe to count");
    check(!isEcmPidHoldEvent("CW | Secret content"),
        "PID hold filter cannot inspect CW payload");
    std::puts("TSDecrypt safe telemetry tests passed");
}
