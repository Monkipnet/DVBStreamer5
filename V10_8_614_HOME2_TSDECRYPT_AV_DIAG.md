# DVBStreamer5 V10.8.614 — home2 TSDecrypt audio/video diagnosis

## Scope

The home2 test service with stream ID sat-1794213530603-129-4
has persistent audio stutter and broken video even with transcoding OFF.
HLS vs HTTP has no effect. Newcamd is not available/allowed on home2.
Do not change NVDEC/NVENC, ECM PID, CAMD credentials, source, or transport
timing while collecting this evidence.

Previous V612 data: MPEG-TS before CA had 0 CC errors; after CA had
37 historical errors (31 adaptation-only), but no new errors during
later observations. TSDecrypt input/output drops and its NULL-underrun
counts also remained stable during the same windows. These numbers do
not prove that decrypted A/V PES data is correct.

V614 is diagnostic only. It changes no payload, CW, ECM, PCR, PTS,
audio codec, decryption algorithm or source buffering. Only one target
stream is traced. No transport payload, CW, ECM or credentials are
written to the logs.

## Enable on home2 (brief service interruption)

    sudo mkdir -p /etc/systemd/system/dvbstreamer5.service.d
    sudo tee /etc/systemd/system/dvbstreamer5.service.d/98-tsdecrypt-av-v614.conf >/dev/null <<'EOF'
    [Service]
    Environment="DVBSTREAMER5_TS_AV_TRACE_STREAM_ID=sat-1794213530603-129-4"
    Environment="DVBSTREAMER5_TS_AV_VIDEO_PID=767"
    Environment="DVBSTREAMER5_TS_AV_AUDIO_PID=770"
    Environment="DVBSTREAMER5_TS_CC_TRACE_STREAM_ID=sat-1794213530603-129-4"
    EOF
    sudo systemctl daemon-reload
    sudo systemctl restart dvbstreamer5.service

Collect 5–10 minutes while reproducing playback (the target tile may
be configured for direct MPEG-TS copy, not transcoding). Print only
metadata. Do not share CAMD command-line arguments or raw TSDecrypt
stderr; these may contain credentials, ECMs or control words.

    SID='sat-1794213530603-129-4'
    journalctl -u dvbstreamer5.service --since '-10 minutes' \
      --no-pager -o cat |
      grep -E 'TS AV STAGE DIAG|TSDECRYPT FIFO DIAG|CC STAGE DIAG|PCR STAGE|TSDECRYPT DIAG' |
      grep -F "stream=$SID" |
      tail -n 220

If output is too long, filter the two stage lines separately:

    journalctl -u dvbstreamer5.service --since '-10 minutes' \
      --no-pager -o cat |
      grep -F "TS AV STAGE DIAG stream=sat-1794213530603-129-4" |
      tail -n 90

## Interpretation

- pre_ca vs post_ca: scrambled input PUSIs cannot be validated as
  clear PES. Compare continuity, transport PID counts and any downstream
  invalid PES on video PID 767 and audio PID 770.
- private1_pes: clear PES packets with stream_id=0xBD, often used
  by AC-3/E-AC-3 audio. Not proof of successful audio decoding.
- bad_pes_delta: clear PUSI on the target PID without an expected
  PES prefix/stream ID. A sustained increase calls for targeted
  capture of stage-boundary TS.
- pts_forward_over200: source PES PTS gap >200ms, not an output
  rendering gap. Video B frames may have reordered PTS; pts_backward
  on video is not automatically an error.
- wall_gap_over200: a target PID received no packets at the observed
  stage for >200ms. Can indicate starvation but may be source timing.
- TSDECRYPT FIFO DIAG: input/output queue occupancy and peak,
  last child output age, count of child packets still scrambled and
  replaced with NULL, private1 PES count, and NULLs synthesized by
  the parent because the output queue was empty.
- PCR STAGE: inspect tracked PCR changes, gaps and reset events.
- input_drops=0 and output_drops=0 do not prove usable media.

A/B against the other healthy Newcamd server is observational only;
Newcamd must not be enabled on home2.

## Disable

    sudo rm -f /etc/systemd/system/dvbstreamer5.service.d/98-tsdecrypt-av-v614.conf
    sudo systemctl daemon-reload
    sudo systemctl restart dvbstreamer5.service

## CI limits

- tests/test_ts_av_stage_v614.cpp emulates video H.264 PES,
  private-stream (0xBD) audio PES, missing PES headers, scrambled
  input, and audio PTS gaps without DVB hardware.
- tests/web/test_tsdecrypt_stage_v614.js checks connection of
  all three trace stages and per-service FIFO telemetry.
- Full Linux build, media CTest and portable must pass separately.
- Only actual home2 playback can establish whether A/V stutter is fixed.
