# DVBStreamer5 Stage 9 — live HTTP / OpenH264 hotfix

Date: 2026-10-01

This hotfix addresses two failures observed with real live streams:

- `native HTTP input failed: HTTP transport error: Failed to read connection`
- `OpenH264 decode failed: 16`

## Native HTTP live input

`NativeUdpRelay` no longer treats a transient HTTP EOF, read timeout, reset, or
upstream connection close as a terminal stream failure. The live input now:

- reconnects automatically while the relay is running;
- uses a bounded 1..5 second retry backoff;
- retries quickly after a connection that delivered transport data;
- enables HTTP keep-alive for the live input client;
- keeps the relay alive instead of forcing the service OFFLINE;
- never forwards 3xx/4xx/5xx response bodies into the MPEG-TS queue;
- keeps redirect/access-key protection from Stage 9.

A media-core regression test now verifies that an HTTP MPEG-TS server can close
and be reconnected without stopping the relay.

## OpenH264 live-start / reconnect synchronization

The H.264 decoder now recognizes Annex-B SPS/PPS/IDR NAL units and caches the
latest SPS/PPS. When a service is joined in the middle of a GOP, pre-roll video
is ignored until a clean random-access point can be decoded. Cached SPS/PPS are
prepended to an IDR when needed.

OpenH264 bitstream/live-transport states such as `dsNoParamSets` are handled as
recoverable conditions instead of immediately failing the whole transcoder.
Reference/bitstream loss after a live transport discontinuity forces a fresh
SPS/PPS + IDR resynchronization. Logic/API failures remain fatal.

A native-transcoder regression test was added for the exact live-join sequence:
non-IDR pre-roll first, then SPS/PPS + IDR recovery.

## Validation performed in the ChatGPT build environment

The HTTP/media-core subproject was rebuilt and tested after the changes:

```text
1/1 media_core_tests ... Passed
100% tests passed
```

The production Stage 9 native-transcoder test requires the project's vendored
static codec prefix (OpenH264/libde265/Kvazaar/FDK-AAC/PL_MPEG). The sandbox did
not have those vendored codec sources available, so the new OpenH264 regression
must be executed in the user's WSL production tree together with the normal
Stage 9 `6/6` test run.

No GStreamer or FFmpeg dependency was added.
