# DVBStreamer5 V10.8.457 — TSDecrypt per-tile CA backend (experimental)

The in-tile backend is now present as a dynamically loaded plugin `dvbstreamer5-ca-tsdecrypt.so`, built by CMake and packaged in `ca-plugins/`. The old standalone `scripts/tsdecrypt_trial.sh` remains available for A/B testing.

## Opt-in selection on one DVB tile

1. Install **upstream TSDecrypt** at `/usr/bin/tsdecrypt` separately. DVBStreamer5 does not build or ship the TSDecrypt executable.
2. In **Menu → Newcamd / OSCam CAM clients**, add a **new dedicated CAM client** (never overwrite the existing working Newcamd client).
3. Select backend **TSDecrypt (external, experimental)** for this new CAM client and configure host, port, user, password, DES key, CAMD protocol, CAID and optional decimal ECM PID (0 = auto).
4. In a **single test DVB tile**, choose **iCAM / Newcamd** and select the newly created TSDecrypt CAM client. The backend is selected by the CAM client; other tiles stay on their existing CA backend.
5. Save and restart **only that test tile**. If the selected plugin is missing, `/usr/bin/tsdecrypt` does not exist, or the DVB service ID is missing, the tile start fails without silently switching the CAM service to another backend.
6. Verify `CA backend service reserved ... backend=tsdecrypt`, `TSDECRYPT_ACTIVE` or `TSDECRYPT_WAITING_CW`, and decoded A/V packet counters. Stop the test tile and select the previously working CAM client to roll back.

**Important limitations.** This is an early experimental per-tile integration, not a production-ready/validated full integration. TSDecrypt processes a bounded asynchronous TS pipe (not packet-by-packet synchronously), so its output may lag input, or emit NULL TS packets while awaiting output. Verify continuity, PAT/PMT, EPG and AV sync before using it for subscribers. Loss of TSDecrypt output does **not** automatically switch to the internal Newcamd decoder. A tile restart is required to recover an exited subprocess. No parallel dual-ECM logic exists; only one manually selected ECM PID is supported. Never enable on an operational channel without isolated media testing.

CAMD credentials are passed as TSDecrypt command-line options (a limitation of the upstream CLI), which can be exposed in process listings. Use a dedicated non-privileged test server and restricted OS permissions, not shared accounts.

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
