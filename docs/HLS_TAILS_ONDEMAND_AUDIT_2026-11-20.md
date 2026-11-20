# DVBStreamer5 — HLS tail cleanup audit (20.11.2026)

## Verified observations

* `StreamManager::stopStream` first requests shutdown, joins input/relay and monitor workers, then stops HLS MPEG-TS, ABR variants and CMAF; an automatic primary/backup failover deliberately skips stopping HLS to preserve playlist sequence and client playback.
* `NativeHlsSegmenter::stop()` flushes segments and writes a final `#EXT-X-ENDLIST`, but does **not** remove the runtime output directory. `NativeHlsSegmenter::start()` removes old `video.m3u8`, `video.m3u8.tmp` and `segment*.ts` only when HLS archive is off.
* ABR outputs additionally use `/abr/<rendition>` and a root `master.m3u8`. ABR cleanup currently happens on some fresh starts, not upon the ordinary stop.
* `NativeCmafSegmenter::start()` creates or overwrites the playlist but does not clear old `segment*.m4s` / `init*.mp4`. Its live-window prune removes old .m4s but not old init files; stop writes ENDLIST.
* Non-archived HLS uses `/tmp/dvbstreamer5-hls/<stream.id>`; archived HLS uses configurable `hlsArchivePath/<stream.id>`. **Do not recursively remove archive roots.**
* `HlsFileCache` limits cached small files to 64 MiB and checks size/mtime per request, but has no per-directory invalidation. Deleted files cannot be served by normal cache reads because the filesystem is checked first.
* OnDemand uses successful segment responses, not manifest polling, to retain an active viewer. There is a 75-second recent-media grace and a separate up-to-120-second cold-start grace. When idle, the monitor calls the same `stopStream(id)`.

> **Update V10.8.598 (20 November 2026):** The audited gap is now
> addressed in code. See `src/media/HlsTemporaryCleanup.h` and
> `tests/test_hls_temporary_cleanup_v598.cpp`. The lifecycle cleanup
> quarantines non-archive TS/ABR/CMAF directories on final Stop and skips
> active concurrent starts and automatic failover. At startup a conservative
> scavenger removes only ended `video.m3u8` directories whose files are all
> older than 24 hours. The original audit below is retained as historical
> context; validation on home2 and GitHub Actions is still pending.
>
> **Important:** An unexpected stop after a crash with NO `#EXT-X-ENDLIST`
> does not qualify for automatic startup scavenging. No media is deleted
> solely because it is old; this is intentional for safety.

## Safe implementation design — NOT YET IMPLEMENTED

1. Make temporary HLS ownership explicit per stream and generation. Never delete the current generation while a new stream has started with the same ID; the async stop worker must not race with a subsequent start.
2. Clean only after the *actual* final teardown of that stream (after segmenter `stop()`, input/relay and pending file writer shutdown), and never on ordinary HTTP playlist requests, segment request completion or viewer disconnect.
3. Do not clean during `automaticFailoverTransition` or `sourceSwitched()`; these intentionally retain live playlists, ABR/CMAF state, content sequence and clients.
4. For non-archive streams, remove precisely the owned temporary per-stream directory (MPEG-TS and ABR/CMAF) with safe path/symlink validation. Do not touch directories for other stream IDs, archive paths, user files, CA or EPG caches.
5. When explicitly stopped, HLS clients with a stale playlist may get 404 for old segments: this is acceptable only after the stream is genuinely shut down, not while OnDemand grace or active HLS sessions are in effect.
6. Prefer generation-aware cleanup with rename-to-private-trash after final close followed by deletion; do not allow old cleanup to erase a new generation's freshly created files.
7. Add simulation tests covering Online Start/Stop/Start, OnDemand cold start, media retention/disconnect grace, post-idle stop, ABR variants, CMAF `init*.mp4`, failover preserved manifests, archive-enabled retention, and concurrent stop/restart.

## Operational diagnostics (read-only)

    du -sh /tmp/dvbstreamer5-hls 2>/dev/null
    find /tmp/dvbstreamer5-hls -maxdepth 2 -type f -printf '%h\n' 2>/dev/null | sort | uniq -c | sort -nr | head -30
    journalctl -u dvbstreamer5 --since '20 minutes ago' --no-pager | grep -E 'ONDEMAND DEACTIVATE|NATIVE HLS|NATIVE CMAF|NATIVE STREAM RELAY STOP'

Do not run `rm -rf` on an active server: OnDemand HLS can be idle now and resume on the next real client request.
