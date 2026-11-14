# DVBStreamer5 V10.8.517 — WISI Chameleon buffered Source-PCR fix

V516 introduced a per-tile opt-in `wisi_chameleon_profile` switch for
transcoded UDP-CBR channels. It is **off by default**. V517 corrects the
case where the selected channel already has a non-default CBR startup
buffer (stored as a private low-3-digit marker).

## Problem fixed

A transcoded 8-Mbit/s channel with `target_bitrate=8000705` (3s
startup) previously set `wisiPreserveSourcePcr=true` but could fail to
convert its carrier to the source-PCR marker. Consequently the V515
physical 20-ms PCR-only smoothing was inactive even though the WISI
encoder profile was active.

V517 sets `wisiSourcePcrStrict=true` for the explicitly selected WISI
channel and uses the existing
`sourcePcrCbrTargetKeepingStartupBuffer(...)` helper.

Examples:

| Saved CBR carrier | Selected Source-PCR carrier | Startup |
|---|---|---|
| `8000000` | `8000777` | 3000 ms default |
| `8000705` | `8000805` | 3000 ms |
| `8000709` | `8000809` | 5000 ms |
| `8000699` | `8000799` | explicitly disabled |

The physical wire target remains 8.000 Mbit/s. Zero startup disables
20-ms smoothing, by design: do not override a disabled buffer without
operator action. The encoder itself can still use its selected WISI
policy in that case; check `pcr_smoothing_test=0` in logs.

## What V516/V517 configure

- Direct VAAPI H.264: 400-ms experimental RateControl/HRD driver request.
- Intel oneVPL QSV and Intel zero-copy: 400-ms CPB, half-full initial.
- NVIDIA NVENC and NVDEC/NVENC zero-copy: retain default two-frame VBV;
  cap an existing higher quality A/B VBV to 400 ms; keep zero reorder.
- CPU OpenH264 H.264 / Kvazaar HEVC: preserve known-working native
  bitrate control and report maximum encoded AU / non-random AU every
  five seconds; do not claim a hard frame-size guarantee.
- Physical UDP-CBR after mux: 1316-byte datagrams, 20-ms reference PCR
  smoothing where buffered Source-PCR applies, original PTS/DTS/PES/CC
  retained. Copy/VBR/other stream IDs are unchanged.

None of these controls alone proves full T-STD, HRD or WISI decoder
conformance. Software tests do not exercise vendor GPU drivers.

## Test on svettranscoding only

```bash
cd /home/monk/DVBStreamer5-main-clean
git switch main
git pull --ff-only origin main
grep kProgramVersion src/AppVersion.h
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_ENABLE_VAAPI=ON \
  -DDVBSTREAMER5_ENABLE_INTEL_QSV=ON \
  -DDVBSTREAMER5_ENABLE_NVENC=ON \
  -DDVBSTREAMER5_BUILD_MEDIA_CORE_TESTS=ON
cmake --build build --parallel 2
ctest --test-dir build -R \
  'wisi_profile_policy|wisi_profile_config|vaapi_hrd_policy|cbr_output_pcr_smoothing|source_pcr_packet_pacing|transcode_cbr_single_shaper' \
  --output-on-failure
node tests/web/test_wisi_chameleon_profile_ui.js
node tests/test_transcoder_editor_ui.js
ctest --test-dir build --output-on-failure
bash scripts/make_portable_linux_bundle.sh \
  --build-dir build \
  --output-dir dist/DVBStreamer5-10.8.517-linux-x86_64 --archive
sha256sum dist/DVBStreamer5-10.8.517-linux-x86_64.tar.gz
```

After installing in a test maintenance window, enable WISI profile for
MIR24_TRANS alone from the channel editor; check:

```bash
journalctl -u dvbstreamer5.service --since '-15 min' --no-pager -o cat |
  grep -E 'NATIVE WISI ENCODER PROFILE|NATIVE VIDEO BURST DIAG|NATIVE VAAPI STRICT HRD|NATIVE CBR OUTPUT PCR SMOOTH|pcr_smoothing_test=|NATIVE AV MUX DROP|BACKPRESSURE'
```

Expected: `NATIVE WISI ENCODER PROFILE active` plus
`pcr_smoothing_test=1` on the selected stream if its startup buffer is
nonzero. Capture and analyze 60s PCAP; check PCR gap/residual, DTS-PCR
margins, audio/video timestamps, continuity, and encoder overshoot.
Previous tested 60-second V515 baseline for MIR24 was PCR P99 1.129 ms
and min end-PES DTS-PCR +353.43 ms; these are references, not guaranteed
thresholds for every scene/backend.

To roll back *only the profile*: uncheck WISI Chameleon profile on that
one channel and save. Service-wide binary rollback requires restoring
the previous portable installation under /opt/DVBStreamer5, which
interrupts all streams on restart.
