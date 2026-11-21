# DVBStreamer5 V10.8.617 — independent DVB DVR ring buffer

## Reproduced problem (home2, 2026-11-21)

On home2 (Linux 5.4, Linux4Media cineS2 v5 / ngene), several frontend
DVR instances reported `NATIVE DVB DVR overflow recovered`. CC gaps
were then visible at the `pre_ca` stage of channel
`sat-1794213530603-129-4` (adapter2/frontend0), and the decoded video
occasionally broke up. Audio and video issues were present through both
HTTP MPEG-TS and HLS; switching output protocols did not fix them.

## Root cause candidate and bounded fix

Linux DVB `DMX_SET_BUFFER_SIZE` operates on the file descriptor to which
it is sent. Prior to V617 the application applied it to `demux0` filter
descriptors, but never to the `dvr0` descriptor read by the shared source
for `DMX_OUT_TS_TAP`. The Linux 5.4 DVB core initializes the DVR ring to
`DVR_BUFFER_SIZE = (10*188*1024)` bytes, independently of the filter
ring. V617 requests 8 MiB specifically on the open DVR descriptor.

If the ioctl fails, the input continues with the driver's buffer and logs
a warning including adapter, frontend, device, errno and requested bytes.
Successful requests are also logged. EOVERFLOW events now identify the
adapter/frontend. Existing TSDecrypt, CW, outputs, and DVB shared source
scheduling are unchanged.

## CI verification

`tests/test_dvb_dvr_buffer_v617.cpp` injects a mock ioctl, asserts the
correct request/size/fd on four independent DVR descriptors, and tests
ENOTTY fallback and invalid descriptors. A source-wiring check in
`tests/test_dvb_dvr_buffer_v617.py` prevents tests from passing if the
real input no longer calls the helper. The checks run in
`.github/workflows/build-main-linux.yml`.

**CI emulation is not hardware validation.** After deploying V617 on the
non-production home2, inspect the log for four successful DVR resize
events (one per adapter) and compare overflow/CC counts over comparable
playback periods. A successful ioctl is not proof that no future packet
loss is possible.

## Runtime diagnostics (home2)

```bash
journalctl -u dvbstreamer5.service --since '-30 minutes' --no-pager |
  grep -E 'NATIVE DVB DVR buffer resize|NATIVE DVB DVR overflow recovered|CC STAGE DIAG:'
```
