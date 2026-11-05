# UDP-CBR / UDP-VBR transport rewrite (10.8.369)

Branch: `rewrite/udp-transport`. This replaces the UDP output implementation,
not the DVB reader, CA backend, remapper, codec, EPG/subtitles or failover logic.
The old sender remains selectable for rollback. Existing configs default to
`udp_output_engine: "legacy"`; select `"native"` to use the new implementation.

## What is new

- Two shared scheduler threads serve all channels. There is no dedicated spinning
  or realtime sender thread for every destination. A producer holding a queue
  lock is skipped and retried, so it cannot block all other channels on a shard.
- CBR is shaped once per channel, then identical datagrams are fanned out to its
  UDP-CBR addresses. There is one TS clock and one source FIFO for those copies.
  VBR destinations form a separate group of the same channel.
- `UdpTsMux` is a new batch-oriented TS packetizer. It does not instantiate the
  old `CbrTsPacer` or use its useful-bitrate EWMA/reservoir controller. The old
  bitrate encoding is decoded for configuration compatibility. The source PCR
  interval mapper is reused as a timestamp primitive.
- The rational absolute UDP clock advances from its scheduled deadlines, not
  from each actual wakeup. Small OS wakeup errors no longer accumulate into
  lower long-term CBR. Recovery spacing is bounded to at least 90% of a nominal
  period. After a stall exceeding four periods the clock is reanchored and the
  event counted; overdue packets are not replayed in an unbounded burst.
- Nonblocking `sendto` and per-destination pending FIFOs retain the exact datagram
  on EAGAIN/EINTR/ENOBUFS. Other destinations continue sending. A full queue or
  congestion lasting 500 ms is an explicit output failure, not a silent TS drop.
- Socket send buffers are requested explicitly (4 MiB); the effective kernel
  value is read back and reported. No sysctl or qdisc changes are made.
- Asynchronous observers retain packet order. SRT/HTTP/HLS/RTSP/RTMP/MPTS still
  receive the shaped CBR transport when a CBR UDP output exists. Observer
  overflow is reported, rather than silently deleting old datagrams.

## Preserved behavior

CBR emits 1316-byte datagrams (7 x 188 TS), removes source NULLs and generates
its own PID 0x1fff budget at the configured transport rate. Source PCR preserves
all useful TS bytes, including PCR/PTS/DTS/CC. Fixed PCR is anchored independently
per PCR PID, with unchanged PTS/DTS and payload. PCR wrap/discontinuities are
handled without deleting queued media. Both modes use available source PCR
intervals to position useful packets, avoiding an average-rate media clock.
When PCR is absent, input arrival timing is used as the fallback.

The existing startup buffer (including 4500 ms) is honored, with the existing
70%-of-8-MiB capacity bound. In the new engine the 5000-ms setting no longer
implicitly selects the old +/- useful-rate controller. The 10.8.368 experimental
DVB pacing checkbox is a legacy-engine option and is not used by the new mux.

VBR preserves source packet bytes, including source NULLs, without added stuffing
or PCR rewriting. It combines input fragments into up to 7 TS per datagram,
retains the remainder between calls, and flushes a partial datagram after 2 ms
or source EOF. This retains low-rate audio/control packets and file tails.

Unicast, multicast, interface IP binding, multiple addresses, CA/remap callbacks,
RTP alongside UDP, HTTP/file/external inputs and source EOF draining remain
supported. Per-output transmitted-byte counters advance on successful sends.
Input TS/UDP/RTP framing remains separate from output packetization.

The interface APIs now include `lo (127.0.0.1)` for input and output selectors,
including MPTS output. Auto/external-address detection still uses the original
non-loopback enumeration. Network metrics also include loopback. Selecting its
IPv4 address for UDP input resolves the corresponding device automatically.
Loopback carries traffic within the same host.

## Deployment and rollback

1. Build the branch, then install the test version in the usual way.
2. In one affected channel choose **UDP output engine -> New UDP-CBR / UDP-VBR**.
   Leave Source PCR / 4500 ms / 3000 kbit/s unchanged for the first comparison.
