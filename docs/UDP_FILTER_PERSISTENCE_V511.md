# DVBStreamer5 V10.8.511 — persistent UDP/RTP input filter checkboxes

## Confirmed root cause in V510

The `/api/network/udp-input-filter` endpoint correctly saved
`udp_input_filter_interfaces` when the operator checked a network interface.
However, normal `/api/save-config` calls from editing a stream, global
settings or language omitted that field. `AppConfig::fromJson` then defaulted
to an empty vector, and the general save overwrote the enabled interface list.
Subsequent metrics polls displayed unchecked boxes. Each config save also
reapplied nftables, removing the filter table when the list had been emptied.

## Fix

- Preserve the previously saved `udp_input_filter_interfaces` on missing
  or malformed arrays in general config saves. An explicit valid empty array
  still disables the filters, preserving import/full-config semantics.
- During a network-filter toggle, keep the requested checkbox state in a
  pending map through asynchronous metrics-table redraws (every 3 seconds).
  Ignore duplicate clicks, wait for any old sample, fetch fresh metrics after
  the API confirms the setting, and render from authoritative server state.
- Existing `nftables` scope, SRT exemption, explicit UDP/RTP endpoint
  allowlist, fail-open behavior, and independent interface addresses remain
  unchanged. No service restart is needed merely to save a checkbox.
- V511 adds `tests/web/test_udp_filter_persistence_v511.js` to GitHub CI.

## Read-only diagnosis on installed servers

```bash
echo '===== SAVED FILTER ADDRESSES ====='
jq '.udp_input_filter_interfaces' /etc/dvbstreamer5/dvbstreamer5-config.json
echo '===== INTERFACE ADDRESSES ====='
ip -4 -br addr
echo '===== OWN NFT TABLE ====='
nft -a list table inet dvbstreamer5_udp_input 2>&1 || true
echo '===== FILTER LOG ====='
journalctl -u dvbstreamer5.service --since '-2 hours' --no-pager |
  grep -E 'UDP INPUT FILTER|config save'
```

Never manually apply `nft flush ruleset`. The DVBStreamer5 filter belongs
only to its own `inet dvbstreamer5_udp_input` table. The visible checkboxes
represent saved intent; a failed nftables apply logs a fail-open condition.
