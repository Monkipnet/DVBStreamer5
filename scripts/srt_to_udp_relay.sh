#!/usr/bin/env bash
# Diagnostic SRT -> raw UDP bridge. The input must already be CBR MPEG-TS.
# FFmpeg's data demuxer/muxer preserves TS bytes, including NULL and PCR.
set -euo pipefail

relay_input=''
relay_output=''
relay_interface=''
relay_bitrate=5000000
relay_ffmpeg=ffmpeg

usage() {
    cat <<'USAGE'
Usage: bash srt_to_udp_relay.sh --input SRT_URL --output udp://IP:PORT [options]

  --interface IP    Local IPv4 address for UDP output (recommended on dual NICs)
  --bitrate BPS     Match the existing CBR transport rate (default 5000000)
  --ffmpeg PATH     FFmpeg executable (default ffmpeg)

FFmpeg SRT latency is in microseconds: latency=1500000 means 1500 ms.
The relay preserves source TS bytes and caps UDP sending at the specified rate.
It does not add NULL packets during input stalls. Downstream UDP has no ARQ.
Use a separate multicast group from the original sender for this test.
Stop with Ctrl+C. No application configuration or system routes are changed.
USAGE
}
fail() { printf '%s\n' "$*" >&2; exit 1; }
while (($#)); do
    case "$1" in
        --input|--output|--interface|--bitrate|--ffmpeg)
            (($# >= 2)) || fail "$1 needs a value"
            case "$1" in
                --input) relay_input="$2" ;;
                --output) relay_output="$2" ;;
                --interface) relay_interface="$2" ;;
                --bitrate) relay_bitrate="$2" ;;
                --ffmpeg) relay_ffmpeg="$2" ;;
            esac
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "Unknown option: $1" ;;
    esac
done
[[ "$relay_input" == srt://* ]] || fail '--input must be an srt:// URL'
[[ "$relay_output" =~ ^udp://@?([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+):([0-9]+)$ ]] || fail '--output must be udp://IPv4:PORT without query options'
relay_host="${BASH_REMATCH[1]}"
relay_port="${BASH_REMATCH[2]}"
[[ "$relay_bitrate" =~ ^[0-9]+$ ]] || fail '--bitrate must be an integer in bit/s'
command -v "$relay_ffmpeg" >/dev/null || fail 'FFmpeg is required for this diagnostic relay (Ubuntu: sudo apt install ffmpeg).'
command -v python3 >/dev/null || fail 'python3 is required for address validation'
python3 - "$relay_host" "$relay_port" "$relay_interface" "$relay_bitrate" <<'PY'
import ipaddress, sys
try:
    ipaddress.IPv4Address(sys.argv[1])
    if not 1 <= int(sys.argv[2]) <= 65535: raise ValueError('UDP port must be 1..65535')
    if sys.argv[3]: ipaddress.IPv4Address(sys.argv[3])
    if not 100000 <= int(sys.argv[4]) <= 200000000: raise ValueError('bitrate must be 100000..200000000 bit/s')
except ValueError as error:
    sys.exit(str(error))
PY
relay_destination="udp://${relay_host}:${relay_port}?pkt_size=1316&bitrate=${relay_bitrate}&burst_bits=10528&ttl=32&buffer_size=4194304"
if [[ -n "$relay_interface" ]]; then relay_destination+="&localaddr=${relay_interface}"; fi
printf 'Raw SRT -> UDP relay: %s:%s, input CBR expected=%s bit/s, interface=%s\n' \
    "$relay_host" "$relay_port" "$relay_bitrate" "${relay_interface:-auto}"
exec "$relay_ffmpeg" -hide_banner -loglevel warning -nostats \
    -f data -raw_packet_size 1316 -i "$relay_input" \
    -map 0:0 -c copy -f data -flush_packets 1 "$relay_destination"
