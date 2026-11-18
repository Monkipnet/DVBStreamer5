# V10.8.578 — Newcamd response matching diagnostics

This is a metadata-only, opt-in diagnostic update to OSCam-mini. It does not change deadlines, card requests, reader selection, replies or control words.

## Why

On home2, CAID 098D SID 008F and 0091 produced alternating client timeouts at 6500 ms even while other requests for those SIDs returned FOUND. The upstream TCP connection remained established. Previous `upstream_reply` traces only covered matched responses, leaving unmatched Newcamd message IDs invisible.

## Added events

Enable via `DVBSTREAMER5_OSCAM_ECM_LATENCY=1` (already enabled on home2):

- `stage=upstream_rx_matched`: received Newcamd response paired with an outstanding reader slot. Includes ECM diagnostic ID, reader label, CAID, SID, protocol message ID, response result and milliseconds since send.
- `stage=upstream_rx_unmatched`: syntactically accepted Newcamd ECM reply whose message ID matches no outstanding reader slot. Includes reader label, message ID, response result and pending count; no ECM ID is invented.
- `stage=ecm_pending_duplicate`: request joined an existing reader ECM request, either `mode=pending` or `mode=cached`. Includes child and origin diagnostic IDs and SID.

These events contain no ECM payload, ECM hash, CW, DES key, login, host or IP address. The reader label is already present in existing optional diagnostic logs. Raw Newcamd message IDs are transaction counters, not credentials.

## Analysis

Correlate `upstream_sent`, `upstream_rx_matched`, `upstream_reply`, `upstream_rx_unmatched`, `ecm_pending_duplicate` and `client_timeout` by ID and SID. An unmatched message can indicate an expired slot, out-of-order response or message-ID reuse; it is not automatically evidence of an upstream server failure.

Do not infer that `upstream_sent send=ok` proves remote delivery. It only proves the local send call succeeded.

## Validation

The existing OSCam ECM latency analyzer Python test guards the receive and duplicate hooks. The main Linux CI builds OSCam-mini. This documentation does not claim a production or home2 deployment.
