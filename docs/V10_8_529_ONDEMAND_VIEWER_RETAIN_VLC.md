# DVBStreamer5 V10.8.529 — OnDemand viewer-retention fix (VLC / HLS)

Date: 2026-11-14. Branch: `Monkipnet/DVBStreamer5 main`.
This release follows the DVB Unicable/JESS and HLS HEAD changes in V528.

## Field observation

On `svettranscoding` the configured channel
`stream-1794629815825` / `MIR24_TRANS_NO_CBR` has UDP input
and HLS output, `activation_mode=ondemand`.

The supplied log showed:

```text
ONDEMAND ACTIVATE stream=stream-1794629815825 source=preview startup_ms=5 ca=fta
HLS STARTUP READY stream=stream-1794629815825 activation_ms=0 first_media_ms=3503 attempts=35
ONDEMAND DEACTIVATE stream=stream-1794629815825 reason=no-clients idle_s=30 hls_startup_expired=0
```

The user explicitly reported opening the channel **in VLC**.
The `source=preview` marker is *the origin of the first activation*,
not proof that the user only used a web-browser preview or that VLC was
not connected. Whether VLC was still receiving packets at the moment
of deactivation was not proven by these lines.

## Defect and adjustment

`StreamManager::monitorOnDemandStreams()` formerly kept the pipeline
running only for:
- a connected *public* HTTP TS session (`previewSession.empty()`);
- a request through the OnDemand endpoint within 30 seconds; or
- the separate bounded first-segment HLS cold-start policy.

An **actively connected private HTTP preview** was excluded. HLS media
playback has no persistent TCP socket: `serveHlsFile()` already records
each successful `.ts/.m4s/.mp4/.aac` request in `adHocSessions`, but
the OnDemand monitor did not consult that verified media-session
timestamp. A player buffering more than 30 seconds could lose the
pipeline despite recent real media delivery.

V529:
1. Counts a connected private HTTP preview session as an active viewer,
   while still removing it promptly when its socket disconnects.
2. Also checks `adHocSessions` **only for HLS sessions that have actually
   received a successful media response**. Retains the pipeline for
   at most **75 seconds** since that last media delivery.
3. Keeps the existing 30-second idle policy for other inactive streams
   and 120-second first-HLS-segment startup cap for transcoded cold start.
4. After 75 seconds with no new HLS media response and no other active
   playback, stops OnDemand; **no permanent keep-warm**.
5. Preserves explicit operator Stop, DVB occupied frontend safety,
   HLS authorization/403 and normal Online mode; no changes to
   PTS, OSCam, codecs, mux or other running tiles.

**Not a root-cause proof for all IP OnDemand failures:** If VLC requests
a manifest but receives no actual segment, these retention guards do
not convert a non-playing session into a viewer. Check path,
authorization, GET response and segment generation if an independent
IP channel still fails.

## Tests and status

- `tests/media_core/test_ondemand_hls_startup_policy.cpp` extended
  with boundary checks for the 75s media grace (at 74s true,
  at exactly 75s false, empty and future timestamps false).
- `tests/web/test_ondemand_viewer_v529.js` checks that the actual
  OnDemand monitor recognizes preview sockets and recent HLS media,
  retains idle shutdown and respects existing GET/segment conditions.
- The test was registered in the existing GitHub Actions workflow.
- Source-level consistency checked on main; **CI, full CTest and
  real VLC playback after installing V529 are not yet confirmed.**

## Build/verify from repository root

```bash
git switch main
git pull --ff-only origin main
node tests/web/test_ondemand_activation_v528.js
node tests/web/test_ondemand_viewer_v529.js
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
(cd dist && sha256sum -c DVBStreamer5-10.8.529-linux-x86_64.tar.gz.sha256)
```

GitHub Actions `build-main-linux.yml` uploads
`DVBStreamer5-10.8.529-linux-x86_64`. Verify the actual SHA, run,
artifact contents and digest before use.

## VLC verification (no bulk restarts)

1. On a **test installation** of V529, keep
   `stream-1794629815825` in UDP input / HLS output / OnDemand.
2. Open its actual HLS output URL in VLC; watch video and audio for
   at least 2 minutes, not merely the window opening.
3. Compare timestamps of `ONDEMAND ACTIVATE`, `HLS STARTUP READY`,
   `ONDEMAND HLS FIRST MEDIA`, `ONDEMAND DEACTIVATE`,
   `NATIVE HTTP TS CLIENT attach/detach` and `HLS STARTUP PENDING`.
4. While VLC *continues fetching media*, the stream must not
   deactivate. After closing VLC, the stream must eventually deactivate
   when no new segment is fetched and the 75s bounded media grace ends.
5. Test a second, ordinary online tile and manual Stop; neither may
   be restarted by a viewer or accidentally interrupted.

```bash
cat /opt/DVBStreamer5/VERSION
journalctl -u dvbstreamer5.service --since '-10 min' -o cat --no-pager |
  grep -E 'ONDEMAND|HLS STARTUP|NATIVE HTTP TS CLIENT (attach|detach)' |
  tail -n 120
```

**Production safety:** Installing the portable bundle stops service
processes. Do not deploy on home2 or other production servers without
backing up the installed V528/V527 bundle/config and arranging a window.
If a regression appears, use the previous verified portable archive
to roll back and recheck unaffected channels.
