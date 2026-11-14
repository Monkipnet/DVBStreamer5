# DVBStreamer5 V10.8.522 — NVENC HLS keyframes and audio peer-stall diagnostics

## Symptoms (home2 / HLS, 2026-11-14)

- Occasional picture corruption reported while using NVENC.
- Periodic missing audio reported for HLS.
- Earlier home2 V518 logs: `NATIVE AV MUX DROP kind=audio reason=queue_full`, audio queue 512/512, thousands of dropped audio samples. This demonstrates mux audio loss for those samples, **not** a proven root cause for every HLS dropout.

## Fix

Both `src/media/NativeHardwareCodec.cpp` (CPU-fed NVENC) and
`src/media/NativeNvidiaZeroCopy.cpp` (NVDEC/CUDA/NVENC) previously advertised
`NV_ENC_PIC_TYPE_I` as a random-access frame. A normal I picture is not always
an IDR and can depend on earlier decoder reference pictures; HLS clients may
start a segment on an invalid boundary if it is incorrectly flagged.

Only explicit `NV_ENC_PIC_FLAG_FORCEIDR` requests / confirmed `NV_ENC_PIC_TYPE_IDR`
remain flagged as random access; the regular forced GOP IDRs and accompanying
SPS/PPS/VPS are preserved.

## Audio diagnostics (no behavioral changes)

When a mux queue head has been waiting at least 2 seconds with the peer
clock established, `NATIVE AV MUX PEER STALL` records at most once every
5 seconds per stream: waiting kind, missing peer, queued video/audio samples,
A/V lead and last mux clocks. This reveals peer starvation that previously
produced no `NATIVE AV MUX` progress logs during a full stall.

**Do not infer HLS audio repair from these diagnostics.** The 150ms preferred
A/V lead, 500ms hard lead, 350ms max hold and queue limits remain unchanged,
pending real logs and TS segment verification.

## Build and verify (source repo root)

```bash
git switch main
git pull --ff-only origin main
grep kProgramVersion src/AppVersion.h
node tests/web/test_nvenc_hls_keyframe_policy.js
node tests/web/test_mux_audio_stall_policy.js
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
VERSION="$(sed -n 's/.*kProgramVersion = "\([^"]*\)";.*/\1/p' src/AppVersion.h)"
(cd dist && sha256sum -c "DVBStreamer5-${VERSION}-linux-x86_64.tar.gz.sha256")
```

Expected archive: `dist/DVBStreamer5-10.8.522-linux-x86_64.tar.gz`.
Build does not restart production. Install only after backing up the installed
runtime and persistent configs and scheduling a test service restart.

## Capture evidence on home2

```bash
journalctl -u dvbstreamer5.service --since '-30 min' --no-pager -o cat |
  grep -E 'NATIVE AV MUX|NATIVE VIDEO ENCODE PERF|NATIVE VIDEO INPUT CADENCE|NATIVE ASYNC VIDEO RESYNC|NATIVE PRIMARY RAW DROP|NATIVE NVIDIA|NATIVE NVENC|NATIVE HLS|NATIVE AUDIO' \
  > /tmp/dvb-v522-nvenc-hls-audio.log
wc -l /tmp/dvb-v522-nvenc-hls-audio.log
tail -n 150 /tmp/dvb-v522-nvenc-hls-audio.log
```

If sound disappears but there is no `NATIVE AV MUX DROP` or `PEER STALL`,
verify actual HLS media segments (AAC PID, continuity, PCR and PTS) and input
track switching before altering mux timing or increasing buffering.
