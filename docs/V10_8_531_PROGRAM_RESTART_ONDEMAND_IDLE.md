# DVBStreamer5 V10.8.531 — panel program restart and OnDemand idle stop

Date: 2026-11-14. Repo: `Monkipnet/DVBStreamer5`, `main`.

## Operator report and unchanged V530 behavior

- V530 fixed regular approximately one-second freezes at each HLS media-segment boundary; retain its 3-segment OnDemand HLS startup prefill unchanged.
- New reports: the **Restart program** control stopped restarting the daemon, and satellite **OnDemand** tiles can remain in the running state after the player disconnects, while IP OnDemand shuts down.
- No command output or actual HTTP restart response was provided for the restart failure, so the old detached-thread/systemd-cgroup shutdown race is a **code-level reliability defect**, not a runtime root cause proven by logs.
- Likewise there was no post-disconnect /api/state or stream_sessions capture for an affected satellite tile. V531 closes two code-level routes that can prevent idle cleanup; field verification is required.

## A. Web program restart

Previously `HttpServer::handleRestartProgram()` started a detached thread
inside the daemon systemd cgroup, slept 500 ms, then ran
`systemctl --no-block restart dvbstreamer5.service`.
The API unconditionally returned `{"result":"restarting"}` even if
thread creation or `systemctl` failed. Triggering systemd Stop may
terminate the task before its restart request is fully dispatched.

V531 changes:

- `src/http/HttpServerConfigActions.inc`: schedule a **transient
  systemd timer/service managed by PID 1** using
  `/usr/bin/systemd-run --quiet --no-block --on-active=2s
  --unit=dvbstreamer5-panel-restart-<monotonic-nonce>
  /usr/bin/systemctl --no-block restart dvbstreamer5.service`.
  The command is static apart from numeric nonce. Because scheduling
  is independent of `dvbstreamer5.service` cgroup, systemd can execute
  it after the HTTP response has been returned and the original process
  is gone.
- `src/HttpServer.h`: restart handler returns `bool` and a detailed
  scheduling error.
- `src/http/HttpServerTransport.inc`: REST endpoint
  `POST /api/restart-program` requires an authorized admin panel
  session or Basic authorization; a stream-session API key may not
  authorize daemon restart. Returns HTTP 503 and JSON error when
  `systemd-run` cannot schedule, instead of false success.
  A successful response means **scheduled**, not that the new process
  is already running; verify systemd MainPID after 2 seconds.

This is a **full service-wide restart**: it interrupts every running
tile, unlike ordinary one-tile Stop. Use in a maintenance window.

## B. Satellite OnDemand after closing VLC

Prior code kept `onDemandStartedStreams` populated only by activation
through `ensureOnDemandStreamParallel()`. If an operator started an
OnDemand-configured DVB tile through panel/API, the stream could be
active but absent from this set, so `monitorOnDemandStreams()` would
never stop it. V531 monitors **all actually active** streams whose own
`state->config.activationMode` is `ondemand`, seeding missing activity
from the original stream generation `startedAt` rather than granting
a new 30-second delay on every monitor tick.

Second problem: `ensureOnDemandStreamParallel()` previously refreshed
`onDemandLastActivity` on every request, including an already-active
HLS playlist/HTTP startup probe. A player or automatic viewer
periodically fetching `.m3u8` *without fetching real media* could
indefinitely prevent shutdown. V531 no longer refreshes the running
stream's idle timestamp for `source=hls` or `hls-startup-wait`
requests. Initial requests can still start inactive streams. The
`adHocSessions` record from a successfully delivered HLS media
segment (75-second bounded grace from V529), live TCP HTTP TS and
preview sessions, and genuine new requests still preserve actively
watched media.

- IP/UDP OnDemand is unchanged by DVB-specific shared-input cleanup;
  the same safer viewer accounting applies equally to HLS sessions.
- On idle, only the channel's `stopStream(id)` is called;
  `sharedDvbInputs.unsubscribe(id)` removes just that DVB service.
