# DVBStreamer5 V10.8.552 — network input stall recovery, bounded program restart and PCR diagnostics

Date: 2026-11-16. Repository: Monkipnet/DVBStreamer5, branch main.

## Observed on home2 V549

During a 20-minute investigation (15:10–15:30 CET), five TSDecrypt/UDP
services produced 50 PCR resets: 31 on service 122, 11 on 132, 6 on 113,
1 each on 108 and 118. The largest group shared transponder identity
sat-1794213237299. The output PCR worker dropped queued TS packets
when detecting discontinuity or a jump greater than two seconds;
dropped_bytes grew from 11590952 to 12523244 while the input TSDecrypt
input_drops and output_drops stayed zero. The separate ECM/Newcamd
timeouts gradually stopped at 15:18; a cached ECM PID on service 122
fell back from 6674 to AUTO and PID 7186 was later verified.
Correlation is not proof that ECM caused PCR jumps.

## Runtime recovery

The V510 Auto Start supervisor recovered only inactive relay instances.
An active relay can retain its socket and tile active bit indefinitely
after upstream media disappears; it is not eligible for the original retry.

For **Online + Auto Start + network input only**, V552 records when the
InputLivenessPolicy has continuously declared input stalled.
After 120 seconds of sustained stalled state, the existing 3-lane
Auto Start worker tries to restart **only that tile** with the saved
configuration. Any renewed selected-media progress cancels the timer.
Operator Stop, pending Stop, OnDemand, and active source failover are
authoritative. A short live-input jitter interval does not restart.
DVB input is intentionally excluded: forcibly restarting a service
could disrupt other programs sharing the tuner/transponder.

A failed start remains under the existing bounded per-tile retry
schedule. This is a recovery policy, not a guarantee that a failing
source will resume or a cure for a permanently unavailable upstream.

## Full program restart

The privileged panel /api/restart-program continues to schedule an
independent systemd-run transient timer (V531), returning scheduled
status only. Portable/universal/legacy systemd install definitions
now explicitly use a **20-second TimeoutStopSec**, control-group
termination and SendSIGKILL=yes. Any child processes lingering
after SIGTERM must not indefinitely block the new main process.
Program restart interrupts *all* tiles. OSCam-mini remains a separate
unit and is not restarted by the panel's DVBStreamer5-only restart.
An update installer has its own service stop/start transaction.

## PCR handling

The source-PCR clock now ignores isolated backward timestamp jitter
under 1 ms when no discontinuity flag is present, without dropping
previously queued media. Actual discontinuity flags and larger jumps
retain the existing protective reset/flush. Each reset now reports
cause=discontinuity_flag|backward_jump|forward_jump, signed
pcr_jump_ms, dropped_packets and dropped_bytes. The legacy generic
RESET reason previously mixed all three cases, so earlier logs cannot
establish what type caused home2's 50 resets.

This is a conservative PCR mitigation and diagnostic enhancement, NOT
proof that the underlying upstream discontinuities are fixed.

## Tests

- CTest: udp_vbr_pcr_clock existing source interpolations and new
  sub-ms backward jitter / explicit discontinuity reason.
- Node: tests/web/test_runtime_recovery_v552.js.
- Existing Node cluster editing regression V541: remove a stale
  syntactic requirement that the V548 native spoiler implementation
  legitimately replaced.
- Main GitHub Actions Linux/portable workflow includes the V552 test.

## Verify on home2 after CI succeeds

1. Keep a known-good V549 archive/config backup; install a new
   version only in a maintenance window.
2. Observe a network-input Online/Auto Start tile while its source
   is unavailable for over 120 s; verify `AUTO START RECOVERY STALLED INPUT`
   then the same tile returns after source recovery, without disturbing
   other streams or restarting the full daemon.
3. Repeat on an OnDemand tile and a manually Stopped tile; neither
   may be resurrected.
4. With controlled PCR jitter under 1 ms, verify no RESET/drop.
   For flagged discontinuity/large jump verify protective reset and
   new cause/pcr_jump_ms/dropped_packets fields.
5. Trigger an authorized panel restart and verify old/new MainPID
   differs and service becomes active; record `systemctl show`
   KillMode/TimeoutStopUSec and check child cleanup.
6. Monitor live MPEG-TS continuity and decoder playback; CTest
   emulation cannot substitute for actual source/tuner tests.

Read-only diagnostics:

```bash
cat /opt/DVBStreamer5/VERSION
systemctl show dvbstreamer5.service -p MainPID -p KillMode -p TimeoutStopUSec
journalctl -u dvbstreamer5.service --since '-30 minutes' --no-pager -o cat |
  grep -E 'AUTO START RECOVERY|NATIVE INPUT HEALTH LOST|NATIVE UDP VBR PCR (RESET|STATUS|OVERFLOW)' | tail -100
```
