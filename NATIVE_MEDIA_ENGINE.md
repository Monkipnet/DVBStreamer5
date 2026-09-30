# DVBStreamer5 native media engine

DVBStreamer5 no longer links to or loads GStreamer.

Active native media paths:

- Linux DVB-S/S2 input
- UDP and RTP MPEG-TS input/output
- HTTP/HTTPS single-request MPEG-TS input
- HLS MPEG-TS input: master/media playlist parsing, variant selection, Header/Query credentials, AES-128 EXT-X-KEY
- HLS MPEG-TS output: PCR/random-access segmentation, atomic live playlist updates, optional DVR archive retention
- SRT caller/listener input and output with native reconnect, live/message API, TSBPD, streamid and passphrase/PBKEYLEN
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

HLS supports MPEG-TS media segments. fMP4/CMAF playlists using EXT-X-MAP and SAMPLE-AES are rejected with a clear error instead of falling back to an external media framework.

## Stage 8: source-built SRT 1.5.7 + OpenSSL EVP

SRT is no longer supplied by an installed runtime package or embedded shared object. Haivision SRT 1.5.7 is built directly from the vendored `third_party/srt` source tree as `srt_static` with `USE_ENCLIB=openssl-evp`. DVBStreamer5 links that static transport into the final executable and uses the SRT C API directly.

There is no SRT `dlopen`, `memfd` payload, GnuTLS/Nettle compatibility layer, external `libsrt*.so`, or `libsrt*-dev` build dependency. OpenSSL is the only SRT crypto backend and is the same OpenSSL already used by DVBStreamer5.
