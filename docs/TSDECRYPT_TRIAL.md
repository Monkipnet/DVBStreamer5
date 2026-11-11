# DVBStreamer5 V10.8.463 — TSDecrypt per-tile CA backend (experimental)

The plugin `ca-plugins/dvbstreamer5-ca-tsdecrypt.so` is opt-in per CAM client;
Newcamd, OSCam-mini and DVB-CI remain unchanged. The external `bin/tsdecrypt`
executable is included in portable bundles only when the packager is given
`--tsdecrypt-binary /path/to/tsdecrypt`. For current builds, use
`bash scripts/make_portable_linux_bundle.sh --build-dir build --tsdecrypt-binary /tmp/dvbstreamer5-tsdecrypt/tsdecrypt --archive`.
No production service restart is needed to compile or package the bundle.

## Safe ECM/CW monitoring (V10.8.460)

After selecting the experimental TSDecrypt backend for **one test tile**,
inspect these key-free counters:

```bash
journalctl -u dvbstreamer5.service --since '-5 min' -o cat --no-pager |
  grep 'TSDECRYPT DIAG' | tail -n 30
```

- `ecm_reports`: number of successfully parsed periodic `-H 10` reports;
  distinguish unavailable telemetry (`ecm_reports=0`) from a real zero ECM
  count. In V461, duplicate and completed ECM counts are not restricted to the
  number of unique requests in the same interval; they are independent counts.
- `ecm_received`, `ecm_processed`, `ecm_duplicates`: accumulated numeric
  counts from **TSDecrypt's own periodic `-H 10` reports**, not host-side guesses.
  These can remain zero until the first report interval.
- `cw_valid_events`: the number of **first-valid-CW status transitions**
  reported by the external TSDecrypt process with `-K` active. This is
  **not** a count of every new CW, since upstream suppresses detailed per-CW
  logs to avoid leaking key material. `cw_warnings` tracks emitted no-CW
  warnings. No CW bytes, ECM payloads or CAMD credentials are logged.
- `input_scrambled`: scrambled input TS packets; `output_null`: output
  padding/NULL TS packets.
- `output_clear_encrypted_pid`: clear output packets on PIDs that have
  carried scrambled input; not necessarily valid media.
- `video_pes_encrypted_pid`, `audio_pes_encrypted_pid`: plausible **clear
  video/audio PES starts on previously scrambled PIDs**. These are stronger
  evidence than a cleared scrambling bit, but do **not** prove a successful
  media decode or functional audio/video.
- `csa_alt_verified=no`: verification of iCAM/CSA-ALT is deliberately NOT
  claimed. CAID 098D is a hint but does not uniquely identify CSA-ALT.
  A valid CW response alone does not establish CSA-ALT support.

The stream CA status shows `TSDECRYPT_WAITING_ECM`,
`TSDECRYPT_ECM_NO_PES`, `TSDECRYPT_CW_SIGNAL_NO_PES`,
`TSDECRYPT_PES_EVIDENCE`, or `TSDECRYPT_EXITED` with key-free
`ECM`, `CW_OK`, `V_PES`, `A_PES` counters. For CAID 098D it also
reports `CSA_ALT=UNVERIFIED`. No status claims successful decryption
based solely on a CAMD connection, received ECM/CW, or clear scrambling bits.

The child process stderr is consumed by an allowlisted parser and never
relayed verbatim to the main journal; `-K` remains enabled.

## Important limitations

This remains an experimental bounded asynchronous stdin/stdout TS processor.
Output can lag, contain NULL TS or be suppressed while waiting for CAMD.
The external process is not automatically restarted after failure and
does not fall back to Newcamd. No parallel dual-ECM selection is implemented.
Dedicated A/B tests with video/audio decoding and continuity/PCR checks are
required before serving subscribers. Upstream CAMD credentials are still passed
via command-line arguments, so use dedicated credentials and restricted hosts.
Do **not** paste `pgrep -a`, `ps -ef` or `/proc/<pid>/cmdline` containing
CAMD credentials into public logs or chats.

---
# TSDecrypt A/B integration trial — DVBStreamer5 V10.8.456

The earlier standalone trial remains available; the new experimental per-tile backend described above is also present. Neither implementation replaces the default Newcamd backend.

## Purpose

