# DVBStreamer5 V10.8.530 — OnDemand HLS live-edge startup prefill

**Date:** 2026-11-14. **Branch:** `Monkipnet/DVBStreamer5 main`.
**Scope:** transcoded MPEG-TS HLS with `activation_mode=ondemand` and
an effective segment target of 4 seconds or less. HTTP MPEG-TS,
DVB/UDP source, video codecs, remux, CA, online HLS, video-copy and
long custom HLS segments are intentionally left unchanged.

## Field evidence from svettranscoding V529

Stream: `stream-1794629815825`, `MIR24_TRANS_NO_CBR`;
UDP input, VAAPI 1080p25 transcode, HLS OnDemand output;
`hls_segment_duration=0` (Auto/4 seconds), `hls_segment_count=0`
(Auto/8 segments). Operator reports smooth HTTP TS and **approximately
one-second VLC freezes on every next HLS chunk**.

- VLC view and 1-second playlist monitor: an old completed OnDemand
  session showed `#EXT-X-ENDLIST` at 19:29:13; its manifest disappeared
  during restart at 19:29:26. New stream started with sequence 0 at
  19:29:30 and advanced 0→25 almost exactly every 4 seconds until
  19:31:10, with `endlist=0` and no OnDemand stop in the captured period.
  Thus the continual freezes in the new run are **not** explained by
  repeated stop/start or segment creation gaps.
- Local HTTP GET of the playlist: 200, 790 bytes, 0.000432 seconds.
  Local HTTP GET of the last media file: 200, 2,716,224 bytes,
  0.001584 seconds. This proves only **loopback** delivery, not
  end-to-end bandwidth to VLC.
- Native log: first segment `duration_s=2.04 target_s=4`,
  `HLS STARTUP READY ... first_media_ms=3803–4203`.
  Before V530 a transcoded segmenter's initial playlist was advertised
  after **one** completed chunk. An immediate VLC connection could start
  at the live edge with only ~2 seconds of playable media, then wait
  for every next 4-second chunk to be closed and published.

**Conclusion:** near-live-edge playback / insufficient initial media
buffer is the **leading hypothesis**, supported by timing and the
actual playlist-publication code. End-to-end playback after deploying
V530 still needs field verification; a different PTS/decoder problem
has not been entirely ruled out.

## Focused V530 implementation

1. `src/media/NativeHlsSegmenter.h`: opt-in
   `startupPrefillSegments=0` by default.
2. `src/StreamManager.cpp`: set this to **3** only for short-segment,
   transcoded OnDemand MPEG-TS HLS. Long explicit segment durations,
   passthrough and Online mode keep existing values.
3. `src/media/NativeHlsSegmenter.cpp`: complete segments as before
   (IDR-aligned, flush/close/encrypt before advertising); suppress
   publication of `video.m3u8` until three complete segments exist.
   Emit one `NATIVE HLS STARTUP PREFILL READY` diagnostic on first
   coherent publication. The moving 4-second rotation cadence and
   normal manifest updates **do not change**.
4. `src/http/HttpServerDvbMedia.inc`: this selected OnDemand startup
   may hold its original authorized GET for up to **20 seconds**,
   instead of 12 seconds, to wait for the three ready chunks; other
   transcoded OnDemand HLS retains 12 seconds and passthrough retains
   the 3-second wait. Already-running playlists return promptly.
5. `tests/media_core/test_native_hls_source_switch.cpp`: native
   segmenter regression checks first publication only after three
   finalized segments while existing primary/backup source switch
   preserves its established behavior.
6. `tests/web/test_ondemand_hls_prefill_v530.js`: source/wiring
   regression added to both matching sections of
   `.github/workflows/build-main-linux.yml`.
7. `src/AppVersion.h` → `10.8.530`.

**Trade-off:** first picture on OnDemand HLS can start approximately
10–15 seconds after a cold connection, rather than around 4 seconds,
but with an initial stock of media to buffer. For individual viewers
joining an already-running stream the manifest remains an ordinary
rolling HLS playlist. No client can be forced to buffer if it ignores
normal HLS live-edge conventions; still verify VLC and external
receivers. Do not equate successful first publication with validated
continuous playback.

## Build from repository source root

```bash
git switch main
git pull --ff-only origin main
node tests/web/test_ondemand_activation_v528.js
node tests/web/test_ondemand_viewer_v529.js
node tests/web/test_ondemand_hls_prefill_v530.js
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
(cd dist && sha256sum -c DVBStreamer5-10.8.530-linux-x86_64.tar.gz.sha256)
```

The build may require repository vendored dependencies already installed
or prepared by the existing normal DVBStreamer5 build workflow.
GitHub Actions `build-main-linux.yml` builds the portable package.
Full CI, actual CMake/CTest and remote VLC test were **not yet
confirmed** at the time of this documentation commit.

## Playback validation

- First check `cat /opt/DVBStreamer5/VERSION` after installation
  on a test server. Simply working in the extracted dist directory
  does not establish what systemd is running.
- Open only the **public** HLS URL of `MIR24_TRANS_NO_CBR` in VLC,
  with unchanged 4s/8 Auto profile, 25fps, VAAPI and OnDemand.
- Read `NATIVE HLS FIRST SEGMENT` and
  `NATIVE HLS STARTUP PREFILL READY ... segments=3 target=3`.
- First published playlist should list **at least three** finalized
  `#EXTINF` segments; it should not include `#EXT-X-ENDLIST`.
- Monitor sequence increments for 3–5 minutes and watch for sub-second
  freezes. Only a smooth result counts as a playback fix.
- Check HTTP TS of the **same** channel remains smooth and verify another
  Online HLS or Video Copy channel has not changed startup.
- After VLC disconnects, OnDemand remains bounded by viewer activity
  and the V529 last-HLS-media grace of 75 seconds.
- Keep a known-good V529 portable tarball and configuration backups
  before installing. Portable installation interrupts service;
  perform it only in a maintenance window. If any regression occurs,
  restore the verified V529 archive and check channel/CA/EPG outputs.

## Quick readonly diagnostics

```bash
ID=stream-1794629815825
DIR=/tmp/dvbstreamer5-hls/$ID
cat /opt/DVBStreamer5/VERSION
journalctl -u dvbstreamer5.service --since '-10 min' --no-pager -o cat |
  grep -E 'NATIVE HLS (FIRST SEGMENT|STARTUP PREFILL READY|SEGMENT DELAY|OUTPUT FAILED)|HLS STARTUP (READY|PENDING)|ONDEMAND (ACTIVATE|DEACTIVATE)' | tail -n 100
test -f "$DIR/video.m3u8" && {
    grep -E '^#EXTINF:|^#EXT-X-MEDIA-SEQUENCE:|^#EXT-X-ENDLIST' "$DIR/video.m3u8"
}
```

No tuning, HTTP listener, encoder or channel is restarted by these diagnostics.
