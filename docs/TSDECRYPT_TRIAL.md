# TSDecrypt A/B integration trial — DVBStreamer5 V10.8.456

This stage adds an **optional, isolated** external TSDecrypt runner. It does not replace the built-in DVBStreamer5 CA backend and does **not** add a per-tile selector or dual ECM yet.

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
