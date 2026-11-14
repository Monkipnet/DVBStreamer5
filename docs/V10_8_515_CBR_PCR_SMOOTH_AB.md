# DVBStreamer5 V10.8.515 — opt-in physical-slot PCR smoothing

## Diagnosis (svettranscoding, MIR24_TRANS)

V514 direct-VAAPI H.264, 1920x1080p25, video 6Mbps, WISI transport CBR
8Mbps, original PCR on first video PES packet. The user captured 5 minutes
of HRD-enabled UDP at `236.9.0.7:9000`. 7,496 PCR intervals, 18 residual
outliers >80ms, 6 >100ms, peak 127.14ms; 893 useful / 0 NULL transport
packets between two PCRs that were only 40ms apart in the source clock.
The 893 packets took 167ms to transmit over the fixed 8Mbps wire.
Neither a larger backlog nor sending faster than 8Mbps can resolve this.

V514 HRD enabled explicitly via
`DVBSTREAMER5_VAAPI_HRD_STREAM_ID=stream-1794627918823` produced
a 162.1KiB max video PES in the five-minute sample; no >200KiB PES
observed. This does **not** prove HRD guarantees a per-picture size cap.

## V515 experimental transport behavior

**Default off.** Select one transcoded, buffered Source-PCR WISI UDP-CBR
tile using:

```ini
[Service]
Environment="DVBSTREAMER5_CBR_PCR_SMOOTH_STREAM_ID=stream-1794627918823"
```

A CbrTsPacer boolean enables smoothing only when ALL conditions hold:

- matching exact stream ID
- transcoded transport path and UDP-CBR output
- Source-PCR encoded CBR target, with nonzero startup reservoir
- Source-PCR useful-packet pacing is actually enabled

It makes *no changes* to DVB/copy, Fixed PCR, VBR, no-buffer profiles,
HTTP/SRT/HLS observer pacing, non-selected tiles or raw media samples.

For that selected CBR output only:

1. Original source-PCR-bearing TS packet has PCR restamped to **physical
   188-byte output slot time** relative to the first source PCR for its PID.
2. Where queued media is present, a new **adaptation-only** packet is
   emitted on the reference PCR PID at most once every **20 ms** of output
   CBR slots. It repeats the last transmitted **payload** continuity
   counter. It has no PUSI, no payload, no discontinuity.
3. The source-PES packets are never popped, reordered, corrupted or
   dropped by smoothing; a clock-only packet takes one of the physical
   CBR transport slots instead of its previous NULL/useful schedule.
4. The source useful-packet scheduler remains unchanged and the physical
   UDP datagram layout stays 7x188 / fixed 8Mbps.
5. Source PTS/DTS bytes remain **unchanged**. Consequently their distance
   to the new physical PCR can vary and **must** be checked on the WISI
   decoder, especially for a frame larger than the 500ms decode preroll.

The mode is intentionally **not a general standards-certified PCR/jitter
or decoder-buffer repair**. It does not alter the bitrate or quality of
the video encoder. Discontinuities retain their source flags. It is
a reversible A/B transport experiment, not a production default.

## Cross-encoder audit (code inspection, not live GPU validation)

- Direct VAAPI H.264: `VA_RC_CBR`, V514 optional 400ms HRD per selected
  tile, may be ignored or undershot by device driver.
- Intel QSV/oneVPL H.264/HEVC: `MFX_RATECONTROL_CBR`,
  `BufferSizeInKB` configured for roughly **2 seconds** and half-full
  initial delay. Large pictures are still possible.
- NVIDIA NVENC CUDA H.264/HEVC and NVIDIA zero-copy: NVENC CBR, average
  and max target set to configured bitrate, VBV typically **2 frames**
  (plus optional NVENC quality A/B). Actual enforcement is GPU-dependent.
- CPU OpenH264 H.264: `RC_BITRATE_MODE`, `iTargetBitrate=iMaxBitrate`,
  `bEnableFrameSkip=false`; average bitrate is not a hard per-AU cap.
- CPU Kvazaar HEVC: bit-rate lambda rate control; per-picture peaks
  require measurement. Copy streams bypass the transcoder and are NOT
  modified by the V515 pilot.

All codecs that supply `NativeTranscoderPipeline::writeMuxSample` use
the same MPEG-TS mux rule: PCR on the first packet of each video PES.
Thus the *transport* vulnerability is backend-independent. Existing
`DVBSTREAMER5_DIAG_VIDEO_BURSTS_STREAM_ID=<ID>` now logs both largest
random-access and largest non-random-access AU, 100ms/1s peaks and
a reference payload-only transmission time at 8Mbps. One backend/stream
can be inspected at a time with the exact stream-ID selector.

## Test and validation

Hardware-free unit test:
`ctest --test-dir build -R 'cbr_output_pcr_smoothing|source_pcr_packet_pacing|transcode_cbr_single_shaper' --output-on-failure`.

`cbr_output_pcr_smoothing` simulates 120 H.264-like 40ms PES intervals
including a **900-packet P-frame**, verifies that every useful input
transport packet survives in its original order, with its CC, PES and
PTS bytes unchanged; checks synthesized PCR-only AFC=2 CC repetition
and physically timed output PCR intervals.

A/B on *svettranscoding only*:

1. Build/release, preserve V514 portable archive and service backup.
2. Keep the V514 VAAPI HRD selector unchanged for both A/B legs.
3. Record baseline 30- or 300-second PCAP with PCR smoother **off**.
4. Add selector, daemon-reload and restart **only during maintenance**;
   restart interrupts **every** tile served by that instance.
5. Expect log:
   `NATIVE CBR OUTPUT PCR SMOOTH ... experimental=1`,
   `WISI CBR start ... pcr_smoothing_test=1`.
6. Capture same-duration PCAP to
   `/root/mir24-test/v515-cbr-pcr-smooth.pcap`.
7. Compare PCR residual P95/P99/max, PCR backwards, CC, TEI,
   video PTS intervals, PTS-PCR and **DTS-PCR min** with
   `DVBSTREAMER5_WISI_CBR_TS_DIAG=1` and
   `DVBSTREAMER5_WISI_PTS_PCR_DIAG=1`.
8. Observe WISI actual video/audio, artifacts, stalls, delay, quality.
   Abort and roll back if video gets behind PCR, if A/V drifts, or if
   continuity/clock resets are observed. Merely getting low PCR jitter
   does not prove decoder-buffer safety.

Rollback the **transport pilot** without reinstall:
```bash
sudo rm -f /etc/systemd/system/dvbstreamer5.service.d/cbr-pcr-v515.conf
sudo systemctl daemon-reload
sudo systemctl restart dvbstreamer5.service
```

The V514 VAAPI HRD selector can remain installed separately.
Do **not** deploy the pilot to `home2` until the WISI PCAP and
decoder A/B succeed. Do not enable it by default on every encoder.