Compare output produced by upstream TSDecrypt against the existing DVBStreamer5+OSCam-mini pipeline for a *single authorized test service*. This can help isolate TS/CSA parsing from CAMD server delays without restarting DVBStreamer5 or changing production tiles.

Upstream TSDecrypt supports VideoGuard/NDS, CAMD NEWCAMD and CS378X, a manually selected ECM PID (`-X`), and MPEG-TS UDP/file input/output. Upstream TSDecrypt does **not** provide a verified parallel dual-ECM mode.

See: https://github.com/gfto/tsdecrypt

## Requirements

- Install `tsdecrypt` separately in the test environment. This repository **does not download, build, or bundle** it automatically.
- Use an independently routed MPEG-TS test feed that does not interfere with production multicast routing. The input must include PAT/PMT/ECM.
- Use a unique output port and a separate test player.
- Obtain proper authorization for the CAMD service; existing OSCam-mini or a different compatible CAMD server is still needed for CW.
- Avoid running on a shared host: upstream TSDecrypt receives CAMD credentials on the command line, which other users may see in process listings.

## Preflight (does not start a process)

```bash
bash scripts/tsdecrypt_trial.sh \
  --input 239.10.1.1:5000 \
  --output 127.0.0.1:56000 \
  --service-id 401 \
  --caid 098D \
  --camd-server 127.0.0.1:12001
```

When ready to test, set `TSDECRYPT_CAMD_USER`, `TSDECRYPT_CAMD_PASS`, and `TSDECRYPT_CAMD_DES_KEY` in a protected test shell, then append `--execute`. For NEWCAMD the DES key has exactly 28 hex digits; for CS378X no DES key is needed. Do **not** include secrets in logs, shell history, or bug reports.

To fix a specific ECM PID, add `--ecm-pid <decimalPID>`. Do not confuse the original DVB service ID with the hexadecimal fields in OSCam log messages.

The trial runs until stopped with Ctrl+C; **no production DVBStreamer5 service is stopped or restarted**.

## Still needed for full integration

1. Native managed per-tile selection with opt-in-only backend and separate worker lifecycle.
2. Feeding each selected service its own complete MPEG-TS with the correct PSI, ECM and EPG handling.
3. Port-ownership/collision checks, health monitoring, secure CAMD credential delivery, and automatic fallback to current CA path.
4. Optional dual ECM design, explicitly separate from upstream TSDecrypt capabilities.
5. A/B media validation, tests and production rollback.

Do not mark TSDecrypt as an implemented in-tile CA engine until these steps are completed and verified.

## V10.8.462 — experimental iCAM-aware external TSDecrypt build

V461's externally supplied TSDecrypt binary used FFdecsa, without carrying
the VideoGuard/iCAM ECM mode into its CSA key schedule. New script
`scripts/build_tsdecrypt_icam.sh` copies existing upstream TSDecrypt sources
into an isolated temporary directory, patches ECM mode propagation to the
CAMD reply, then builds with DVBStreamer5's vendored ECM-aware static
`libdvbcsa`. It uses `dvbcsa_bs_key_set_ecm()` for nonzero modes and the
bitslice decrypt API for even a single iCAM packet. Unmodified source,
installed TSDecrypt, and the working Newcamd plugin remain untouched.

From the source root on the build host:

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
bash scripts/build_tsdecrypt_icam.sh /tmp/dvbstreamer5-tsdecrypt build
bash scripts/make_portable_linux_bundle.sh --build-dir build \
    --tsdecrypt-binary build/tsdecrypt-icam/tsdecrypt --archive
cp -a dist/DVBStreamer5-linux-x86_64.tar.gz \
    dist/DVBStreamer5-10.8.462-linux-x86_64.tar.gz
sha256sum dist/DVBStreamer5-10.8.462-linux-x86_64.tar.gz \
    > dist/DVBStreamer5-10.8.462-linux-x86_64.tar.gz.sha256