- **Shared DVB Warm Standby is NOT a running tile**. With no service
  subscribers, `SHARED DVB STANDBY ... subscribers=0` may keep frontend
  LOCK and MPTS reader ready for a later request; it must have no
  channel CA, transcode or HLS pipeline. Other services on the same
  transponder stay alive.

## Regression and release

- `tests/web/test_restart_ondemand_v531.js` checks timer ownership,
  error reporting, privileged API gate, monitor discovery of manually
  started DVB OnDemand, no HLS manifest-based keepalive, and shared DVB
  subscription teardown.
- Test registered in both relevant sections of
  `.github/workflows/build-main-linux.yml`.
- App version `src/AppVersion.h` -> `10.8.531`.
- Source consistency checks passed; **full CMake/CTest, GitHub Actions
  and field behavior are not yet confirmed**.

## Build and verify from source root

```bash
git switch main
git pull --ff-only origin main
node tests/web/test_ondemand_activation_v528.js
node tests/web/test_ondemand_viewer_v529.js
node tests/web/test_ondemand_hls_prefill_v530.js
node tests/web/test_restart_ondemand_v531.js
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
(cd dist && sha256sum -c DVBStreamer5-10.8.531-linux-x86_64.tar.gz.sha256)
```

GitHub Actions `build-main-linux.yml` publishes the portable artifact
`DVBStreamer5-10.8.531-linux-x86_64`. Verify the run and SHA256, save
V530 portable/config/service unit backups and arrange a test maintenance
window before installing. Portable installer stops the service;
`/opt/DVBStreamer5/VERSION` must match the actual installed build.

## Restart check after installation

1. Capture old `MainPID`: `systemctl show -p MainPID --value dvbstreamer5.service`.
2. Trigger **Restart program** from the logged-in web panel once.
3. Verify journal contains `PROGRAM RESTART V531 SCHEDULED`; after
   a few seconds, systemd should show a **different nonzero MainPID**.
4. If scheduling fails, the panel/API should receive 503 JSON error;
   inspect `journalctl -u dvbstreamer5.service` and transient
   `dvbstreamer5-panel-restart-*.service/.timer`. Verify
   `/usr/bin/systemd-run` exists and PID 1 is systemd.
5. Recheck configured Online auto-start, OnDemand standby behavior,
   OSCam-mini and primary output ports.

## OnDemand shutdown check on home2 / test server

1. Take one satellite OnDemand tile. Open its HTTP/HLS output in VLC
   and verify real media (not only playlist GET).
2. Close VLC completely. Leave any unrelated channels running.
3. For HTTP TS, allow at least **30 seconds** after socket detach.
   For HLS, allow **75 seconds since the last successfully delivered
   segment**, or up to **120 seconds** if cold start never reached its
   first HLS media. A second actual viewer prevents shutdown.
4. Log
   `journalctl -u dvbstreamer5.service --since '-10 min'
   --no-pager -o cat | grep -E
   'ONDEMAND TRACK ACTIVE|ONDEMAND DEACTIVATE|NATIVE HTTP TS CLIENT detach|SHARED DVB subscriber removed|SHARED DVB STANDBY'`.
5. Expected sequence:
   `ONDEMAND DEACTIVATE ... reason=no-clients` ->
   `SHARED DVB subscriber removed stream=<id>`.
   If `SHARED DVB STANDBY` follows and the tile is inactive, this is
   intentional transponder prewarm and **not** an OnDemand leak.
6. If the tile is still active after 130 seconds, examine
   `/api/state` per-stream `active`, `stop_pending` and
   `stream_sessions` for genuine other viewers before forcing Stop.
   Do not retune the frontend or interrupt neighbor channels.
7. Retest IP HLS OnDemand and the smooth 4-second segment/VLC profile
   from V530, as well as normal Online HLS, HTTP and DVB-CA.

## Rollback

Restore the previously verified V530 portable archive, service unit
and backed-up configuration using the normal installer during a
maintenance window. Confirm `VERSION=10.8.530`, port listeners,
HTTP/HLS, DVB LOCK, CA/EPG and neighboring streams. Never use a
manual tuner retune to test OnDemand idle shutdown.
