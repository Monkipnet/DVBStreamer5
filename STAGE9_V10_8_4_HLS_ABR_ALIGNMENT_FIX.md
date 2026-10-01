# Stage 9 V10.8.4 — native HLS ABR resolution alignment

Fixes the native HLS multibitrate ladder failing to start the 480p HEVC rendition with Kvazaar:

`Kvazaar requires width and height divisible by 8 (got 854x480)`

Changes:
- generated ABR coded dimensions are aligned to an 8-pixel boundary;
- the nominal `854x480` rendition becomes `856x480`;
- bitrate scaling and the HLS master playlist use the actual aligned dimensions;
- no FFmpeg, GStreamer or libav dependency is introduced.

This fix only changes generated lower ABR renditions. Primary/user-selected transcode dimensions are left unchanged.
