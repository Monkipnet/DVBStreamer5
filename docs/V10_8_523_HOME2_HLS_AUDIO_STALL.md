# DVBStreamer5 V10.8.523 — home2 NVENC/HLS A/V mux skew investigation

## Evidence from home2 on 2026-11-14

The V522 user log excerpt contains two different pathological A/V histories:

- `sat-1794213237299-113-4` (older session): audio mux queue at 508–512 / 512, and `drop_audio` increased beyond 26,000. A/V output delta briefly grew to over 30 seconds as mux audio progress stalled.
- `sat-1794213401098-108-0`: repeated `NATIVE AV MUX PEER STALL waiting=audio missing=video` with `wait_ms` around 4–12 seconds and audio queue around 22–63; video output samples still progress between reports; after startup `drop_audio=0` in the supplied excerpt.
- A `NATIVE NVIDIA ZERO-COPY FPS DROP dropped=1` then `dropped=2` is present. Multiple `ZERO-COPY selected`, `RANDOM ACCESS frame=0` and `NATIVE HLS FIRST SEGMENT` lines suggest pipeline restarts/reconfigurations; determine why from unfiltered journal before attributing picture corruption to NVENC itself.

**Crucial distinction:** `wait_ms` is the `queuedAt` age of the oldest waiting mux sample, NOT a direct measurement of how long the opposite encoder has been inactive. No evidence yet that a 12-second video outage occurred. Do not increase A/V hard-lead cap, reset DTS/PTS, or blindly drop waiting audio.

## V523 instrumentation

Retain V522 scheduler and all defaults, including 150ms preferred A/V lead, 500ms hard lead, 64 video and 512 audio mux queue bounds. Add to each rate-limited (5 s) `NATIVE AV MUX PEER STALL` diagnostic:

- `video_producer_idle_ms`, `audio_producer_idle_ms`: actual wall time since each encoded track most recently reached the mux, **including attempted enqueues when full**;
- `last_video_produced_pts`, `last_audio_produced_pts`: PTS/DTS of latest encoded arrival for each track;
- `queued_head_pts`, `queued_video_head_pts`, `queued_audio_head_pts`: oldest queued samples' timestamps;
- `input_video_pts`, `input_audio_pts`, `encoded_audio_pts`: producer/source watermarks.

Together these distinguish source PTS discontinuity, a slow/missing video producer, and an ordinary long mux queue without conflating queue age with video inactivity.

## Build/test/package from repository root

```bash
git switch main
git pull --ff-only origin main
grep kProgramVersion src/AppVersion.h
node tests/web/test_mux_audio_stall_policy.js
node tests/web/test_nvenc_hls_keyframe_policy.js
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
VERSION=$(sed -n 's/.*kProgramVersion = "\([^"]*\)";.*/\1/p' src/AppVersion.h)
(cd dist && sha256sum -c "DVBStreamer5-${VERSION}-linux-x86_64.tar.gz.sha256")
```

Portable: `dist/DVBStreamer5-10.8.523-linux-x86_64.tar.gz`.

## Capture full relevant journal from a V523 test

```bash
journalctl -u dvbstreamer5.service --since '-30 min' --no-pager -o short-iso-precise |
  grep -Ei 'sat-1794213237299-113-4|sat-1794213401098-108-0|NATIVE AV MUX PEER STALL|NATIVE AV MUX DROP|NVIDIA ZERO-COPY|NVENC|HLS|FAILOVER|FAILBACK|RESTART|INPUT LOSS|MUX|AUDIO|ERROR|WARNING' \
  > /tmp/dvb-v523-home2-full.log
tail -n 200 /tmp/dvb-v523-home2-full.log
```

Interpretation:
- `video_producer_idle_ms` stays small but `wait_ms` grows: timestamp skew/queue scheduling rather than a stopped video producer; inspect latest produced and queued PTS.
- `video_producer_idle_ms` grows with `wait_ms`: actual zero-copy/NVENC video production gap, investigate decoder/reinit and input.
- `last_audio_produced_pts` jumps far ahead of video while latest video PTS stays smooth: suspect audio input/encoder timestamp discontinuity, then inspect full input and media segments.
- For HLS playback verify AAC PID, TS continuity, PCR, PTS and `EXT-X-DISCONTINUITY` in the actually served segment files before adjusting timing.

Never upload secret configuration, CAM keys, full subscriber lists or credentials in shared logs.
