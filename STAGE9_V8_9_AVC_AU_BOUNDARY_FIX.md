# Stage 9 V8.9 — AVC Main/B-frame live access-unit boundary fix

Observed live LVM input (ffprobe): H.264 Main, 720x576 progressive, 25 fps, GOP contains I/B/B/P pictures. Attaching mid-stream initially reports missing PPS and later recovers once parameter sets/random access arrive.

V8.9 keeps the already-working output H.264/OpenH264 and H.265/Kvazaar paths unchanged. It fixes only live input H.264 assembly:

- parses H.264 SPS and PPS state needed for slice-header interpretation;
- parses first_mb_in_slice, pic_parameter_set_id, frame_num, field flags, IDR id and POC fields;
- detects a new primary coded picture using AVC picture identity instead of `first_mb_in_slice == 0` alone;
- retains incomplete trailing Annex-B NAL data across PES boundaries;
- keeps original PES PTS/DTS whenever available;
- continues to use the V8.7 decoder IDR/SPS/PPS resync gate.

No FFmpeg/GStreamer/libav dependency is added. ffprobe was used only as an external diagnostic tool by the user.
