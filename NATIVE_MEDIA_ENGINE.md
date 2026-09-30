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

## Stage 7: embedded SRT with OpenSSL crypto compatibility

The final DVBStreamer5 ELF contains the SRT 1.5 payload plus two minimal OpenSSL-backed compatibility modules for the GnuTLS/Nettle ABI expected by a GnuTLS-flavoured SRT build. NativeSrtTransport loads all three objects from Linux memfd. The deployed host therefore requires no external libsrt, libgnutls or libnettle runtime. The compatibility surface is deliberately limited to the seven crypto symbols used by SRT 1.5.x: RNG, AES key setup/block operations, CTR mode and PBKDF2-HMAC-SHA1.
