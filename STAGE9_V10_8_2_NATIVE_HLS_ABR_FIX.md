# Stage 9 V10.8.2 — Native HLS multibitrate ABR fix

V10.8.2 fixes the previously incomplete Multibitrate HLS switch. The UI and public URL already selected `master.m3u8`, but runtime created only the primary `video.m3u8`; no lower native renditions or master playlist were produced.

Changes:

- adds a post-remap/post-CA MPEG-TS input tap before the primary transcoder;
- creates up to three independent lower native transcoder pipelines from the same input;
- creates one NativeHlsSegmenter per lower rendition under `abr/<profile>/`;
- writes a real HLS master playlist with BANDWIDTH, AVERAGE-BANDWIDTH, RESOLUTION and CODECS;
- primary rendition remains `video.m3u8` and keeps all existing non-HLS outputs unchanged;
- lower renditions are HLS-only and do not duplicate UDP/SRT/RTSP/RTMP outputs;
- no FFmpeg, GStreamer or libav production dependency was added;
- multibitrate currently targets MPEG-TS HLS. CMAF ABR is rejected explicitly instead of silently producing a broken master.

Automatic ladder uses the configured primary rendition plus up to three lower profiles selected from 1080p, 720p, 480p and 360p without upscaling above the primary resolution. Variant bitrates are derived from the primary bitrate and bounded by profile nominal targets.

Expected log lines:

    NATIVE HLS ABR RENDITION init name=720p size=1280x720 ...
    NATIVE HLS ABR SEGMENTER start name=720p ...
    NATIVE HLS ABR MASTER ready variants=... path=.../master.m3u8

Expected files:

    master.m3u8
    video.m3u8
    abr/720p/video.m3u8
    abr/480p/video.m3u8
    abr/360p/video.m3u8

The exact lower profiles depend on the configured primary resolution.
