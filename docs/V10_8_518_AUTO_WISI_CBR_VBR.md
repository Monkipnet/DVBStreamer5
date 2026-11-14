# DVBStreamer5 V10.8.518 — automatic encoder profile, UDP CBR and VBR

This version extends the *measured* V517 VAAPI/UDP-CBR profile to
**automatic** transcoded H.264/H.265 UDP operation. No WISI checkbox is
required. Legacy JSON `wisi_chameleon_profile` fields are ignored and not
written back. Video Copy and non-transcoded outputs remain on their old path.

## Encoder policy

The selected CPU/VAAPI/QSV/NVENC backend (including Intel and NVIDIA
zero-copy) inherits the automatic WISI encoder buffer policy when a channel
transcodes H.264/HEVC to UDP-CBR or UDP-VBR:

- Direct VAAPI H.264: VAAPI strict HRD request, bitrate-based 400ms CPB,
  50% initial occupancy. Driver support must be checked on the actual GPU.
- Intel QSV / oneVPL and Intel zero-copy: 400ms BufferSizeInKB,
  half-filled InitialDelayInKB, CBR, no B-frame reorder.
- NVIDIA NVENC (including zero-copy): CBR/low-latency, existing 2-frame
  VBV retained, any larger preset VBV capped at <=400ms, no B-frames,
  lookahead or multi-pass. Actual driver acceptance not proven.
- CPU OpenH264 and Kvazaar: keep tested codec-specific rate-control
  modes, CABAC/bitrate overshoot safeguards and live complexity/preset.
  Monitor worst encoded access units; do not claim per-frame hard caps.
- No bitrate/profile changes to browser preview or HLS ABR renditions.

## UDP-CBR

Use V517 tested physical-slot PCR-only smoothing (~20ms) and
Source-PCR useful-packet pacing for eligible transcoded channels with
nonzero startup buffers. Respect configured physical bitrate and
existing CBR-buffer selection. If headroom for video + audio + TS/PSI
is inadequate, the encoder profile and smoother stay disabled and an
explicit log reports the failed budget gate. Copy and non-UDP paths
remain unchanged.

## UDP-VBR

A different transport path, `PcrVbrUdpOutputWorker`, already sends
packet groups according to source-PCR intervals. V518 adds optional
**per-output best-effort peak pacing** only on transcoded video:

- cap = clamp(2*(effective video bitrate + audio bitrate + 500 kbit/s),
  4 Mbit/s, 100 Mbit/s). For 6M+192k this is 13.384 Mbit/s.
- Space UDP datagrams using monotonic send timestamps and sizes. Do
  **not** inject null packets, change PCR/PTS/DTS/CC or turn VBR into CBR.
- Never add more than 80ms to the given source-PCR deadline; bypass the
  cap in a risk-of-underrun case and report `cap_deadline_bypass`.
- Preserve the existing per-output worker, shared observer fanout,
  backpressure boundaries and no-PCR fallback. In particular, a
  slow HLS/HTTP observer cannot block physical UDP.

This is a ceiling *target*, not a guarantee: bursts can bypass when
necessary for decoder timing. VBR needs receiver peak bitrate and jitter
buffer configuration. A VBR stream whose average bitrate is too high
for the network cannot be made safe solely with peak pacing.

## Field evidence and remaining verification

On `svettranscoding` Intel Haswell, the V517 **VAAPI H.264 CBR**
baseline ran for ~600 seconds at 8.000 Mbit/s. Measured PCR
gap max=20.116ms, PCR residual P99=1.132ms, no measured CC/TEI,
and min observed PES DTS-PCR lead +339.07ms. A 580-second middle
extract decoded with FFmpeg `-xerror`, exit 0, 0 error lines.
These are not full T-STD/HRD conformity or WISI receiver tests.

V518 must still be verified with CI, then with a fresh 10-minute
CBR+VBR pair of captures (and audio/video strict decoding). The
NVENC driver paths can only be verified on the GPU-equipped `home2`
machine. Do not move V518 to production without passing these tests.

## GitHub Actions / portable

`.github/workflows/build-main-linux.yml` is triggered on main push.
It builds full app with NVENC/QSV/VAAPI flags, pinned codec libraries,
OSCam-mini and TSDecrypt, runs unit/UI CTests, and publishes a 30-day
workflow artifact named `DVBStreamer5-linux-x86_64` containing:

- `DVBStreamer5-10.8.518-linux-x86_64.tar.gz`
- `SHA256SUMS.txt`
- raw `DVBStreamer5` binary

Download from the successful workflow run's **Artifacts** section:
https://github.com/Monkipnet/DVBStreamer5/actions/workflows/build-main-linux.yml

## Test / install / rollback (execute from source root)

```bash
git switch main && git pull --ff-only origin main
grep kProgramVersion src/AppVersion.h
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_ENABLE_VAAPI=ON \
  -DDVBSTREAMER5_ENABLE_INTEL_QSV=ON \
  -DDVBSTREAMER5_ENABLE_NVENC=ON \
  -DDVBSTREAMER5_BUILD_MEDIA_CORE_TESTS=ON
cmake --build build --parallel 2
ctest --test-dir build -R \
  'wisi_profile_policy|wisi_profile_config|udp_vbr_peak_rate|udp_vbr_pcr_clock|shared_vbr_fanout|cbr_output_pcr_smoothing|vaapi_hrd_policy' \
  --output-on-failure
node tests/web/test_wisi_chameleon_profile_ui.js
ctest --test-dir build --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build \
  --output-dir dist/DVBStreamer5-10.8.518-linux-x86_64 --archive
sha256sum dist/DVBStreamer5-10.8.518-linux-x86_64.tar.gz
```

On `home2`: save the existing `/opt/DVBStreamer5` and
`/etc/dvbstreamer5` before replacing the portable bundle, schedule
a maintenance window, and keep the complete previous installation for
rollback. GPU device presence is not enough: journal evidence must
confirm NVENC initialization/API compatibility, SPS/PPS/IDR, and
the strict decode. On NVENC failure, roll back the bundle rather than
changing unrelated channel settings.
