# UDP references, PCR policy and a server-side SRT bridge

## What the Astra comparison establishes

The [official Cesbo UDP documentation](https://github.com/cesbo/help/blob/40cb1e4ef338f8a255f3fa2515e569fafc3c8034/src/content/docs/en/astra/delivery-udp/index.md)
describes an explicit output interface, TTL=32, a separate synchronization
thread and `cbr=Kbps` NULL filling. Its [CBR page](https://github.com/cesbo/help/blob/40cb1e4ef338f8a255f3fa2515e569fafc3c8034/src/content/docs/en/astra/delivery-udp/cbr.md)
states that excess traffic above the configured CBR is dropped.

The [public Astra 4.4.187 archive](https://github.com/whittinghamj/astra/blob/646cc63bdf14d3da29b00a8da5c697fa9dcb31e2/modules/udp/output.c)
groups seven TS packets per datagram, buffers input for PCR synchronization,
and fills CBR gaps with NULL packets. This is an archival implementation;
it does not establish the internal scheduler of current commercial releases.

DVBStreamer5's new engine already has the same basic TS/UDP ingredients:
1316-byte CBR datagrams, buffering, PCR-based payload scheduling, NULL filling,
and absolute output deadlines. Version 10.8.370 matches the documented TTL=32.
It reports bounded-queue overflow rather than silently discarding media. No
claim is made that its scheduler is identical to current Astra.

## Source PCR versus Fixed PCR at a changed CBR rate

Source PCR preserves the incoming PCR bytes. Replacing source NULLs and shaping
a new CBR changes TS packet positions. A continuous, CC-clean output alone
does not establish that PCR values match the new constant-rate byte clock.

Fixed PCR uses every output TS slot, including NULLs, to regenerate PCR for
each PCR PID, retaining media payload and PTS/DTS. A ten-minute virtual-time
regression covers input at 3.5 Mbit/s and output at 4 Mbit/s. It verifies the
output PCR against the rational packet-position clock to within one 27-MHz
tick, including wrap, while checking byte order and unchanged non-PCR data.
This is an internal TS consistency check, not a wire-level PCR-jitter or
physical WISI receiver measurement.

For a controlled SRT -> UDP-CBR test on one channel:

1. Leave the SRT input, output address/interface, configured CBR and startup
   buffer unchanged.
2. Select **Fixed PCR**.
3. Disable the separate **Source PCR for HTTP/SRT -> UDP-CBR** experimental
   checkbox. It can otherwise override the general PCR-mode selector.
4. Save and restart only that tile. The native output startup log should show
   `source_pcr=0` and the expected `target_kbps` (for example, `4000`).
5. Compare the receiver counters/image over the previous failure periods.

## Independent server-side raw SRT -> UDP sender

`scripts/srt_to_udp_relay.sh` is a diagnostic FFmpeg wrapper for changing only
the network sender. Both input and output use `-f data`: no MPEG-TS demux/remux,
decoder, encoder, PCR rewrite or source NULL replacement is involved.

**The input must already be the desired complete CBR transport.** The UDP
`bitrate` option caps emission; it does not manufacture missing bytes/NULLs.
For example, this raw bridge cannot create a 4-Mbit/s CBR from a 3.5-Mbit/s
transport. Use the application's actual CBR shaper for that rate conversion.
Input stalls also remain output stalls in this diagnostic bridge.

Example for an existing 5-Mbit/s SRT transport:

```bash
bash scripts/srt_to_udp_relay.sh \
  --input 'srt://192.0.2.10:34000?mode=caller&latency=1500000' \
  --output 'udp://239.255.99.2:56445' \
  --interface '192.0.2.10' --bitrate 5000000
```

Use the server's real SRT listener address and local output IP. A listener bound
to a specific IP may not accept connections to `127.0.0.1`. FFmpeg SRT URL
latency uses microseconds: `1500000` means 1500 ms. The native application's
SRT latency controls use milliseconds.

This tool requires an FFmpeg CLI with SRT support. It is a separate diagnostic
process; the native application does not acquire a new runtime dependency.
The bridge can run on the same server as the SRT output. Keep its test multicast
group distinct from the original output. On a Wi-Fi comparison, receive one
multicast group at a time to avoid changing the offered multicast load.

The local end-to-end check fed real CBR MPEG-TS through SRT, then this bridge,
and compared the multicast bytes against a direct reference. It preserved the
TS bytes including NULL/PCR, emitted 1316-byte datagrams at approximately
5 Mbit/s and delivered multicast TTL=32.

SRT recovery protects the SRT leg only. A same-server bridge does not add
acknowledgements or retransmissions between the UDP sender and WISI. Recovery
across a problematic network requires an SRT receiver after that network,
followed by a reliable local UDP link to the modulator.
