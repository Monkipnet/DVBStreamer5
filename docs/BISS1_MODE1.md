# BISS-1 (Mode 1) decoder — DVBStreamer5 V10.8.475

Adds a separate, opt-in local CSA descrambling plugin
`ca-plugins/dvbstreamer5-ca-biss1.so`.

## Scope

- Supports **BISS-1 / Mode 1** only, using an operator-supplied
  fixed session word for feeds the operator is authorized to receive.
- No ECM, Newcamd, OSCam-mini, remote network service, key scanning,
  BISS-2 key management, RSA session-key exchange, or key search.
- Existing Newcamd, TSDecrypt, iCAM, DVB-CI+ and FTA paths are unchanged.
- Per DVB service choose a CAM client whose backend is `biss1`.
  A CAM client can be reused for several services only when they are
  authorized to use that **same** fixed CW. Different keys require
  separate CAM client entries.
- The plugin sees the already selected SPTS from a native DVB tile.
  It decrypts payloads of TS packets with scramble bits EVEN/ODD via
  standard libdvbcsa, preserving adaptation fields and packet length.
  A packet without a valid payload is never marked clear.
- The packet counter indicates packets processed, not confirmed video
  or audio decoder output. Verify real A/V playback separately.

## Key configuration

In Menu → CAM clients, add a client, choose backend
**BISS-1 (fixed CW)**, enter the authorized key, save, and select
that CAM client for the DVB tile using Access mode iCAM/CA.

The field accepts:

1. **12 hexadecimal characters** (6 bytes):
   the plugin inserts BISS checksum bytes after each group of three.
2. **16 hexadecimal characters** (8 bytes):
   both checksum bytes must already be valid.

Example **format only** (not a live feed key):
`010203040506` expands to `010203060405060F`.

Both TS scrambling parities use this fixed BISS-1 word. This is not
compatible with systems requiring independently changing Even/Odd
control words. Incorrect, all-zero or incomplete keys fail closed.

For an administrator managing the configuration directly:

```json
{
  "id": "biss-feed-1",
  "name": "BISS authorized feed",
  "max_services": 1,
  "backend_id": "biss1",
  "backend_config": "{\"mode\":\"biss1\",\"key\":\"REPLACE_WITH_AUTHORIZED_BISS_HEX\"}"
}
```

The `backend_config` field is the existing *JSON-encoded string*
format in the configuration file. Never paste production CWs into
issue reports, logs, tickets or public GitHub comments.

The UI uses a password input and does not insert the stored key
into the HTML value field. Leaving it blank retains the saved key
when editing an existing BISS client. Administrative configuration
and backups **still contain the BISS key**: protect them with
filesystem permissions, authentication and secure backups.
The backend status JSON never includes the CW.

## Build/test

```bash
git switch main && git pull --ff-only origin main
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
node tests/web/test_biss1_ui.js
test -s build/ca-plugins/dvbstreamer5-ca-biss1.so
bash scripts/check_ca_backend_plugin.sh build/ca-plugins/dvbstreamer5-ca-biss1.so
```

The portable packaging script collects all `dvbstreamer5-ca-*.so`
plugins from the build directory, so BISS-1 is included in
`ca-plugins/` when the full app is built.

As with other decoder changes, test on a non-production authorized
feed and maintain a backup of the previous portable installation and
systemd unit files. Never assume a successfully loaded key means
real decoding until verified on screen.

## Monitoring

The plugin reports `readers`, `services`, `packets`,
`descrambled_packets`, and `invalid_ts_packets`, without the key.
If packets remain scrambled, inspect the original BISS-1 session
word and TS PID/parity; never fall back to copying a CW from an
unrelated CA service.
