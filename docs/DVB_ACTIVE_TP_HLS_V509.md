# DVBStreamer5 V10.8.509 — Active transponders, live LOCK, HLS decode status

## Confirmed problems on test server home2

1. The "Add channel" adapter selector showed "назначено 1/1" while
   DVBStreamer5 was already receiving several services on that adapter.
   The UI counted *any* configured frontend as "assigned", without separating
   currently active clients or physically opened frontends.
2. The signal-only /api/dvb-signal path refused to examine a frontend
   already owned by DVBStreamer5 and returned an error; the modal then
   displayed NO LOCK despite live received television.
3. HLS playlists and fragments use many short requests. V506 needed a
   client indication to overlap two good output-PES observations before
   promoting "ДЕКОД: PES" to "ДЕКОД: ОК". Thus HLS playback could be
   functioning while the indicator stayed PES.

## V509 changes

- DVB adapter API now exports frontend_active, active_transponders and
  active_transponder_count separately from configured_streams, consumers,
  OnDemand assignments, and external process owners. Only active primary
  DVB-backed tiles contribute to transponder counts.
- Adapter selector says "используется N/M · TP K" when a frontend is
  currently open or has live service consumers; an idle OnDemand
  assignment says "назначено". The frontend selector retains SHARED N.
- A signal-only query reads already open frontend stats without
  re-tuning. A service scan still refuses to retune a busy frontend.
- A Linux DVB TS reader records the last time it actually read packets.
  If the FE_READ_STATUS ioctl fails or reports no lock, recent TS gives
  a separately identified "TS ACTIVE" inference, *not* a claim of
  confirmed FE_HAS_LOCK. Stale TS never keeps the status indefinitely.
- HLS logical subscribers are created/refreshed only when a real
  media segment is successfully served. A recent media viewer and
  fresh decrypted PES now promote the UI gate immediately. Once
  promoted, the indicator stays green while PES and output remain
  fresh; short interruptions are tolerated and genuine stalls reset it.
- Public stream auth, actual CA/ECM/CW, TS packets, HLS segments,
  preview transcode and tuner LNB/Unicable tuning are unchanged.

## Tests

```bash
node tests/web/test_dvb_live_transponder_v509.js
c++ -std=c++17 -I. tests/test_tsdecrypt_viewer_decode_gate.cpp -o /tmp/v509-gate
/tmp/v509-gate
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
```