3. Save and restart only that tile. The startup log must contain
   `UDP TS OUTPUT engine=native` and the expected destination count.
4. Compare the exact multicast group at the server and at WISI over multiple
   previous fault periods. Check CC/PCR and NIC/switch/WISI drop/buffer counters.
5. Expand to more channels after this check. Roll back by selecting **Legacy**,
   saving and restarting the tile. Existing settings/outputs are retained.

For 100 channels at 3 Mbit/s the TS payload totals 300 Mbit/s, approximately
315 Mbit/s including usual Ethernet/IPv4/UDP overhead. Use the sum of actual CBR
budgets for a mixed SD/HD lineup; this estimate is not a measurement of the NIC.

`/api/state` exposes `udp_transport` per active tile: mode, sent datagrams,
send retries, maximum scheduling lateness, clock resets, source/pending/observer
queue bytes, source queue peak, effective send buffer and NULL-only datagrams.
These are sender-side observations, not confirmation of receiver delivery.

## Verification

- The existing relay suite runs against both engines, including UDP/RTP,
  HTTP, files, CA callbacks and two UDP outputs plus the shared SRT observer.
- New packetizer tests cover cross-chunk VBR framing, 2-ms partial flush, queue
  bounds, invalid input atomicity, source/fixed PCR, PCR wrap, byte/CC/PTS/DTS
  preservation and ten minutes of virtual VBR source timing.
- An injected nonblocking sender simulates temporary failure of one destination;
  both destinations and the observer must receive exactly the original TS with
  no dropped, duplicated or reordered media packets.
- A real loopback load test runs 100 CBR channels at 3 Mbit/s each, verifies
  200,000 media packets, and checks sustained per-channel bitrate and scheduler
  resets. It is run without overlapping builds; it is not a physical NIC/WISI
  test or proof of performance under arbitrary CPU saturation.
- GitHub Actions is configured to build/test/package on Ubuntu 22.04 and 24.04.
  The implementation uses C++17, standard threads and normal Linux UDP sockets;
  there is no new library, kernel pacing or network administrator requirement.
- Core transport/error/retry tests passed ASan/UBSan and ThreadSanitizer locally.
  A complete application check used a UDP input on lo and identical multicast
  and unicast output copies. The portable bundle was also started with its own
  loader/libraries, including native VBR, Newcamd and loopback API/metrics.

The new engine cannot repair multicast losses after the kernel accepts a packet,
IGMP membership problems, a CBR budget below sustained useful TS bitrate, or a
source signal already containing damaged media. OS stalls still exist and are
reported. Long-term source clock drift and actual hardware buffer tolerances
must be monitored on the deployment host; short loopback and virtual-time tests
are not a guarantee that the reported broadcast artifacts have disappeared.

## Build / archive

Update the existing checkout:

```bash
cd ~/DVBStreamer5
git fetch origin
git switch rewrite/udp-transport
git pull --ff-only origin rewrite/udp-transport
```

Dependencies and codec preparation are the same as 10.8.368. For Ubuntu 22.04,
the codec helper uses its existing Autotools fallback for older CMake. Build on
22.04 when that is the oldest intended deployment target.

```bash
BUILD="$HOME/DVBStreamer5-build-udp-native"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_ENABLE_FFMPEG_AVC=ON \
  -DDVBSTREAMER5_BUILD_MEDIA_CORE_TESTS=OFF
cmake --build "$BUILD" --parallel 2
ctest --test-dir "$BUILD" --output-on-failure
cmake -S tests/media_core -B "${BUILD}-media" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD}-media" --parallel 2
ctest --test-dir "${BUILD}-media" --output-on-failure
OUT="$PWD/dist/DVBStreamer5-10.8.369-udp-native-linux-x86_64"
bash scripts/make_portable_linux_bundle.sh --build-dir "$BUILD" \
  --binary "$BUILD/DVBStreamer5" --output-dir "$OUT" --with-oscam --archive
sha256sum "${OUT}.tar.gz" > "${OUT}.tar.gz.sha256"
```

