# DVBStreamer5 V10.8.502 — TSDecrypt UI status with browser frame evidence

Home2 V501 showed amber “ДЕКОД: …” for multiple playable streams, and red
“ДЕКОД: НЕТ” on a stream whose HTTP/HLS preview played successfully.
The V501 server conservatively categorized all nominally clear TSDecrypt
output as unverified, while the preexisting output TS monitor treated any
scrambled non-PSI payload (including non-video PIDs) as a total loss.

V502 **changes CA indication only**:
- An output with unsuppressed clear PES and remaining scrambled packets on
  a TSDecrypt service is `partial`, not automatically `scrambled`.
- Unverified/partial status uses accurate amber “ДЕКОД: PES” or
  “ДЕКОД: ЧАСТ.” rather than displaying “ДЕКОД: ОК” without proof.
- The existing browser preview publishes a short-lived, per-tile signal
  only after actual presented video frames. While recent (15 seconds)
  and within the same tile generation, the UI can show green “ДЕКОД: ОК”
  with an explicit “confirmed by preview” explanation.
- Preview frame evidence is browser-local, does not alter CA/TS packets,
  and expires when playback stalls or the tile restarts. When nobody
  views the preview, the UI honestly falls back to PES status.

No media transcoder or output protocol is modified. No active preview
or decoded frames are needed for ordinary broadcasting. This is not
a remedy for CAM-reader denials or actual CW loss. A successful browser
preview confirms media playback, but does not assert that all downstream
protocol outputs are healthy.

See V501 notes for verified iCAM-binary portable packaging.
