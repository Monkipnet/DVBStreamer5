# DVBStreamer5 V10.8.615 — find cause of persistent SID 129 video/audio PES defects

## Grounded findings from home2 V614

Stream sat-1794213530603-129-4, with transcoding disabled:
- Before CA video PID 767 and audio PID 770 were encrypted, so their
  PES signatures could not be examined.
- After CA, clear PUSI packets without the expected PES structure
  occurred continuously on BOTH video and audio PIDs.
- Over five V614 reporting windows, video bad PES count rose 70 -> 98
  (+28), audio 11 -> 14 (+3).
- post_ca and pre_output counters matched, so the observed malformed
  PES headers were present before the egress protocol split.
- Eight historical CC errors already existed before CA on PID 6842;
  CC_delta stayed zero in these windows. Do NOT misattribute these
  to video PID 767 or audio PID 770.
- The async TSDecrypt FIFO had no ongoing drops, output-null underruns,
  or child-output latency surge in the samples. Its initial suppressed
  scrambled/NULL counts were stable.
- Do not assert corruption is caused by a wrong CW until malformed PES
  reasons are separated and the source TS is compared.
- No Newcamd can be used on home2. Do not touch NVDEC/NVENC.

## New V615 diagnostics (metadata only)

The existing opt-in TS AV STAGE DIAG adds the following fields:
- bad_pes_prefix_delta: expected video/audio PES PUSI with no
  00 00 01 signature after adaptation field.
- bad_pes_stream_delta: PES signature exists, but stream_id unexpected
  for target video/audio PID.
- bad_pes_short_delta: expected PUSI PES header truncated within TS.
- bad_pes_adaptation_delta: invalid adaptation length for AFC=3.
- scramble_even_delta / scramble_odd_delta: observed encrypted TS packet
  parities on pre_ca (not code words).
- scramble_reserved_delta: reserved TS scrambling_control value 01.

The bad-pes reason deltas are mutually exclusive and sum to
bad_pes_delta, per stage/PID. The probe never records ES payload,
encrypted transport content, CW, ECM bytes, passwords, or CAMD endpoints.

## On home2 after installing portable V615

If the existing v614 unit drop-in is still installed, retain it:

    systemctl show -p Environment dvbstreamer5.service | sed -E 's/(PASS|KEY|TOKEN)=[^ ]+/\1=REDACTED/g'

Avoid displaying secrets in shared diagnostics. If necessary recreate
98-tsdecrypt-av-v614.conf with ONLY these four environment variables:

    DVBSTREAMER5_TS_AV_TRACE_STREAM_ID=sat-1794213530603-129-4
    DVBSTREAMER5_TS_AV_VIDEO_PID=767
    DVBSTREAMER5_TS_AV_AUDIO_PID=770
    DVBSTREAMER5_TS_CC_TRACE_STREAM_ID=sat-1794213530603-129-4

Collect 5 minutes of copied (non-transcoded) MPEG-TS playback:

    journalctl -u dvbstreamer5.service --since '-5 minutes' \
      --no-pager -o cat |
    grep -F 'TS AV STAGE DIAG stream=sat-1794213530603-129-4' |
    grep -E 'stage=(pre_ca|post_ca|pre_output)' |
    tail -n 36

    journalctl -u dvbstreamer5.service --since '-5 minutes' \
      --no-pager -o cat |
    grep -F 'TSDECRYPT FIFO DIAG stream=sat-1794213530603-129-4' |
    tail -n 15

## Interpretation

If both video and audio report repeated bad_pes_prefix_delta but no
PES stream-ID/adaptation failures, suspect media payload that is not
validly decrypted or transport PES bytes altered by a child process.
Inspect optional CSA-ALT status and decrypted TS directly; never infer
key validity from a cleared TS scrambling flag alone.

If bad_pes_stream_delta dominates, check PID remap / service selection
and TSDecrypt output stream ID mapping.

If bad_pes_short_delta or bad_pes_adaptation_delta dominates,
investigate TS packet framing and adaptation field handling.

If no new bad_pes_delta but playback still fails, next isolate
PCR/PTS pacing, HTTP/HLS delivery and client buffer handling with
simultaneous server-side output captures.

V615 does not change CSA, media packets, CW, PCR, or playback timing.
CI emulates metadata classes; real decryption quality requires home2.