```

**Not yet runtime verified:** Before deploying any production tile, validate
one authorized test SID and its video/audio PES, actual playback, and CW
changes. A valid CW or cleared scrambling bit does not prove decryption.
Keep the standard Newcamd backend as the per-tile rollback option.

## V10.8.463 — TSDecrypt per-service supervision

V462 showed working iCAM/CSA-ALT output on two test services, but the external
TSDecrypt child could exit or stop producing valid media. The host plugin
previously set `exited=true` and did not restart the child. V463 adds an
opt-in-backend-only watchdog for each service:

- Restart after unexpected child exit or a broken input/output pipe.
- Restart after 20 seconds without child stdout while encrypted input is
  active (after 30 seconds of process startup).
- Restart after 45 seconds without recognizable clear media PES following
  successful PES output, or 70 seconds after startup without any such output.
- Do not restart when encrypted input is absent or idle.
- Clear stale input/output TS queues and prior CW/PES health markers on restart.
- Use increasing 1/2/4/8/16/30-second retry delays, resetting after one
  minute of healthy output.
- Keep detailed keys, raw ECM bytes, CAMD passwords and child stderr out of
  application logs. Only `TSDECRYPT RECOVERY` with non-secret reason/counts
  and `TSDECRYPT DIAG input_drops= output_drops= restarts= recovering=`
  are emitted.

The normal Newcamd and OSCam-mini backends are not modified. Restart is
local to a single TSDecrypt service and does not restart the DVBStreamer5
process or other tiles. Status evidence expires after 15 seconds instead
of carrying a stale lifetime PES-success value. Recovery cannot resolve
a continuously failing CAMD server, bad ECM source or unsupported channel;
inspect the reason and per-tile error counters before further changes.

The pure recovery policy is covered by `ctest --test-dir build -R
tsdecrypt_recovery --output-on-failure`. Runtime supervision needs a real
one-tile home2 test, including abrupt child failure, and has **not yet
been verified on the server**.

## V10.8.467 — concurrent streams: CAMD attribution and reconnect staggering

Each TSDecrypt service uses its own OS process and Newcamd connection.
When more than two or three services are active, do not assume that a
CW dropout is a CSA-ALT algorithm failure. Check the key-free counters
`camd_connect_attempts`, `camd_connected`, `camd_connect_errors`,
`camd_reconnects`, `camd_no_cw`, and `camd_ecm_queue_overflows`
alongside `input_drops`, `output_drops`, and `restarts`.
No secret, ECM payload, code word or raw CAMD log line is printed.

The V463+ supervisor now gives the child's built-in CAMD reconnect logic
up to 180 seconds to recover when recent CAMD errors are observed and the
child is still writing TS. Unexpected child exits and stopped output retain
the existing restart policy. Host retry delays are deterministically
staggered by 0–1800 ms per stream so several channels do not reconnect
at exactly the same instant.

**This is mitigation and diagnostics, not proof of the failure cause.**
If the installed binary is V462, host-side watchdog is not involved.
Look for OSCam/reader ECM capacity, multiple Newcamd sessions per account,
missing CW events, or input/output queue drops, and test on a non-production
channel before activating four or more services.

## V10.8.468: nine-channel timeout incident and classified Newcamd errors

The V467 home2 nine-channel trial showed OSCam-mini logging 1,887 ECM
timeouts and 1,814 successful ("found") ECM results in thirty minutes,
with repeated TSDecrypt `media_stalled` recoveries. A configured 5000 ms
client timeout was observed. Host transport queues had no drops; CPU/memory
OOM was not observed. These event counts are NOT percentages of unique ECMs.

The single `camd_protocol_errors` counter from V467 conflated Newcamd
read failures, late `msg_id` replies, card decode denials, checksum failures,
unexpected responses and login errors. V468 adds these **key-free** subtypes:

- `camd_read_errors`: failure to read a Newcamd reply
- `camd_bad_msg_id`: delayed/unsynchronized reply identifier
- `camd_card_denied`: server explicitly could not decode the channel
- `camd_checksum_errors`: invalid Newcamd checksum
- `camd_unexpected`: unexpected Newcamd return length/status
- `camd_login_errors`: authentication error
- `camd_protocol_errors`: unchanged aggregate across those and other errors

Only allowlisted event markers are classified. Raw stderr, credentials,
control words, and ECM payloads must never be forwarded to the journal.

**Supervisor mitigation:** while an active child keeps reporting CAMD or
ECM queue failures, the host no longer forces a `media_stalled` restart.
This avoids creating more simultaneous Newcamd logins on an already failing
reader. A genuine child exit or stalled stdout remains restartable. After
60 seconds with no new CAMD errors, media-stall recovery is reenabled.

This does not increase OSCam-mini capacity or repair remote ECM processing.
Diagnose reader load and the new categories before attempting concurrent
9-channel production decoding. Avoid raising `clienttimeout` blindly.
