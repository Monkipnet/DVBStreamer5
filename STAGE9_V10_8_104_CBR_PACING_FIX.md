# Stage 9 V10.8.104 — CBR pacing drift fix

## Problem

UDP CBR used `CbrTsPacer` with seven 188-byte MPEG-TS packets per UDP datagram.
When the worker woke more than two datagram intervals late, the pacer reset its
next deadline to the current wall-clock time.

At normal DVB bitrates two datagram intervals are only a few milliseconds, so
ordinary Linux scheduler jitter repeatedly discarded elapsed pacing time. The
result was a real UDP payload bitrate below the configured target and visible
short-term CBR instability.

## Fix

`CbrTsPacer::nextDatagram()` now keeps its monotonic pacing timeline across
ordinary scheduler jitter. The relay can drain overdue datagrams using the
existing bounded drain loop. Re-anchoring is reserved for a real long stall
(250 ms), preventing a huge catch-up burst after suspend/debugger/source stops.

No decoder, encoder, audio path, HLS segmentation, SRT transport, remapping or
CA/Newcamd logic is changed.

## Runtime verification

After installing V10.8.104, verify the binary version and then measure the UDP
payload rate for at least 10 seconds. For a 6000 kbit/s target the measured TS
payload should stay close to 6000 kbit/s (allowing normal measurement/scheduler
error) and the datagrams should normally be 1316 bytes.

Example measurement:

```bash
timeout 10 tcpdump -i <IFACE> -nn -l 'udp dst host <DEST_IP> and dst port <PORT>' 2>/dev/null | \
awk '/UDP, length/ {bytes += $NF} END {printf "UDP payload: %.1f kbit/s\n", bytes*8/10/1000}'
```

For multicast, replace `<DEST_IP>`, `<PORT>` and `<IFACE>` with the configured
output destination and interface.
