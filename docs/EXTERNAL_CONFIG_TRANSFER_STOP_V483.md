# DVBStreamer5 V10.8.483 — Cesbo Astra / Flussonic migration and forced Stop

## Where to find it

Open **Menu → Administration → Configuration**.

Export format dropdown:
- DVBStreamer5 (full JSON backup, includes subscribers and sensitive secrets).
- Cesbo Astra (JSON `make_stream`).
- Flussonic (UTF-8 `.conf` stream blocks).

Import format dropdown:
- DVBStreamer5 or TVStreammer5 (existing complete import and preview).
- Cesbo Astra (JSON exported from `/api/config`).
- Flussonic (the `/etc/flussonic/flussonic.conf` stream block syntax).

Click **Preview import** to inspect stream count, skipped entries and
incompatible directives. No changes are applied until **Apply import**.

### Migration scope and safety

The external converters support SPTS streams with a primary input,
one backup URL, and explicit UDP/RTP push/output destinations.
Additional UDP/RTP outputs are copied when possible.
Generated streams default to `auto_start=false` and never replace
existing streams on import. Existing subscribers, server credentials,
HTTP listener, CAM clients, XMLTV, transcoders and running pipelines
remain untouched. Existing matching name+input streams are skipped.

Not currently migrated: Astra MPTS, DVB tune groups, PID mappings and
Astra-specific input modes; Flussonic templates, DVR/archive and
auth; Flussonic API server_config JSON; any DRM/CA backend credentials,
SDT/EPG mapping, codec profiles or dynamic output authentication.
Review the generated configuration before production use.
Imported SPTS outputs supported here: UDP unicast/multicast and RTP.
Non-UDP/RTP configurations are exported with a warning / omitted.

Do not share exports publicly: channel input URLs may include HTTP
userinfo/query tokens, and a full DVBStreamer5 JSON backup contains
private CAM credentials and panel passwords.

Supported Cesbo schema:
https://astra.cesbo.com/en/api/stream/

Flussonic stream block and input syntax:
https://flussonic.com/doc/fms/live/internals/

### Safe stop escalation

A Stop button queues an operator stop and prevents OnDemand/failover
restarting that stream while teardown is pending. It does **not**
optimistically mark the stream stopped.

When the stop remains pending after nine seconds, the web UI sends
`POST /api/stop-stream` with:

```json
{"id":"exact-stream-id","force":true}
```

This can also be requested with the new **Force stop** button while
the stream is stopping. The force handler immediately sends
cancellation signals to the selected tile's relay and HLS/SRT/RTSP/RTMP
input and shuts down that tile's HTTP sockets. Three independent
async-stop workers prevent a single failed source from blocking the
entire stop queue. No other stream is intentionally stopped or
restarted.

Because DVBStreamer5 uses **in-process C++ media workers**, no safe
operating-system API can forcibly kill an individual arbitrary worker
thread and reclaim its memory. If a codec or external I/O operation
ignores interruption, the tile can still remain `stop_pending`.
This release deliberately does NOT use `pthread_cancel`, `SIGKILL`
on the whole server, detached worker destruction, or fraudulent
"Stopped" status. In that rare case inspect the journal for the
blocking stage and preserve the remaining active channels.

### Build and checks from the source root

```bash
git switch main
git pull --ff-only origin main
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
node tests/web/test_external_config_transfer_ui.js
node tests/web/test_stream_toggle_stop_policy.js
ctest --test-dir build -R external_config_transfer --output-on-failure
ctest --test-dir build --output-on-failure
bash scripts/make_portable_linux_bundle.sh \
  --build-dir build --binary build/DVBStreamer5 \
  --output-dir dist/DVBStreamer5-10.8.483-linux-x86_64 \
  --with-oscam --archive
```

For servers using TSDecrypt, package the previously verified native
TSDecrypt binary explicitly with `--tsdecrypt-binary`, or first run
`scripts/build_tsdecrypt_icam.sh` and test the output.

No production restart is necessary to perform a source checkout,
compilation or portable packaging. Install only after all tests pass.
