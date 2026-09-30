# DVBStreamer5 native media engine

DVBStreamer5 no longer links to or loads GStreamer.

Stage 4 active native media paths:

- Linux DVB-S/S2 input
- UDP and RTP MPEG-TS input/output
- HTTP/HTTPS single-request MPEG-TS input
- HLS MPEG-TS input: master/media playlist parsing, variant selection, Header/Query credentials, AES-128 EXT-X-KEY
- HLS MPEG-TS output: PCR/random-access segmentation, atomic live playlist updates, optional DVR archive retention
- local MPEG-TS file input for paced CBR output
- MPEG-TS service/PID remap, PAT/PMT/SDT generation and native mux
- CBR pacing and RTP packetization
- native HTTP MPEG-TS preview fan-out
- MPTS aggregation and CA transport hook

Still disabled until native implementations are added:

- video/audio transcoding
- RTSP
- RTMP / YouTube
- generated test-pattern media

HLS Stage 4 intentionally supports MPEG-TS media segments. fMP4/CMAF playlists using EXT-X-MAP and SAMPLE-AES are rejected with a clear error instead of falling back to an external media framework.

Stage 5 adds native SRT input/output using the SRT runtime ABI with no development headers or link-time dependency.

## Stage 6: embedded SRT runtime

The final DVBStreamer5 ELF contains the SRT 1.5 runtime payload as a linked binary resource. NativeSrtTransport loads it from Linux memfd; no external libsrt file or package is required on the deployed host. Source builds use an SRT 1.5 runtime only as a build-time payload. CMake embeds it directly; `scripts/vendor_srt_runtime.sh` is optional for offline/reproducible builds.
