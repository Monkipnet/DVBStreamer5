# DVBStreamer5 V10.8.526 — bounded source-PES clock recovery for H.264 AUs

## Verified diagnostics (home2, 2026-11-14)

V525 runtime, SID 113 HISTORY, process 19385:

- `NATIVE DEMUX RAW AV CLOCK`: original video PES PTS remained close to
  audio PES PTS (about -1 to -2 seconds), while the *assembled* AVC AU
  PTS fell 126-147 seconds behind raw video PES PTS. The respective
  timestamps were fresh (typically a few milliseconds old).
- `NATIVE AV MUX`: audio queue was full at `512/512`,
  `drop_audio` reached at least 17500; video continued encoding.
- This persisted after user set HISTORY audio track to Auto; its
  selected audio PID was 772.
- Previously the assembler was extrapolating AU PTS using fixed 1800
  (one interlaced field) or 3600 (PAL frame) ticks without re-anchoring
  to subsequent PES headers. This can cause cumulative clock drift
  if the synthesized AU cadence differs from the source PES cadence.

## V526 changes

- Added `H264PesPtsRecovery`: records original H.264 PES PTS/DTS and
  their exact byte offsets in the Annex-B AU reassembly buffer.
- Applies a source anchor only to an AU starting at that PES boundary,
  never to an unfinished preceding NAL spanning multiple PES packets.
- Preserves existing extrapolated timing and B-frame jitter tolerance
  until accumulated signed 33-bit PTS difference exceeds **500 ms**;
  then locks to correctly aligned source PES timestamps. After lock,
  subsequent aligned timestamps keep source timing, instead of
  repeatedly accumulating 500-ms jumps.
- Handles PTS wrapping at 2^33 ticks and source DTS as well as PTS.
- Logs bounded `NATIVE AVC PES PTS RESYNC` diagnostics with PID,
  source and old timestamps, signed drift and event count.
- No changes to Newcamd/TSDecrypt/OSCam, NVENC/NVDEC, A/V mux lead
  envelopes, audio codec selection, HLS/SRT outputs, or EPG.
- Added C++ test `h264_pes_pts_recovery` to media-core CTest. It checks
  boundary ownership, PES split inside NAL, normal source jitter, PTS
  wrap and a 250-frame under-clock regression (even in Release builds).

## Build from source root

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
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
(cd dist && sha256sum -c DVBStreamer5-10.8.526-linux-x86_64.tar.gz.sha256)
```

The portable installer stops existing `dvbstreamer5.service` and
`oscam-mini.service`. Use a test server or maintenance window,
preserve `/opt/DVBStreamer5` and `/etc/dvbstreamer5` before installing,
and keep a known-good V525 rollback archive.

## home2 validation

After installing and starting V526 on home2:

```bash
cat /opt/DVBStreamer5/VERSION
PID=$(systemctl show -p MainPID --value dvbstreamer5.service)
journalctl -u dvbstreamer5.service --since '-15 min' --no-pager -o short-iso-precise |
  grep -F "run.sh[$PID]:" |
  grep -E 'NATIVE AVC PES PTS RESYNC|NATIVE DEMUX RAW AV CLOCK|NATIVE AV MUX stream_id=|NATIVE AV MUX PEER STALL|NATIVE AV MUX DROP'   > /tmp/dvb-v526-home2-avclock.log
grep -E 'NATIVE AVC PES PTS RESYNC|NATIVE DEMUX RAW AV CLOCK' /tmp/dvb-v526-home2-avclock.log | tail -n 25
grep -E 'NATIVE AV MUX stream_id=|NATIVE AV MUX DROP' /tmp/dvb-v526-home2-avclock.log | tail -n 20
```

Expected evidence: a `NATIVE AVC PES PTS RESYNC` event after drift
exceeds 500 ms, thereafter `raw_video_vs_assembled_ms` remains bounded
rather than growing to tens of seconds; `drop_audio` should remain
stable (ideally zero in a fresh process). Validate real A/V playback
and uninterrupted HLS/SRT/UDP outputs as well as logs.

This is a targeted code-level fix with a locally checked recovery-policy
unit test. Full binary compilation, CI, and on-server playback verification
must still be confirmed; do not claim the audio issue resolved until then.
