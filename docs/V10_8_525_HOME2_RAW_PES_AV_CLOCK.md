# DVBStreamer5 V10.8.525 — home2 raw PES vs H.264 AU clock audit

## Evidence (14 Nov 2026)

From `home2`, channels `sat-1794213237299-113-4` (HISTORY, SID 113)
and `sat-1794213401098-108-0` (Sky Sport News, SID 108):

- `NATIVE AV MUX PEER STALL waiting=audio missing=video` shows normal
  `video_producer_idle_ms` (mostly 0-238 ms), but very large sample
  `wait_ms` and `queue_video=0`. Thus it does **not** prove NVENC stopped.
- Channel 113: `last_video_produced_pts=8498053316`,
  `last_audio_produced_pts=8499909714`: audio producer media clock is
  about 20.6 seconds ahead, while `input_video_pts=8498064116` and
  `input_audio_pts=8499910194` reproduce that skew *before encoding*.
  Audio queue full `512/512`; `drop_audio` reached at least 6600.
- Channel 108: audio PES/encoded PTS ahead of video by around six seconds.
- Video PES arrivals have 200-672 ms gaps with normal source PTS
  steps of 40 ms and no PES queue drops in sampled windows. Actual
  clock difference is far larger than that short-term jitter.
- NVIDIA GTX 1060 6GB driver 580.196.07; NVENC 2-8%,
  NVDEC 2-19%; no evidence of GPU saturation. Sporadic NVDEC
  concealed-corruption events are separate.
- `NATIVE AV CLOCK TRACE` had zero matches in the filtered V524 log;
  raw source video PES header timestamps were **not** yet available.
  `lastInputVideoPts90k_` is recorded *after* the AVC Annex-B
  access-unit assembler, not directly from source PES headers.

## Narrow hypothesis

`NativeTsDemux::queueVideoSample` generates AU timestamps by advancing
the retained PTS 1800/3600 ticks per field/frame while reassembling H.264.
If incoming raw PES PTS occasionally jumps because an AU is missing,
or source timestamps are otherwise discontinuous, the generated
AU clock might drift behind raw source PTS and AC-3 PTS. That needs
a controlled measurement **before** changing clock recovery.

## V525 changes (diagnostic only)

- `NativeTsDemux` captures raw H.264 video PES header PTS before
  AU reconstruction, raw audio PES header PTS, and most recently
  emitted reconstructed H.264 AU PTS.
- Every 5 seconds (if all values exist), logs
  `NATIVE DEMUX RAW AV CLOCK` with
  `service_id`, `pmt_pid`, `video_pid`, `audio_pid`,
  `raw_video_pes_pts`, `raw_audio_pes_pts`,
  `assembled_video_au_pts`, and three wrap-aware signed
  offset fields: `raw_audio_vs_raw_video_ms`,
  `raw_video_vs_assembled_ms`,
  `raw_audio_vs_assembled_ms`.
  Age fields flag stale timestamps.
- No A/V scheduling, hardware decode/encode, PTS assignment, 500-ms
  lead envelope, queue sizes, video frame drops, audio codec or CA
  handling changed.
- `tests/web/test_raw_pes_av_clock_policy.js`: timestamp-diagnostic
  invariants and signed 33-bit PTS wrap arithmetic. Included in Linux
  main CI.

## How to interpret

1. If `raw_audio_vs_raw_video_ms` remains near normal while
   `raw_video_vs_assembled_ms` increases to seconds, investigate
   H.264 PES-to-AU source-PTS reanchoring, preserving the correct
   association between a PES header PTS and its access unit.
2. If `raw_audio_vs_raw_video_ms` itself grows to seconds,
   investigate input TS/PCR, source H.264 PES timestamp continuity
   and audio track timeline *upstream* of the transcoder.
3. If raw vs reconstructed AV clocks remain aligned but the mux
   clocks diverge, investigate queue scheduling and encoder PTS.
4. Avoid blindly increasing mux queue limit or allowing seconds of
   unrestricted A/V drift; neither repairs the source timebase.

## Commands from repository root

```bash
git switch main
git pull --ff-only origin main
node tests/web/test_raw_pes_av_clock_policy.js
node tests/web/test_mux_audio_stall_policy.js
node tests/web/test_incremental_build_policy.js

bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
(cd dist && sha256sum -c DVBStreamer5-10.8.525-linux-x86_64.tar.gz.sha256)
```

Install only on a test server after build and CI validation; portable
installer stops the systemd DVBStreamer5 and OSCam-mini services, so a
production upgrade requires a maintenance window and rollback copy.

Then collect:

```bash
journalctl -u dvbstreamer5.service --since '-15 min' --no-pager -o short-iso-precise |
    grep -E 'DVBStreamer5 10\.8\.|NATIVE DEMUX RAW AV CLOCK|NATIVE AV MUX PEER STALL|NATIVE AV MUX DROP|NATIVE VIDEO INPUT CADENCE' \
    > /tmp/dvb-v525-home2-avclock.log
wc -l /tmp/dvb-v525-home2-avclock.log
grep 'NATIVE DEMUX RAW AV CLOCK' /tmp/dvb-v525-home2-avclock.log | tail -n 30
```

No claimed runtime build or CI success in this note. The observed audio
failure is **not** claimed fixed in V525.
