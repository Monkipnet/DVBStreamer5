# DVBStreamer5 native media engine

DVBStreamer5 does not use GStreamer, FFmpeg, libavcodec, libavformat, libavfilter,
libswscale or libswresample.

## Native transport paths

- Linux DVB-S/S2 input.
- UDP and RTP MPEG-TS input/output.
- HTTP/HTTPS MPEG-TS input and HTTP MPEG-TS preview/output.
- HLS MPEG-TS input/output with AES-128 and SAMPLE-AES.
- HLS fMP4/CMAF input/output with `EXT-X-MAP` (`init.mp4`) and `.m4s` fragments.
- CMAF SAMPLE-AES/cbcs encryption/decryption using OpenSSL.
- SRT caller/listener input/output using vendored SRT 1.5.7 built statically with OpenSSL EVP.
- RTSP input: native RTSP client, RTP over TCP interleaved or UDP; MPEG-TS,
  H.264, H.265 and AAC RTP payloads are converted to the common MPEG-TS pipeline.
- RTSP output: embedded RTSP server with DESCRIBE/SETUP/PLAY/TEARDOWN and
  RTP/MP2T over TCP interleaved or UDP.
- RTMP/RTMPS input: native handshake/chunk/AMF0/FLV client with H.264/AAC -> MPEG-TS.
- RTMP/RTMPS output (including YouTube-style RTMP publish): native
  handshake/chunk/AMF0 client with MPEG-TS H.264/AAC -> FLV messages.
- MPEG-TS service/PID remap, PAT/PMT/SDT generation, PES/PTS/DTS/PCR and native mux.
- CBR pacing, preview fan-out, MPTS and CA/OSCam transport hook.

## Native transcoder

Stage 9 restores the in-process transcoder without a media framework.

Production codec backends are project-local static libraries:

- H.264 decode/encode: OpenH264 2.6.0.
- H.265/HEVC decode: libde265 1.1.3.
- H.265/HEVC encode: Kvazaar 2.3.2.
- AAC-LC decode/encode: FDK-AAC 2.0.3.
- MPEG-1 Layer II decode: PL_MPEG snapshot pinned in `NATIVE_CODEC_SOURCE_LOCK.txt`.
- MPEG-1 Layer II encode: the existing vendored TwoLAME core.

Raw video processing is owned by DVBStreamer5: I420 bilinear scaling and blend
field deinterlacing. PCM16 sample-rate/channel conversion is also native C++.

Current codec limits are deliberate: 8-bit 4:2:0 video is the supported raw
video format; FDK-AAC output is AAC-LC and Stage 9 audio output is mono/stereo;
PL_MPEG is an MP2 decoder, not an MP3 decoder. Frame-rate conversion is not a
separate filter yet: the encoder FPS setting controls encoder timing/config,
while decoded frames are processed one-for-one.

## Build model

`install_deps.sh` vendors locked SRT and codec source snapshots, then builds the
codec libraries into `third_party/native-codecs-prefix`. Normal CMake builds use
only those static codec inputs. There is no system-SRT or runtime-codec fallback in the production tree.
Normal builds require the locked vendored SRT 1.5.7 source and the statically
built codec prefix produced by the project scripts.
