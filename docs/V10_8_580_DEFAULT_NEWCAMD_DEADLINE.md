# V10.8.580 — default iCAM Newcamd grace

For CAID 098D with **inbound Newcamd clients** and an **outbound Newcamd reader**, OSCam-mini now applies 3000 ms grace when `DVBSTREAMER5_OSCAM_NEWCAMD_GRACE_MS` is **unset**. With `clienttimeout = 5000`, this yields an 8000 ms client ECM deadline without a systemd override.

All other CAIDs, inbound protocols and reader types retain existing deadlines. This is a conditional default and does not globally change `clienttimeout`.

Explicit overrides remain supported: `0` disables grace; `1500` selects the former 6500 ms policy; `3000` selects the 8000 ms policy; values over 3000 or invalid strings are rejected to zero. No ECM payload/CW manipulation or reconnect behavior changes.

## home2

The existing `99-ecm-deadline.conf` explicitly sets `3000`, which is equivalent to the new default. No need to remove it during the first upgrade. To verify that the new default works **without** the override, remove only the grace assignments from all applicable systemd drop-ins, `systemctl daemon-reload` and restart **only** oscam-mini during a maintenance window. Verify `deadline_ms=8000` and ECM result rates. Production server `svettranscoding` is unaffected until separately deployed.

Previous 10-minute home2 observation: 859 FOUND, zero NOT_FOUND, client_timeout, late replies and unmatched. Longer operational validation is required. External upstream NOT_FOUND around 8045 ms could still miss the 8000-ms deadline.
