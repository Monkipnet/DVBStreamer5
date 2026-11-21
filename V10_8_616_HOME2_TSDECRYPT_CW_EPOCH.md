# DVBStreamer5 V10.8.616 — diagnose 7.2-second A/V decrypt blackouts

## Home2 observations, 2026-11-21

The user cannot run Newcamd on home2. SID 129 is kept on
its current TSDecrypt/CAMD configuration with transcode disabled.
HTTP and HLS exhibit the same persistent stuttering/mosaic.

V615 logs after CA, video PID 767 and audio PID 770:

- First clean period: no invalid PES headers on either PID.
- In two consecutive reporting windows, **362 video** and **44 audio**
  clear PES PUSIs became invalid. Every reason was
  bad_pes_prefix; none was wrong stream ID, truncated header or
  malformed adaptation.
- Valid PES returned after ~7.2 seconds: max positive video PTS jump
  7,260 ms; audio PTS jump 7,200 ms.
- During that interval the packet wall arrival gap stayed <=39 ms.
  FIFO input/output drops stayed zero, output-null-underrun and
  child-scrambled-suppressed stayed flat in nearby snapshots.
- The clear TS scrambling bit does not prove correct CSA output.
- Video PTS reordering due to B frames is not proof of packet loss.
- Source CC errors on unrelated PID 6842 do not explain this window
  on media PIDs 767 and 770.

This strongly suggests a temporary failure to decrypt media data
during a crypto period, but does not prove whether CAMD returned a late/
incorrect CW, an incorrect ECM/iCAM mode was selected, or CSA applies
a wrong parity CW. Do NOT blindly change the crypto engine.

## V616 changes

The pinned TSDecrypt builder emits one strictly numeric and key-free
marker after each valid CW is applied to the CSA schedule:

    DVB5 | CW_APPLIED mode=<number> even=<0|1> odd=<0|1>

The DVBStreamer5 parent process allowlist-parses only this marker from
the child stderr and logs for the exact stream configured by the
existing opt-in DVBSTREAMER5_TS_AV_TRACE_STREAM_ID. The raw stderr
is NEVER passed through: it could contain secrets. No CW values, CW
hashes, ECM payload, credentials or CAMD endpoints are emitted or
retained as new diagnostic data.

A parent log event will look like:

    TSDECRYPT CW APPLY stream=sat-1794213530603-129-4 count=7 even=1 odd=0 mode=2 diagnostic_only=1

This is an EXAMPLE of format, not a home2 result.

Every 15s, existing TSDECRYPT FIFO DIAG also shows the numeric
counters cw_applied, cw_even_applied, cw_odd_applied,
cw_nonzero_mode_applied and cw_last_applied_age_ms.

This does NOT change ECM/CW acquisition, decryption, source/CA
buffering, encryption flags, media payload, PCR/PTS, transcoding
or output protocols. It remains diagnostic only.

## Collection on home2 after installing a successful V616 CI artifact

Retain existing /etc/systemd/system/dvbstreamer5.service.d/
98-tsdecrypt-av-v614.conf (video PID 767, audio PID 770, SID 129).
No Newcamd changes or credentials.

Use journal timestamps to correlate errors and CW updates:

    SID='sat-1794213530603-129-4'
    journalctl -u dvbstreamer5.service --since '-10 minutes' \
      --no-pager -o short-iso-precise |
    grep -E 'TSDECRYPT CW APPLY|TSDECRYPT FIFO DIAG|TS AV STAGE DIAG' |
    grep -F "stream=$SID" |
    grep -E 'CW APPLY|FIFO DIAG|stage=pre_ca|stage=post_ca' |
    tail -n 150

For a shorter view:

    journalctl -u dvbstreamer5.service --since '-10 minutes' \
      --no-pager -o short-iso-precise |
    grep -E 'TSDECRYPT CW APPLY stream=sat-1794213530603-129-4|TSDECRYPT FIFO DIAG stream=sat-1794213530603-129-4' |
    tail -n 45

Interpret only when time windows match:

- If the error episode begins on the input even/odd parity transition
  and no corresponding fresh CW is applied until its end, suspect
  delayed/missing CW.
- If a CW is applied promptly and the new parity still yields bad
  PES prefixes, investigate ECM mode/CSA application.
- If CW update and corrupted PES are unrelated, capture protected TS
  boundary traces in a controlled environment and avoid guessing.
- CW applied count is an installation count, NOT proof of correct
  decryption or distinct CW content.

CI unit tests validate numeric parser and instrumented code paths;
real VideoGuard/iCAM decryption requires test hardware and is NOT
emulated in GitHub Actions.
