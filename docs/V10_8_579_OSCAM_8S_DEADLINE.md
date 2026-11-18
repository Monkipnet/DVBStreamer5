# V10.8.579 — Optional 8-second Newcamd ECM deadline

On home2, `clienttimeout = 5000` and `DVBSTREAMER5_OSCAM_NEWCAMD_GRACE_MS=3000` previously produced `deadline_ms=5000`, not 8000. The bounded parser in `oscam-newcamd-grace.h` silently rejected all values above 1500. This version raises the maximum accepted grace to 3000 ms.

## Scope

- Opt-in only, CAID `098D`, inbound Newcamd and outbound Newcamd reader.
- Existing defaults and the 1500-ms grace remain unchanged for those using them.
- `clienttimeout=5000` + `NEWCAMD_GRACE_MS=3000` now yields `deadline_ms=8000`.
- No ECM/CW transformation, replay, retry, reader reconnect, or other behavior changes.

## home2 verification

Keep the existing systemd drop-in `99-ecm-deadline.conf` with `Environment="DVBSTREAMER5_OSCAM_NEWCAMD_GRACE_MS=3000"` and diagnostic environment. Install the V10.8.579 portable package before restarting `oscam-mini.service`.

Check `journalctl -u oscam-mini.service -o cat | grep 'stage=upstream_sent' | tail` for `deadline_ms=8000`.

Observed negative upstream replies at approximately 8045 ms are likely still too late at an 8000-ms deadline. A timely NOT_FOUND also remains a negative response, not FOUND. Evaluate client timeouts and negative replies separately.

Regression test: `test_oscam_newcamd_grace.c` verifies 3000 accepted, 3001 rejected, 8s deadline, and routing scope.
