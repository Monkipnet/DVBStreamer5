# DVBStreamer5 — BISS-2 Mode 1/E (V10.8.482)

This opt-in plugin decrypts an **authorized** DVB-CISSA transport stream
using EBU Tech 3292 v3 and ETSI TS 103 127 v1.1.1.

Supported:
- **BISS2 Mode 1 (BISS2-1)**: 128-bit Clear Session Word / SW (32 HEX).
- **BISS2 Mode E (BISS2-E)**: 128-bit encrypted session word / ESW (32 HEX)
  and 128-bit injected receiver ID (32 HEX). The SW is recovered with
  AES-128-ECB under the receiver ID.
- MPEG-TS packet descrambling: AES-128-CBC with DVB-CISSA's normative
  fixed IV `445642544D4350544145534349535341`. Each TS packet is
  decrypted independently. Headers/adaptation fields and the trailing
  1–15 payload bytes remain clear. Even/odd TS flags are handled.

**Not supported:** BISS-CA (Mode CA, CA_system_id 0x2610), in-stream
RSA-2048 receiver entitlements, dynamic ECM/EMM decryption and key
rotation. This module does not auto-extract keys, scan keys or provide
session words. It does NOT implement BISS1 Mode E; legacy BISS-1 Mode 1
remains in `dvbstreamer5-ca-biss1.so`.

## Web UI configuration

1. Menu → CAM clients → Add client.
2. Choose backend **BISS-2 (AES-128, Modes 1/E)**.
3. For Mode 1, enter the authorized 32-hex SW.
4. For Mode E, enter the 32-hex ESW and 32-hex injected ID supplied by
   the sender / entitlement administrator. Never share these credentials
   in logs, issue reports or screenshots.
5. Save, then select this client for one DVB tile under
   **Access mode → iCAM/CA → CA engine BISS-2**.
6. Confirm that decoded PES/AV is valid using a player; a changed
   scrambling-control flag or "ACTIVE" status alone is not proof of
   correct key material.

BISS-2 uses CA_system_id 0x2602 for Modes 1/E, and CA_PID 0x1FFF in
the PMT. There are no ECM or EMM packets for these two modes.
The backend does not modify PMT/PAT/CAT; it consumes the selected
stream and descrambles its TS payload in place.
It is safe to use multiple tiles with distinct BISS2 keys.

### Key handling and security

The web form uses password-type fields and retains existing values
when a field is left blank. The native backend does not print CW/SW,
ESW, injected ID or clear key bytes in its status API. The decoded
session word is wiped when the reader is destroyed; temporary
cryptographic buffers and injected IDs are wiped after use.

**Configuration storage:** like existing CAM-client credentials,
the BISS2 backend_config is stored as part of DVBStreamer5's local
configuration. Protect the configuration file and API; do not treat
the password input type as encryption at rest. Injected ID readback
from the underlying configuration API must be restricted to trusted
administrators; hardened write-only credential storage is outside the
scope of this version.

### Build and regression tests — from source root

```bash
git switch main
git pull --ff-only origin main
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build -R 'biss(1|2)_.*|tsdecrypt_pid_.*' --output-on-failure
node tests/web/test_biss2_ui.js
```

CMake target `DVBStreamer5` depends on the BISS1 and BISS2
plugins, and the portable packager automatically copies
`build/ca-plugins/dvbstreamer5-ca-biss2.so` alongside the other
CA backends.

The regression test validates BISS2-E against the official EBU Tech
3292 Annex A AES-128 vector, CISSA against the ETSI TS 103 127 Annex B
test ciphertext, plus even/odd TS packet round-trips and adaptation
field / short-payload handling.

**Test status:** synthetic unit tests are registered in CTest. Actual
satellite signal interoperability and BISS-CA were not validated here.

### Related standards

- EBU Tech 3292 v3 (2018): https://tech.ebu.ch/docs/tech/tech3292.pdf
- EBU Tech 3292s1 (BISS-CA): https://tech.ebu.ch/publications/tech3292s1
- ETSI TS 103 127: https://www.etsi.org/deliver/etsi_TS/103100_103199/103127/01.01.01_60/ts_103127v010101p.pdf
