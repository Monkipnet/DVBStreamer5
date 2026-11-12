# DVBStreamer5 V10.8.485 — staged realtime CPU/PCR remediation

**Status:** source patch; not verified on Eurosport / WISI Chameleon.
Do not deploy to production until side-by-side comparison with Astra.

## Changes

1. `NativeMpegTsMux::sampleClock`: if a DTS/PTS is duplicated or
   regresses, clamp the internal clock to +1 tick rather than
   synthesizing a nominal full 25-fps frame (+3600 ticks). Normal
   forward timestamps and wrap handling retain the original time base.
   This avoids artificial PCR acceleration due to timestamp repeats.
   An actual source discontinuity still requires transport validation.
2. `NativeTranscoderPipeline`: on the compressed-PES queue reaching
   128 items, first look for the most recent random-access picture and
   retain the complete GOP from that point. Reset the decoder before
   resuming. If no usable IDR exists, keep the safe
   clear-and-wait-for-IDR policy; never decode orphaned P/B pictures.
   This reduces over-dropping; it does not increase encoder throughput.
3. OpenH264 CPU complexity honors the `fast`/`balanced` preset.
   Previously all SD and <=720p/30-fps streams were forced to
   `HIGH_COMPLEXITY` even with a fast preset.
   Explicit quality/best presets retain high complexity.
4. PCM resampling bypasses per-sample floating-point interpolation
   when sample rate and channel layout already match (typically 48kHz,
   stereo audio), preserving exact samples/PTS.
5. UDP-VBR PCR anchor lookup uses `std::upper_bound` across deque
   iterators rather than a 512-entry linear scan, with equivalent
   interpolation/extrapolation and no TS modification.
6. Tile editor: remove obsolete `Fixed/Source PCR`,
   `DVB Source PCR pacing`, `HTTP/SRT source PCR` toggles and
   the legacy HTML injection. Retain **CBR bitrate and startup buffer**.
   Existing serialized private `target_bitrate` PCR marker and both
   per-tile boolean preferences are retained on unrelated saves;
   backend compatibility is preserved.

## Scope and limits

The user-observed V482 Eurosport test (08:38–08:48 UTC) dropped
about 11k of 16k video frames, produced ~8 rather than 25 fps,
had 821 PCR discontinuities with jumps up to 2.5 seconds and
a reported ~1% clock speed-up. CPU was reported 7% vs 3% for
the reference FFmpeg, and passthrough PCR jitter 93ms vs 16ms.
These results have **not** been reproduced or resolved in this commit;
this is a first small, reversible batch of code fixes.

No changes to CAM/TSDecrypt, EPG, UDP CBR pacer, transport stream
header/CC rewriting, encoder backends other than CPU complexity,
channel auto-start, service control or systemd. No running tiles are
restarted by checkout or compilation.

## Regression checks

```bash
git switch main
git pull --ff-only origin main
grep kProgramVersion src/AppVersion.h

cmake -S tests/media_core -B build-test-media -DCMAKE_BUILD_TYPE=Release
cmake --build build-test-media --parallel 2
ctest --test-dir build-test-media --output-on-failure

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
node tests/web/test_cbr_profile_ui.js
node tests/web/test_pcr_cpu_optimization_ui.js
ctest --test-dir build -R udp_vbr_pcr --output-on-failure
ctest --test-dir build --output-on-failure
```

## Acceptance criteria before production

**Transcoded Eurosport:** at least 25 fps sustained at 25-fps target,
no persistent encoder/RAW/PES/mux queue drops, no cumulative A/V drift,
monotonic PCR and accepted PCR jitter. Capture:
`NATIVE VIDEO DECODE PERF`, `NATIVE VIDEO ENCODE PERF`,
`NATIVE AV MUX`, `NATIVE AV CLOCK TRACE`,
`NATIVE ASYNC VIDEO RESYNC`, `NATIVE PRIMARY RAW DROP` and
`NATIVE UDP VBR PCR RESET`.

**Untranscoded Usadba:** compare source PCR and outgoing UDP PCR under
the same 10-minute window as Astra; target equivalence to the source,
with no continuity errors or byte changes. Check packet pacing mode
and wall-time PCR jitter as separate measurements.

Avoid adjusting CBR queue sizes until real encoder time per frame
and queue occupancy have been measured. A larger queue cannot make
an encoder that sustains 8 fps produce 25 fps; it only adds delay.
Do not enable CPU debugging/dump outputs on production by default.
