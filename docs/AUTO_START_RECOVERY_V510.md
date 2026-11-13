# DVBStreamer5 V10.8.510 — recovery of unexpectedly stopped online tiles

* Startup **Auto Start** checkbox is now also a runtime recovery policy for `activation_mode=online`.
* If the native relay exits, the existing monitor logs `NATIVE STREAM RELAY STOP stream_id=... reason=...` and marks the tile inactive. The supervisor cleans the stale instance and starts the same saved configuration.
* Failed initial auto-starts also get retries. Three independent recovery lanes avoid one stalled input holding up all tiles.
* The first supervisor check is 35 seconds after the service starts, then every 5 seconds.
* Per-tile attempts back off: 10, 20, 40, 60, 120 seconds (max), returning to the first delay after 2 minutes of stable running.
* Explicit panel Stop is authoritative until the next explicit panel Start; OnDemand, failed input with registered failover, and in-progress manual stop/restart are excluded.
* The regular 5-second media-silence indicator **does not** trigger a restart. The relay must really be inactive.
* All launches are protected by a per-ID gate against duplicate parallel initialization. No systemd restarts, tuner retunes or modifications to unrelated active streams.

## Diagnose root cause on the installed server

```bash
journalctl -u dvbstreamer5.service --since '-2 hours' --no-pager -o cat |
  grep -E 'NATIVE STREAM RELAY STOP|AUTO START RECOVERY|NATIVE OBSERVED CBR OVERFLOW|NATIVE (AV|VIDEO|HLS|SRT) |ONDEMAND DEACTIVATE|NATIVE INPUT FAILOVER|GLOBAL STREAM RESTART' |
  tail -n 250
```

A recovery is **mitigation**, not proof that the original relay failure has been repaired. Inspect `reason=` and compare with actual input and output rates. Never auto-restart tiles marked OnDemand or explicitly stopped by the operator.
