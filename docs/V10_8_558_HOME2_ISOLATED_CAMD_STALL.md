# DVBStreamer5 V10.8.558 — recover proven-media TSDecrypt CAMD stalls sooner

## Evidence from 2026-11-17 home2 (V557)
After sustained 5-second OSCam-mini ECM timeouts across multiple services, the
TSDecrypt service `sat-1794213237299-142-0` stopped producing clear video
PES, although encrypted input continued. At 08:27:57 CET the V557 child-only
watchdog restarted it (reason=camd_blackout); the child changed ECM PID
7937 -> 6657 and verified clear video PES at 08:28:22. This was a genuine
recovery without restarting DVBStreamer5, OSCam-mini or sibling services.

The upstream reader still logged mixed `found` and `timeout (5000 ms)`
results across SID 0B54/1260/025F/1272/0B5D, so local media recovery must
not be mistaken for a fix for external Newcamd latency. The remote reader's
capacity, per-service access and queuing remain under investigation.

## Changes
- CAMD blackout with a **previously valid clear PES** recovers its
  individual TSDecrypt worker after **90 seconds** of continuous no-clear-media
  evidence (was 180s), if encrypted input and child TS output remain active
  and a CAMD error is recent.
- Never-proven/cold services keep the 180-second grace. Valid PES progress
  cancels the condition; input absence and manually stopped tiles do not
  cause a restart. Host backoff and deterministic 0-27-second jitter remain.
- A shared in-process gate limits CAMD blackout restarts to **one per
  configured reader every 30 seconds**, to avoid flooding a loaded OSCam
  reader with simultaneous logins. Unrelated readers have independent gates.
  The gate retains only reader identifier and monotonic next-allowed time,
  never ECM/CW data or passwords.
- No changes to OSCam-mini `clienttimeout=5000`, authorization behavior,
  encryption/decryption algorithms, or PMT/ECM PID selection.

## Tests
`tests/test_tsdecrypt_recovery.cpp`: 90-second threshold, no restart when
clear PES resumes, cold startup grace, reader-budget isolation/cooldown.
Regular CTest, media/failover emulation and portable CI remain required.

## Production verification
Monitor `TSDECRYPT DIAG` sampled over several minutes, including cumulative
`clear_media_pes_encrypted_pid`, `video_pes_encrypted_pid`,
`camd_blackout_restarts`, `ecm_pid`, and `camd_card_denied`. A verified
live recovery requires advancing PES following child restart, not just a
CAMD successful login. Examine actual playback on an authorized service.

Do not deploy until GitHub Actions passes, and back up current home2 V557
before installing. Service update/restart is a maintenance-window operation.
