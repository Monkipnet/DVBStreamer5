# V10.8.474 — X Acceleration Codec softcam reliability review

## What the public Xaccel release history actually confirms

The published X Acceleration Codec (Xaccel) changelog mentions:

- v4.6.10: Newcamd iCAM support.
- v5.1.17: fixed loss of frames in SoftCAM decryption.
- v5.1.21: fixed incomplete TSDecrypt cleanup.
- v5.1.30: fixed too-short valid-CW waiting interval.
- v5.3.5: fixed parsing of SoftCAM CW.
- v5.2.20: fixed DVB SoftCAM decryption problems.

Source: https://www.xaccel-codec.com/docs/codec/releases.html

These are *release descriptions*, not implementation or evidence of a
particular TSDecrypt command line, queue design, ECM PID policy or codeword
cache. No proprietary source is copied or reverse engineered here.

## DVBStreamer5 independent adaptation

The experimental TSDecrypt backend already has bounded TS input/output,
worker restart protection, CW signalling and PES counts. V474 adds a
**display/telemetry-only** independent health state machine:

- `starting`: first 45 seconds without a valid clear PES;
- `reader_rejected`: recent CAMD error after the startup grace;
- `cw_signaled_no_pes`: CW reported but no clear PES;
- `awaiting_cw`: no CW reported and no clear PES;
- `clear_pes_recent`: matching scrambled input PID has clear
  MPEG PES evidence within the last 10 seconds;
- `clear_pes_stalled`: earlier PES evidence has stopped advancing.

These labels appear in `TSDECRYPT DIAG ... cw_health=...` and the
TSDecrypt backend JSON `cw_acquisition_state`. They are intentionally
not proof of full A/V decoding; they do not extend the CW validity,
automatically change ECM PID, or suppress the child watchdog.

This permits separating the three different home2 V471 observations:
SID 105/108 kept advancing PES despite occasional Newcamd card-denied
responses, while SID 132 had only intermittent PES and repeatedly rotated
ECM PIDs. A cached CW from SID 105 cannot be used for SID 132.

## CI reliability

The main Linux CI previously compiled only a subset of registered CTest
targets. Five tests therefore failed as `Not Run`, including the new
Shared CW Broker unit test. V474 builds all registered TSDecrypt and
CW Broker test executables before running CTest and explicitly checks the
web broker integration policy.

## Validation and deployment

Build from repo root:

```bash
git switch main && git pull --ff-only origin main
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
node tests/web/test_shared_cw_broker_policy.js
bash scripts/build_tsdecrypt_icam.sh /tmp/dvbstreamer5-tsdecrypt build
bash scripts/make_portable_linux_bundle.sh --build-dir build \
  --tsdecrypt-binary build/tsdecrypt-icam/tsdecrypt --archive
```

This change does not directly repair remote OSCam/reader refusals,
nor does it connect the external TSDecrypt process to the opt-in
in-process Newcamd CW Broker. Test one authorized channel at a time;
keep the previous portable installation and systemd units for rollback.
