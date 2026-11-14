# DVBStreamer5 V10.8.527 — H.264 source PTS carried across an incomplete AU

## home2 V526 failure, November 14, 2026

The user installed V10.8.526 on home2, but the issue was still present:
- HISTORY, SID 113: `raw_video_vs_assembled_ms` approximately 201,720–201,920
  ms, with `raw_audio_vs_raw_video_ms` around -1 to -2 seconds.
- Sky Sport News, SID 108: `raw_video_vs_assembled_ms` around 55,380–55,560 ms.
- HISTORY audio mux full at 512 samples, cumulative `NATIVE AV MUX DROP`
  count at least 97,100.
- Hardware video producer was active (producer idle generally below 150 ms).
- The original video PES timestamps were fresh; the drift arose in
  `NativeTsDemux` before NVENC. Audio Auto / PID 772 did not repair it.
- At the end of the excerpt the video clock was near raw PES again, with
  HISTORY audio PID now 257 instead of 772. This could indicate channel
  reinitialization; do not infer self-recovery.

## Why V526 failed

`H264PesPtsRecovery::completeAu` only accepted PTS anchors whose recorded
PES byte offset was **exactly zero** at the start of an emitted AU.
It then discarded every anchor with `offset < consumedBytes`. If a
new PES starts **inside the end of the previous incomplete AVC frame**,
the header PTS belongs to the first AU starting in that PES (the
following AU). V526 discarded that PTS as the previous AU was emitted.
If PES boundaries commonly fall inside an AU, *no reanchor occurs*,
and the old erroneous 1800/3600-tick predictor can accumulate minutes
of A/V skew.

## V527 surgical correction

- Keep the newest PES timestamp from inside an emitted AU and **carry
  it to the start of the next assembled AU**; never attach it to the
  preceding unfinished frame.
- Prefer an actual PES PTS at the exact next AU boundary over an
  older interior-carried anchor, if both are present.
- Retain the V526 500-ms drift-detection threshold, source-clock lock,
  33-bit wrap-safe signed arithmetic, and DTS semantics.
- `NATIVE DEMUX RAW AV CLOCK` now includes
  `h264_interior_pes_carried_total` so home2 can verify whether this
  code path is actually being exercised.
- Added regression cases to `h264_pes_pts_recovery`:
  1000 consecutive 25-fps AUs whose source PES always begins halfway
  through the previous AU, with an incorrect 1800-tick frame predictor;
  and a case where exact-boundary PES PTS must supersede a carried
  interior anchor.
- Local C++17 standalone recovery helper regression passed with
  GCC `-O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror` and the same
  flags without NDEBUG. Full DVBStreamer5 CI and runtime playback
  validation **not yet confirmed**.
- No changes to DVB CA, OSCam, TSDecrypt, NVDEC/NVENC, A/V mux lead
  or depth limits, audio codecs, EPG, HLS/SRT/UDP/HTTP outputs.

## Build and CTest from source root

```bash
git switch main
git pull --ff-only origin main
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
ctest --test-dir build-release -R h264_pes_pts_recovery --output-on-failure
bash scripts/make_portable_linux_bundle.sh \
  --build-dir build-release --with-oscam --archive
(cd dist && sha256sum -c DVBStreamer5-10.8.527-linux-x86_64.tar.gz.sha256)
```

GitHub Actions `build-main-linux.yml` packages the versioned portable
bundle including OSCam and TSDecrypt. Confirm the exact SHA/run and
successful tests before downloading/deploying. For Ubuntu 18.04 check
glibc compatibility; the hosted runner builds on Ubuntu 24.04.

## home2 after installing V527

Do not install without an OSCam/config backup and a maintenance window:
the portable installer explicitly stops DVBStreamer5 and OSCam-mini.
Run:

```bash
cat /opt/DVBStreamer5/VERSION
PID=$(systemctl show -p MainPID --value dvbstreamer5.service)
journalctl -u dvbstreamer5.service --since '-15 min' \
  --no-pager -o short-iso-precise |
grep -F "run.sh[$PID]:" |
grep -E 'NATIVE AVC PES PTS RESYNC|NATIVE DEMUX RAW AV CLOCK|NATIVE AV MUX stream_id=|NATIVE AV MUX PEER STALL|NATIVE AV MUX DROP' \
  > /tmp/dvb-v527-home2-avclock.log

grep 'NATIVE DEMUX RAW AV CLOCK' /tmp/dvb-v527-home2-avclock.log | tail -n 20
grep 'NATIVE AVC PES PTS RESYNC' /tmp/dvb-v527-home2-avclock.log | tail -n 20
grep 'NATIVE AV MUX stream_id=' /tmp/dvb-v527-home2-avclock.log | tail -n 20
grep 'NATIVE AV MUX DROP' /tmp/dvb-v527-home2-avclock.log | tail -n 20
```

Expected in a fresh process: `h264_interior_pes_carried_total` is
nonzero/increasing when PES starts in the body of unfinished AUs,
`raw_video_vs_assembled_ms` stays bounded rather than growing toward
minutes, and `drop_audio` remains zero. Confirm with live audio and
picture on actual HLS/SRT/UDP outputs. If `carried_total` stays zero
and drift grows, investigate PES header-to-AU mapping further, rather
than increasing queues.

## Rollback

Before touching services, preserve known-good `/opt/DVBStreamer5`
and `/etc/dvbstreamer5`, and retain the V525/V526 portable tar.gz.
If the new release fails, stop services and restore your verified
previous installation plus `systemctl daemon-reload` and restart.
