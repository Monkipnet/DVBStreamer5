# DVBStreamer5 V10.8.516 — WISI Chameleon per-channel CBR profile

Status: **experimental / opt-in**, not a proof of WISI decoder compliance.
All production channels remain unchanged until an operator explicitly checks
the new profile in the channel transcoding editor.

## Why

Earlier `svettranscoding` MIR24 comparison (VAAPI 1080p25, 6 Mbit/s
video, 8 Mbit/s UDP-CBR) showed:

| Capture | PCR max gap | PCR residual P99 | CC / TEI |
|---|---:|---:|---:|
| Copy | 37.788 ms | 1.132 ms | 0 / 0 |
| Transcode UDP-VBR | 40.000 ms | 14.757 ms | 0 / 0 |
| V515 transcoded UDP-CBR | 20.116 ms | 1.129 ms | 0 / 0 |

VBR average 5.526 Mbps but 100ms peak 16.409 Mbps. The V515 UDP-CBR
capture was 8.000 Mbps with maximum measured PCR residual 1.151 ms and
**+353.43 ms minimum sampled end-PES DTS–PCR lead**. These are field
measurements, not ETSI PCR accuracy or full T-STD/HRD conformance evidence.
Capture durations/scenes differed; CC/TEI zero only proves no such errors
were seen in those captures.

## Activation and eligibility

Web channel editor → Transcoding → **WISI Chameleon profile (A/B)**.
The setting is stored in `wisi_chameleon_profile` (bool), defaults to
`false`. Copy/paste transcoding settings includes the switch. Settings
for existing channels are unchanged.

Activation additionally requires:

- a transcoded channel, encoded H.264/H.265 video (not Video Copy);
- at least one UDP-CBR output with CBR enabled;
- effective video target fits the physical transport after reserved
  audio+PSI/TS allowance: max(500 kbit/s, audio + 5% CBR target).

If any gate fails the program logs `NATIVE WISI ENCODER PROFILE inactive`
and uses previous encoder/transport behavior. The UI flag stays saved,
so operators should disable it on unsupported configurations.

When enabled, StreamManager logs `NATIVE WISI ENCODER PROFILE active` and
propagates the selection into primary transcoder and UDP-CBR relay.
Browser preview, Copy/VBR, other stream IDs and HLS ABR variants retain
previous behavior.

### Encoder-specific behavior

| Backend | New WISI-profile treatment |
|---|---|
| VAAPI-direct H.264 | V514 experimental 400-ms rate-control window / 400-ms HRD CPB; actual driver compliance must be measured |
| QSV / oneVPL H.264 or HEVC | 400-ms video bitrate buffer (instead of legacy ~2 s in CPU-fed direct QSV), initial buffer half full |
| Intel QSV zero-copy | 400-ms buffer with half-full initial delay applied at both initial configuration and geometry re-init; default path otherwise unchanged |
| NVENC H.264 / HEVC | Keep old two-frame VBV unless an existing quality A/B selected more; any A/B cap limited to <=400ms; keep CBR, no B frames or lookahead |
| NVIDIA NVDEC-to-NVENC zero-copy | Same VBV rule as CPU-fed NVENC |
| OpenH264 CPU H.264 | Existing `RC_BITRATE_MODE`, target/max bitrate, overshoot correction and no frame skip. Report that no hard per-access-unit cap is available |
| Kvazaar CPU HEVC | Existing lambda bitrate rate control; per-picture size not hard bounded; preserve native GOP/ordering path |
| Video Copy | Not modified by the WISI encoder profile |

**Important:** no encoder SDK can guarantee one-size-fits-all "ideal" quality
and latency. Driver support, source complexity, available hardware and
decoder buffer capacity must be validated. The WISI profile is not an
automatic fix for frames that require more physical CBR transmission time
than the available DTS–PCR preroll.

### Transport behavior

The saved profile opts into the V515 output PCR smoothing path on an
eligible *buffered Source-PCR transcoded UDP-CBR output*. Original audio
and video PES bytes/PTS/DTS/continuity counters are preserved; the paced
UDP output stays 7 × 188-byte TS packets per 1316-byte datagram and
maintains the selected CBR rate. Reference-PID adaptation-only PCR
packets are inserted on ~20-ms physical output-slot intervals while
source media is queued. No packets are dropped or overwritten to
satisfy the PCR interval.

The PCR smoother requires a nonzero startup reservoir. The normal
V516 per-channel selection preserves existing target bitrate/startup
settings; no auto-rewrite of the channel config is performed. The relay
logs `NATIVE CBR OUTPUT PCR SMOOTH ... experimental=1` and
`WISI CBR start ... pcr_smoothing_test=1` only when the gated
smoother is actually activated. If the stored CBR startup buffer was
explicitly disabled, investigate before enabling the profile.

For selected WISI channels, `NATIVE VIDEO BURST DIAG` emits 5-second
max AU / IDR / non-random AU, peak-100-ms and peak-1-s encoder rates.
Other channels keep diagnostic logs disabled by default.

## Build/test (from source root, Ubuntu)

```bash
git switch main
git pull --ff-only origin main
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
bash scripts/make_portable_linux_bundle.sh --build-dir build \
  --output-dir dist/DVBStreamer5-10.8.516-linux-x86_64 --archive
sha256sum dist/DVBStreamer5-10.8.516-linux-x86_64.tar.gz
```

Full codec backend acceptance requires a GPU-equipped server. These
hardware-free tests prove only the policy, config, web integration and
transport invariants from the synthetic regression.

## Pilot and rollback

Start on `svettranscoding`, test tile MIR24_TRANS
`stream-1794627918823`, H.264 VAAPI 1920x1080p25, 6-Mbit/s video,
8-Mbit/s UDP-CBR. Record 30–60 sec PCAP from the multicast output and
compare PCR gaps, PCR residual P99/max, CC, TEI, DTS–PCR margins,
audio-vs-video timestamps, peak output bitrate and queue/drops. Repeat
with and without the saved WISI checkbox using **the same encoder,
bitrate, stream and output** as far as possible.

Direct image verification at WISI is currently unavailable. No
production recommendation is made until WISI-side monitoring or a
representative decoder validates clean A/V over extended running time.

Rollback just this experiment by unchecking WISI profile and saving the
channel settings; other channels need no change. If service-level
rollback is required, restore the previous portable bundle backed up
before install, restart service in a maintenance window, and verify
port/listener and journal status. Do not enable this profile globally.
