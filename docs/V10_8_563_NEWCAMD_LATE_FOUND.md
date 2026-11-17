# DVBStreamer5 V10.8.563 — optional local Newcamd iCAM late-FOUND recovery

## home2 evidence
Five ECMs returned FOUND at 5060, 5826, 5835, 5870 and 6108ms.
Client timeout is 5000ms; queueing was 0–1ms, and all five late replies
had late=1, found. The root cause of the upstream delays is unknown
and cannot be fixed solely at the client.

## Targeted mitigation, OFF by default
Set only `DVBSTREAMER5_OSCAM_NEWCAMD_GRACE_MS=1500` for oscam-mini.
Allowed values: decimal 1..1500; invalid/unset/0 => no grace.
The extension applies only to client ECMs with CAID `098D` arriving
by Newcamd and forwarded by a Newcamd proxy reader.

The client deadline becomes 5000+1500=6500ms for those requests.
The old timeout remains unchanged for all other CAIDs, incoming
protocols, reader types, and requests that have no eligible reader.

The same per-request effective deadline drives OSCam's receiver-copy
expiry, accepted-answer age limit and reader duplicate-suppression
cache cutoff. The existing normal ECM cache retention exceeds the
maximum 1500ms grace and is left unchanged. Late found CWs are *not*
replayed into future unrelated ECMs; readers are not reconnected.

This is not a network/remote-reader speed fix and does not guarantee
continuous video: a downstream CAMD client or the active cryptoperiod
could expire before a 6.5-second answer. Measure actual PES stability.
The timeout extension also increases pending-task residency slightly.

## Additional diagnosis
V563 records `send_call_ms` for the local Newcamd send call and shifts
`upstream_ms` to start after successful send-call completion. Values
from previous V560–V562 `upstream_ms` included this send-call duration.

## Safe rollout (maintenance window; oscam-mini restart is disruptive)
```bash
sudo mkdir -p /etc/systemd/system/oscam-mini.service.d
sudo tee /etc/systemd/system/oscam-mini.service.d/20-newcamd-grace.conf >/dev/null <<'EOF'
[Service]
Environment=DVBSTREAMER5_OSCAM_NEWCAMD_GRACE_MS=1500
EOF
sudo systemctl daemon-reload
sudo systemctl restart oscam-mini.service
journalctl -u oscam-mini.service --since '-15 min' -o cat --no-pager |
  python3 /opt/DVBStreamer5/scripts/diagnose_oscam_ecm_latency.py
```

Rollback: remove only `20-newcamd-grace.conf`, daemon-reload,
restart oscam-mini in a maintenance window.
No default OSCam-mini config or credentials are overwritten.

## Tests
CTest builds the regression target even in Release mode; assertions must
remain active under -DNDEBUG. A deterministic fixture asserts that the five
observed home2 delays (5060, 5826, 5835, 5870 and 6108ms) all fail under
the 5000ms baseline and fit within the opt-in 6500ms deadline.
`oscam_newcamd_grace` checks scope, malformed env values, max
bound, overflow, 5000ms baseline and reader expiry boundaries.
Main GitHub Actions builds the actual OSCam-mini source and runs CTest
and media/portable checks.