## Compare UDP and SRT on Windows

Run `scripts/compare_udp_srt.ps1` in Windows PowerShell 5.1 or PowerShell 7,
with `ffmpeg` and `ffprobe` on PATH. Enable both outputs on the same tile before
recording. No DVBStreamer5 rebuild or service restart is needed for this tool.

```powershell
.\scripts\compare_udp_srt.ps1 `
  -UdpUrl 'udp://@239.10.10.1:1234' `
  -SrtUrl 'srt://192.0.2.10:34000?mode=caller&latency=1500000' `
  -DurationSeconds 90
```

Use the actual multicast group and SRT endpoint. FFmpeg interprets the SRT URL
`latency` in microseconds, so `1500000` means 1500 ms. Add
`-InterfaceIP '192.0.2.20'` to select the Windows NIC if multicast arrives via
a specific interface. Alternatively, `-Ethernet` automatically selects a single
connected Ethernet IPv4 address. If multiple wired addresses exist, it lists
them and requires an explicit choice with `-InterfaceIP`. The address must be
assigned to a connected local PC adapter; a network prefix or server address
is rejected before capture. Interface details are included in
`capture-interface.json`. Both captures stop if either FFmpeg process exits
early, and a report missing either audio/video source is marked incomplete.
Custom executable locations can be passed with `-Ffmpeg`
and `-Ffprobe`. Existing captures can be analysed with `-UdpFile` and `-SrtFile`.

The two FFmpeg processes capture raw data concurrently with `-f data` on both
input and output: no MPEG-TS demux/remux, PCR restamping or CC regeneration is
performed during capture. A wall-clock timer stops recording even if timestamps
freeze. Decoding/ffprobe analysis runs on the saved files afterwards.

The new output directory contains original `udp.ts` / `srt.ts`, per-PID CSVs,
`summary.csv` (audio/video packet counts, CC/TEI, PES/PTS changes/span and decoded
video frame count), `clocks.csv` (PCR counts, timestamp span/gaps/backwards), and
capture/probe/decode logs. `report.zip` packages the reports without the large TS
files. Keep both TS captures for further comparison.

Different start/end times and output buffers can produce different packet
counts; counts alone are not evidence of loss. A decoder may report missing
reference frames when capture starts in the middle of a GOP. CC errors need to
be correlated with the input/server/network; they do not identify which device
lost data. PCR timestamp gaps are media-clock measurements, not a measurement
of UDP network jitter. Zero CC errors do not establish that the payload is good.
For intermittent failures, increase `-DurationSeconds` to 300 or more and capture
an actual breakup, rather than drawing conclusions from a clean short window.

## Analyse the server's outgoing packet capture

`scripts/analyze_udp_pcap.py` reads an existing classic `.pcap` / `.pcap.gz`
using Python 3's standard library. It supports Linux cooked SLL/SLL2, Ethernet
with VLAN tags, and raw IPv4, with microsecond or nanosecond capture timestamps.

```bash
python3 scripts/analyze_udp_pcap.py /tmp/udp-out.pcap.gz \
  --destination 239.10.10.1 --port 1234 --json /tmp/udp-out-analysis.json
```

It reports each interface/source socket separately, datagram sizes/rate/gaps,
and per-PID CC, duplication, TEI, scrambling, discontinuities and PCR. Keeping
interfaces separate prevents copies observed on a bridge/bond from creating
false CC alarms. Malformed/truncated records and IPv4 fragments are reported;
fragmented datagrams are not reassembled. A port-based capture filter may omit
non-initial IP fragments. Keep the `tcpdump` drop statistics alongside the file.
TX checksum offload is not treated as payload damage. Software capture timing
does not establish wire-level PCR accuracy or downstream delivery.

Standalone regression checks:

```bash
python3 -m unittest discover -s tests -p test_udp_pcap_analysis.py -v
```
