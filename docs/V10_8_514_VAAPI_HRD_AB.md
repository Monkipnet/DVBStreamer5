# V10.8.514 — experimental per-tile VAAPI H.264 HRD/A-B test

## Observed baseline

svettranscoding, MIR24_TRANS: `stream-1794627918823`, VAAPI-direct H.264
1920x1080@25, 6000-kbit/s video, 8000-kbit/s UDP-CBR. Mir24's
pre-change capture produced a **512.4-KiB non-IDR P-frame**; it consumed
approximately **537 ms** of 8-Mbit/s MPEG-TS output, compared to a 40-ms
video timestamp step. V513 source-PCR packet pacing reduced median
capture-vs-PCR deviation but left rare ~498-ms outliers.

These numbers are from the user's real PCAP; they are **not** proof that
the driver violated its advertised long-term CBR rate.

## Scope of this patch

V514 does not change any output by default. For one exact source tile ID,
`DVBSTREAMER5_VAAPI_HRD_STREAM_ID` enables optional **direct VAAPI H.264**
CBR parameters:

- explicit `VAEncMiscParameterRateControl`: 6000-kbit/s (or configured
  bitrate), 400-ms rate-control window, 100% target
- explicit frame rate `VAEncMiscParameterFrameRate`, numerator/denominator
- explicit `VAEncMiscParameterHRD`: 400-ms coded picture buffer and 50%
  initial fullness; a 6-Mbit/s encoder requests 2.4 Mbit CPB
- log `NATIVE VAAPI STRICT HRD OVERSIZE` when a picture still exceeds
  requested CPB; it is **not** trimmed or dropped

The driver can ignore/miss the requested bound or reject the new VAAPI
misc parameters. This is experimental: if init fails or the tile stops,
remove the env override and restart the service in a controlled window.
The project does **not** claim hardware-independent per-picture guarantees.

The old VAAPI rate-control path, OpenH264, NVENC, QSV/VPL, Copy,
UDP-VBR, MPEG-TS mux, PCR/PTS/DTS and all other stream IDs remain unchanged.
WISI physical output target bitrate and multicast address are not changed.

## Deployment and A/B check (test host only)

After build/test/portable bundle and installation:

```bash
sudo mkdir -p /etc/systemd/system/dvbstreamer5.service.d
printf '[Service]\nEnvironment=DVBSTREAMER5_VAAPI_HRD_STREAM_ID=stream-1794627918823\n' |
    sudo tee /etc/systemd/system/dvbstreamer5.service.d/vaapi-hrd-v514.conf
sudo systemctl daemon-reload
# WARNING: service restart interrupts ALL tiles. Arrange a test-only maintenance window.
sudo systemctl restart dvbstreamer5.service
journalctl -u dvbstreamer5.service --since "-5 min" --no-pager -o cat |
    grep -E 'NATIVE HW ENCODER backend=vaapi-direct|NATIVE VAAPI STRICT HRD|NATIVE VIDEO ENCODE PERF|WISI CBR TS DIAG'
```

Capture 30 seconds of `236.9.0.7:9000` on `enp1s0f1`, analyze with
`/root/compare_ts_clock.py`, compare PCR median/P95/P99/max, max P-frame
bytes, 25 FPS, video/audio drops, WISI decoder observations and source
URL behavior. A shorter VAAPI CPB can reduce visual quality; also check
blocking/banding. Do not roll to home2 merely because V514 builds.

To revert the test and restore the previous VAAPI parameters:

```bash
sudo rm -f /etc/systemd/system/dvbstreamer5.service.d/vaapi-hrd-v514.conf
sudo systemctl daemon-reload
sudo systemctl restart dvbstreamer5.service
```

Rollback of the binary is separate; use the installed archive backup.

## Validation

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
ctest --test-dir build --output-on-failure
```

The pure C++ test `vaapi_hrd_policy` validates default-off selection,
exact-stream matching, 400-ms buffer arithmetic and oversize detection.
No live GPU/driver verification is included. Record the actual driver
acceptance and post-change PCAP before enabling it by default.
