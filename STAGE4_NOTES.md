# Stage 4 — native HLS

Stage 4 restores HLS without GStreamer or another media framework.

## Native HLS input

- master and media M3U8 parsing
- BANDWIDTH-based variant selection using stream target bitrate
- live and VOD playlists
- Header or Query access keys already present in StreamConfig
- per-stream User-Agent
- AES-128 EXT-X-KEY decryption through the project's existing OpenSSL dependency
- bounded ahead scheduling and playlist refresh
- MPEG-TS byte feed into NativeUdpRelay

Unsupported HLS input formats are rejected explicitly: fMP4/CMAF EXT-X-MAP and SAMPLE-AES.

## Native HLS output

- 188-byte MPEG-TS segmentation
- PCR-driven duration
- random_access_indicator preferred as segment cut boundary
- atomic video.m3u8 replacement
- EXT-X-PROGRAM-DATE-TIME
- live-window cleanup
- existing DVR archive directory/retention support
- existing HttpServer serves playlist and .ts files

## Verification

The main DVBStreamer5 target builds without GStreamer. CTest covers media core, native HLS input/output, and MP2. Manual integration was also checked with ffmpeg-generated H.264/AAC HLS, AES-128 HLS input, and ffprobe parsing of the native HLS output.
