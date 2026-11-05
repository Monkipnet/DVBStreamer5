# Experimental DVB Source PCR pacing

Version 10.8.368 adds an opt-in per-channel scheduler for DVB passthrough to UDP-CBR.
It addresses the delivery-phase variation caused by using a smoothed average
useful bitrate across a variable-bitrate service. It is not a fix for multicast
packet loss or an output bitrate below the service's required bitrate.

## Compatibility and scope

- The default is off. Old configurations retain their existing behavior.
- Enable it only for DVB input, UDP-CBR output, Source PCR, a nonzero startup
  buffer, and no transcoding or test pattern. Other profiles ignore the option.
- The wire format stays seven 188-byte TS packets per UDP datagram (1316 bytes).
- The configured physical CBR rate, NULL stuffing, UDP sender deadlines,
  scheduling policy, interfaces and multicast destinations are unchanged.
- Useful TS packet order and all packet bytes, including PCR, PTS/DTS and CC,
  are preserved. PCR wrap is unwrapped only for internal scheduling.
- Concurrent SRT/HTTP observers keep receiving the first UDP-CBR worker's TS.
- Missing PCR lookahead is extrapolated from the last interval. Before a valid
  interval exists the legacy rate fallback applies. Explicit source clock resets
  bridge the affected interval without discarding previously queued packets.

The scheduler interpolates packet positions between adjacent source PCRs already
present in the startup reservoir, instead of smoothing their useful bitrate into
one emission rate. Source clock discontinuities retain their original TS flags.
It never attempts to recover a lost input packet or creates a second UDP clock.

## Controlled WISI Chameleon check

In the channel editor under **CBR: PCR mode and startup buffer**, enable
**Precise DVB Source PCR pacing (experimental)** for one affected channel.
The stored field is `dvb_source_pcr_pacing`; it defaults to `false` when absent.
Keep the user's existing **Source PCR / 4500 ms / 3000 kbit/s** settings for the
first comparison. Save and manually restart only that channel. Check the log for
`useful_schedule=pcr-intervals` on its WISI worker.

Compare the same multicast group at the server and receiver, not two separate
UDP outputs. Monitor WISI input errors/buffer alarms, video/audio CC, network
drops, actual UDP spacing and useful TS bitrate for multiple usual fault periods.
If the useful bitrate exceeds 3000 kbit/s, select an adequate transport budget
before attributing a growing queue to pacing.

Rollback: uncheck the option, save, and restart that channel. The log should show
`useful_schedule=legacy-rate`. No global environment variable or migration is
required. Do not switch PCR mode or change the buffer at the same time as this
comparison. In the legacy implementation the 5000-ms marker also selects a rate
assist; the new scheduler does not use that average-rate controller.

## Validation and remaining limitations

The 600-second virtual-time regression uses 3000 kbit/s, Source PCR, 4500 ms and
a 2000/2800 kbit/s service alternating every 15 seconds. The legacy packet-slot
delivery phase spans about 1011 ms; the opt-in spans less than one TS slot
(0.502 ms at this bitrate). This is a transport model, not a measured Chameleon
result, a real NIC jitter bound, or an ETSI TR 101 290 PCR accuracy claim.

Tests compare every useful input/output packet byte-for-byte, including timestamp
fields; check NULL continuity, missing-PCR fallback, default-off/fixed-PCR
compatibility, PCR wrap/discontinuities, config persistence and eligibility.
A loopback integration check compares two UDP-CBR outputs and their common
SRT/HTTP observer, including the 1316-byte datagram format.

The existing physical sender's bounded phase recovery is intentionally unchanged.
Sustained OS scheduling delays can still reduce its actual output rate. This
patch also cannot recover NIC/switch/receiver drops or repair IGMP membership.
Hardware compatibility and disappearance of the reported glitches require the
controlled live check above before broader activation.

## Build and package on Ubuntu 24.04 x86_64

Update the existing checkout without resetting local work:

```bash
cd ~/DVBStreamer5
git fetch origin
git switch fix/dvb-source-pcr-pacing
git pull --ff-only origin fix/dvb-source-pcr-pacing
```

Install build prerequisites (these are development libraries, not GPU drivers):

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake pkg-config python3 nodejs curl ca-certificates \
  binutils nasm autoconf automake libtool file \
  libpcsclite-dev libssl-dev libcrypt-dev libmp3lame-dev \
  libcurl4-openssl-dev libxml2-dev zlib1g-dev \
  libva-dev libvpl-dev libfreetype-dev fontconfig fonts-dejavu-core \
  libavcodec-dev libavutil-dev
bash scripts/vendor_srt_source.sh
```

If the pinned native codec prefix is not already built, prepare it once:

```bash
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
```

Use an out-of-source build directory on the Linux filesystem. The main build
includes DVBStreamer5, the mandatory Newcamd plugin, OSCam-mini and five tests.
The standalone media build adds fifteen transport/policy/PCR checks. Keeping
`DVBSTREAMER5_BUILD_MEDIA_CORE_TESTS=OFF` in the main build matches the Linux CI
layout; it avoids pulling unrelated optional test targets into this build.

```bash
BUILD="$HOME/DVBStreamer5-build-wisi-pcr"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_BUILD_MEDIA_CORE_TESTS=OFF \
  -DDVBSTREAMER5_ENABLE_FFMPEG_AVC=ON \
  -DDVBSTREAMER5_ENABLE_NVENC=ON \
  -DDVBSTREAMER5_ENABLE_VAAPI=ON \
  -DDVBSTREAMER5_ENABLE_INTEL_QSV=ON
cmake --build "$BUILD" --parallel 2
ctest --test-dir "$BUILD" --output-on-failure
cmake -S tests/media_core -B "${BUILD}-media" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD}-media" --parallel 2
ctest --test-dir "${BUILD}-media" --output-on-failure
```

Create a complete portable archive, requiring OSCam-mini instead of silently
omitting it:

```bash
BUILD="$HOME/DVBStreamer5-build-wisi-pcr"
VERSION=$(sed -n 's/.*kProgramVersion = "\([^"]*\)";.*/\1/p' src/AppVersion.h)
OUT="$PWD/dist/DVBStreamer5-${VERSION}-wisi-pcr-linux-x86_64"
bash scripts/make_portable_linux_bundle.sh \
  --build-dir "$BUILD" --binary "$BUILD/DVBStreamer5" \
  --output-dir "$OUT" --with-oscam --archive
sha256sum "${OUT}.tar.gz" > "${OUT}.tar.gz.sha256"
```

The archive includes the app, Newcamd plugin, OSCam-mini, ELF loader and detected
shared libraries. Host DVB/GPU drivers and firmware remain prerequisites. For
the widest portability, build on the oldest supported Ubuntu target. Building
an archive does not publish or install it on the production server.
