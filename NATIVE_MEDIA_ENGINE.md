# Native media engine

DVBStreamer5 Stage 3 uses only the in-project transport engine for active media
paths. Build and runtime no longer require an external multimedia framework,
plugin scanner, plugin registry or media framework development headers.

Supported now: Linux DVB, UDP/RTP MPEG-TS, HTTP(S) MPEG-TS input, local TS file
input, TS remap/mux, CBR pacing, HTTP preview, MPTS and CA transport processing.

Later native stages will add video/audio codec processing and the remaining HLS,
SRT, RTSP and RTMP transports.
