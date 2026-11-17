# DVBStreamer5 V10.8.560 — opt-in OSCam-mini ECM latency tracing

## Motivation and evidence
On home2 V10.8.557, 10 and even 3 active iCAM services can experience
5000ms ECM timeouts. In one 120s sample, 71 timeout events occurred while
the single upstream TCP connection showed zero additional retransmissions
and an RTT of about 42–54ms. After restoring 10 services, another 120s
sample showed zero timeouts. This intermittent pattern does not prove
whether the delay is local queueing or upstream reader processing.

## Instrumentation
The OSCam-mini Newcamd path now tracks a **diagnostic sequential ID**
and monotonic timestamps for each incoming ECM. It is disabled by default.
Set environment variable `DVBSTREAMER5_OSCAM_ECM_LATENCY=1` on
`oscam-mini.service`, then restart OSCam-mini **only in an approved
maintenance window**. A restart disconnects all Newcamd clients momentarily.

When enabled, journal messages start with `DVB5 ECM LATENCY`:
- `stage=ingress`: received Newcamd ECM request; ID, CAID, SID only
- `stage=upstream_sent`: reader, time **prequeue_ms** (ingress -> queue),
  **queue_ms** (queued -> actual write attempt), and send=ok/failed
- `stage=upstream_reply`: upstream_ms (send attempt -> response), total_ms
  (ingress -> response), result and late=1 if original deadline already elapsed
- `stage=client_timeout`: when OSCam's client timeout expired, elapsed_ms

There are no CW/ECM bytes or hashes, usernames, passwords, IPs, protocol
headers or Newcamd credentials in the new logs. Existing OSCam debug modes
may independently log sensitive data: do not enable them.

The timestamps are monotonic, with the actual send event taken immediately
before handing the ECM to the Newcamd TCP socket; upstream_ms therefore
includes local TCP/kernel delivery plus remote processing. It is **not**
a pure remote compute-time measurement. No new network requests are made.
A late reply is observed only if it reaches OSCam's standard
`casc_check_dcw` path. OSCam's ordinary deadlines, CW processing, reader
policies and PID selection are unchanged.

## Quick use
After installing the new portable archive, opt in through a systemd override:

```bash
sudo systemctl edit oscam-mini.service
# Add exactly:
# [Service]
# Environment=DVBSTREAMER5_OSCAM_ECM_LATENCY=1
sudo systemctl daemon-reload
sudo systemctl restart oscam-mini.service  # maintenance window only
journalctl -u oscam-mini.service --since '-5 min' -o cat --no-pager |
  python3 scripts/diagnose_oscam_ecm_latency.py
```

For home2 Ubuntu 18.04, the analyzer uses Python 3.6-compatible syntax.
To disable: remove the override and restart in a maintenance window.

## Interpretation
- prequeue_ms high => time before OSCam schedules the reader
- queue_ms high => reader job queue / local scheduler / connection setup
- upstream_ms high with low local timings => network or remote Newcamd reader
- 5000ms timeout with late upstream reply => exceeded OSCam client deadline
- multiple SID intermittent failures with low TCP retransmissions do not
  prove reader overload or authorization failures.

## CI
C timing and enable-switch tests, synthetic journal analyzer fixtures,
full OSCam-mini build, media/failover tests and CTest via GitHub Actions.
