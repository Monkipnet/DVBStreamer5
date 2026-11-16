# DVBStreamer5 V10.8.556 — bounded TSDecrypt recovery during sustained Newcamd failures

Date: 2026-11-16. Repo: Monkipnet/DVBStreamer5, main.

## home2 evidence and limitations

On V549 home2, OSCam-mini (local Newcamd server) logged ECM TIMEOUT
after approximately 5001 ms across multiple SIDs in the same period.
Most responses succeeded after OSCam-mini and DVBStreamer5 were restarted,
but intermittent timeouts persisted until 15:18:27 CET. Multi-SID network,
reader or upstream capacity bottlenecks cannot be proven or eliminated
by changing the DVBStreamer5 watchdog. Do not blindly increase OSCam
clienttimeout=5000 or restart OSCam-mini for one impaired service.

A Newcamd negative CW reply does not necessarily distinguish server-side
timeout from a missing entitlement. The TSDecrypt camd_card_denied aggregate
may therefore reflect more than incorrect entitlement.

## Two source-level reliability defects fixed

1. The patched TSDecrypt ECM PID policy permitted rotating to alternative
   PMT candidates on every transport timeout (up to one/15 sec), even after
   recent successful CW on the current PID. V556 now retains a proven PID on
   a ret=-1 transport failure for 30 seconds since last success; a stale,
   unproven, genuinely denied or explicitly forced PID retains the existing
   fallback behavior. This prevents needless PID thrashing during intermittent
   Newcamd delays without pretending a stale PID can never require fallback.
   The external tsdecrypt binary is rebuilt by the CI packager because its
   patch fingerprint includes TsDecryptEcmPidPolicy.h.

2. The host TSDecrypt watchdog suppressed media_stalled recovery forever if
   CAMD errors kept arriving: a child might keep writing encrypted packets
   without ever delivering usable clear PES. V556 waits for at least **180
   seconds without clear media PES**, **180 seconds of child uptime** and
   active encrypted input/output before permitting a per-service
   `camd_blackout` recovery. The existing CAMD grace, failure backoff,
   per-stream identity, exit handling, and stop gates remain in place. Blackout
   reconnects are deterministically staggered by up to 27 seconds across
   streams to reduce simultaneous reader logins. A newly spawned child is
   not eligible for the next blackout recovery for another 180 seconds.

New allowlisted, key-free diagnostic `camd_blackout_restarts` is visible in
`TSDECRYPT DIAG` and the backend status JSON. Never expose CAMD log lines,
credentials, raw ECMs or control words.

## Regression and deployment

`tests/test_tsdecrypt_ecm_pid_policy.c` now verifies intermittent timeout
stickiness and stale-PID fallback. `tests/test_tsdecrypt_recovery.cpp` verifies
short CAMD outages, long blackout recovery, fresh PES, child exit precedence,
absent source input and deterministic reconnect staggering. Both tests are
already registered in CTest and run in GitHub Actions along with the complete
25 media / 32 application CTest suites.

The portable bundle MUST include the newly rebuilt TSDecrypt executable,
plus OSCam-mini, versioned archive and SHA256. A host-only program upgrade
without TSDecrypt would omit the ECM PID policy improvement.

Runtime check on a test or maintenance-window host:
```bash
journalctl -u dvbstreamer5.service --since '-60 min' --no-pager -o cat |
  grep -E 'TSDECRYPT RECOVERY|TSDECRYPT DIAG|TSDECRYPT PID CACHE FALLBACK' | tail -90
```
Monitor source media, clear PES progress, reader timeout incidence and
the new blackout counter. This addresses avoidable local PID churn and
permanently stuck child sessions. **It cannot guarantee recovery while
the remote Newcamd provider fails to answer ECM requests.**
