# DVBStreamer5 V10.8.528 — OnDemand: Unicable/JESS and HLS HEAD preflight

Date: 2026-11-14. Repository: `Monkipnet/DVBStreamer5`, branch `main`.
This is a narrow runtime fix; it does **not** change encoders, video PTS,
OSCam-mini, service mux policy, normal Online startup or the Stop safety gate.

## User evidence from home2 V527

- `Warner TV Comedy`, `sat-1794306945530-136-3`, HTTP OnDemand:
  `ONDEMAND ACTIVATE FAILED source=http stage=dvb-warm
  error=DVB frontend adapter0/frontend0 has active service consumers`
  repeated for multiple player requests.
- `Sky Sport News`, `sat-1794213401098-108-0`, HLS OnDemand:
  `ONDEMAND ACTIVATE FAILED ... stage=dvb-warm
  error=DVB frontend did not lock within 8000 milliseconds`
  for HLS and preview attempts.
- Both channels have `activation_mode=ondemand` and `auto_start=false`;
  this configuration is valid, so do not turn on global Auto Start to fix it.
- The user reports that IP-source OnDemand channels also fail.
  **No IP-specific activation log has been provided yet.** It may be
  an independent problem; do not claim it is explained by DVB/Unicable.

## Confirmed source-code defect — DVB warm tune identity

`src/StreamManager.cpp` builds a live DVB tune from
`DvbSatelliteParams` including `unicableMode`, `unicableUserBand`,
`unicableFrequencyKHz`, `unicablePosition`, `unicablePin`.

Before V528, `src/StableUdpOutput.cpp::makeWarmDvbTune()` copied all
basic transponder fields but **omitted all five Unicable/JESS fields**.
`SharedDvbInputPool::transponderKey()` explicitly includes these fields.

Consequence: OnDemand warm standby calculated a *different* transponder
identity than an already-active compatible DVB service and refused to
join the frontend because its bus has active subscribers. Even without
other active consumers, missing Unicable settings could trigger an
incorrect satellite tune and a subsequent 8-second LOCK timeout.

**V528 fix:** copy all five Unicable/JESS values into the warm
`LinuxDvbTuneConfig` so they agree with a normal live subscription.

**Safety:** this does **not** allow preempting a busy frontend. If a
requested OnDemand channel genuinely belongs to a different transponder
from the active service, `warmStandby()` still refuses and protects the
currently running broadcasts. Either use an available tuner or tune the
required transponder through an operator-approved change.

## HLS HEAD-first clients: preflight while idle

Before V528, `HEAD /hls/.../*.m3u8` returned 404 if the OnDemand channel
had not generated a playlist. Some external IP/HLS players probe HEAD
and never proceed to the GET that activates the channel.

**V528 behavior:** for a *configured OnDemand HLS manifest* the
authorized HEAD request can return a 200 with the proper playlist
content type even before the file exists. HEAD remains side-effect-free:
it neither tunes a DVB frontend nor starts an IP-source transcoder.
The following normal **GET** must still activate the OnDemand stream
and produce actual media. Segment HEAD requests are not relaxed;
subscriber/IP and middleware authorization remain enforced.

**Scope limitation:** this fixes the HEAD-first compatibility case,
not every reported IP OnDemand failure. A failed authorized GET must be
investigated separately with its `ONDEMAND ACTIVATE FAILED`,
`stream has an operator stop request`, `HLS STARTUP PENDING`,
source error, or 403/503 response. The panel's manual Stop intentionally
blocks OnDemand resurrection until the panel's explicit Start.

## Regression and release

- `tests/web/test_ondemand_activation_v528.js`: compares all 19 DVB
  warm/live tune identity fields and checks OnDemand HEAD handling,
  GET activation, and operator Stop guard.
- GitHub Actions `build-main-linux.yml` runs this test alongside
  existing Stop and configuration tests (two matching workflow sections).
- Source-level field parity checks passed before commit.
- Runtime activation after V528 install, full CMake/CTest and GitHub
  CI archive result have **not yet been confirmed**.

### Build from the source tree root

```bash
git switch main
git pull --ff-only origin main
node tests/web/test_ondemand_activation_v528.js
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
(cd dist && sha256sum -c DVBStreamer5-10.8.528-linux-x86_64.tar.gz.sha256)
```

The GitHub Actions portable bundle includes OSCam-mini and TSDecrypt
as configured by its workflow. Verify the run and checksum and check
Linux/GLIBC compatibility for Ubuntu 18.04 `home2` before installing.
Installation from the portable directory with `./install.sh` stops
services and is **not** a zero-downtime test. Back up
`/opt/DVBStreamer5` and `/etc/dvbstreamer5` and schedule a window.

### Verify OnDemand in V528

1. Open the original HTTP output URL of Warner TV Comedy in a player,
   once. Record timestamp and HTTP status.
2. Check `journalctl -u dvbstreamer5.service --since '-5 min'
   --no-pager -o cat | grep -E 'ONDEMAND|SHARED DVB|HLS STARTUP'`.
3. A compatible channel should show `ONDEMAND ACTIVATE`, not
   `stage=dvb-warm has active service consumers`. Monitor unaffected
   DVB services on the same frontend.
4. Test Sky Sport News by GET on its public HLS `index.m3u8`/real
   playlist path; allow cold-start time, then verify the first media
   segment and playback. If LOCK still fails, check the input
   transponder/Unicable parameters and physical RF signal.
5. For an IP-source OnDemand tile with HTTP/HLS output, compare an
   authorized `curl -I` (HEAD, no activation) with a short
   `curl -v --max-time 15 -o /dev/null` GET (triggers activation).
   Inspect `ONDEMAND ACTIVATE` and any `stage=service-pipeline error=`.
6. Disconnect the client. OnDemand normal idle grace is 30 seconds;
   transcoded HLS has a separate bounded startup grace.
7. Verify manual panel Stop remains authoritative and unaffected
   channels stay live.

### Rollback

Retain the exact known-good V527 portable archive and backups before
installing V528. If needed, restore the previous archive using its
installer in a maintenance window, restore verified configuration only
if changed, and verify `VERSION`, streams, DVB LOCK and CA.
Never force retune an active frontend simply to make OnDemand work.

## Outstanding

- A real IP-source failed GET log is needed if IP OnDemand still fails.
- Confirm behavior of HEAD-first players; the code path is fixed but
  field validation is outstanding.
- Complete GitHub Actions/CTest and prolonged playback checks; do not
  claim an already deployed V528.
