#!/usr/bin/env bash
set -euo pipefail

# DVBStreamer5 GStreamer capability check.
# Core transcoder elements are mandatory. Input/output protocol elements are
# reported separately because a deployment may intentionally use only a subset.

if ! command -v gst-inspect-1.0 >/dev/null 2>&1; then
  echo "In-process transcoding is unavailable: missing gst-inspect-1.0" >&2
  exit 1
fi

required=(
  parsebin
  decodebin
  queue
  videoconvert
  deinterlace
  videoscale
  capsfilter
  h264parse
  h265parse
  audioconvert
  audioresample
  audiorate
  aacparse
  appsrc
  appsink
)

missing=()
for element in "${required[@]}"; do
  if ! gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    missing+=("$element")
  fi
done

aac_encoder=""
for element in voaacenc fdkaacenc avenc_aac; do
  if gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    aac_encoder="$element"
    break
  fi
done

x264_encoder=""
if gst-inspect-1.0 x264enc >/dev/null 2>&1; then
  x264_encoder="x264enc"
fi
x265_encoder=""
if gst-inspect-1.0 x265enc >/dev/null 2>&1; then
  x265_encoder="x265enc"
fi

nvenc_encoder=""
if gst-inspect-1.0 nvh264enc >/dev/null 2>&1; then
  nvenc_encoder="nvh264enc"
fi
nvenc_hevc_encoder=""
if gst-inspect-1.0 nvh265enc >/dev/null 2>&1; then
  nvenc_hevc_encoder="nvh265enc"
fi

if [[ -z "$x264_encoder" && -z "$nvenc_encoder" ]]; then
  missing+=("H.264 encoder: nvh264enc or x264enc")
fi

mp3_encoder=""
for element in lamemp3enc avenc_mp3; do
  if gst-inspect-1.0 "$element" >/dev/null 2>&1; then
    mp3_encoder="$element"
    break
  fi
done

if [[ -z "$aac_encoder" ]]; then
  missing+=("AAC encoder: voaacenc, fdkaacenc or avenc_aac")
fi

if ((${#missing[@]} > 0)); then
  echo "GStreamer transcoding is unavailable. Missing required elements:" >&2
  printf '  - %s\n' "${missing[@]}" >&2
  echo "Install plugins base/good/bad/ugly, gstreamer1.0-libav and gstreamer1.0-tools." >&2
  exit 1
fi

input_elements=(
  "http/https:souphttpsrc"
  "hls:souphttpsrc hlsdemux"
  "udp:udpsrc"
  "rtp:udpsrc rtpmp2tdepay"
  "srt:srtsrc"
  "rtsp:rtspsrc"
  "rtmp:rtmpsrc"
)

transcoded_output_elements=(
  "udp/udp-cbr/udp-vbr:appsink"
  "rtp:rtpmp2tpay udpsink"
  "http:tcpserversink"
  "hls:hlssink"
  "srt:srtsink"
  "rtmp/youtube:tsparse tsdemux flvmux rtmpsink"
  "rtsp-push:tsparse tsdemux rtspclientsink"
)

passthrough_remap_elements=(
  "generic-remap:tsparse tsdemux mpegtsmux"
  "hls-passthrough:tsparse tsdemux hlssink2"
)

print_group() {
  local title="$1"
  shift
  local entries=("$@")
  echo "$title"
  local entry protocol elements ok element
  for entry in "${entries[@]}"; do
    protocol="${entry%%:*}"
    elements="${entry#*:}"
    ok=true
    local missing_list=()
    for element in $elements; do
      if ! gst-inspect-1.0 "$element" >/dev/null 2>&1; then
        ok=false
        missing_list+=("$element")
      fi
    done
    if $ok; then
      printf '  [ok]      %s\n' "$protocol"
    else
      printf '  [missing] %s -> %s\n' "$protocol" "${missing_list[*]}"
    fi
  done
}

echo "DVBStreamer5 Stage 2 transcoder core is available."
echo "  external gst-launch: disabled"
echo "  MPEG-TS mux: native (PAT/PMT/SDT/PES/PCR/CBR)"
echo "  transcoder mpegtsmux/tsparse/udpsink dependency: removed"
echo "  Auto video encoder: ${nvenc_encoder:-${x264_encoder:-not available}}"
echo "  NVIDIA NVENC: ${nvenc_encoder:-not available}"
echo "  CPU x264: ${x264_encoder:-not available}"
echo "  HEVC NVENC: ${nvenc_hevc_encoder:-not available}"
echo "  CPU x265: ${x265_encoder:-not available}"
echo "  AAC encoder: ${aac_encoder}"
echo "  MP3 encoder: ${mp3_encoder:-not available}"
echo
print_group "Input protocol elements:" "${input_elements[@]}"
echo
print_group "Transcoded output protocol elements:" "${transcoded_output_elements[@]}"
echo
print_group "Passthrough/remap-only GStreamer elements:" "${passthrough_remap_elements[@]}"
