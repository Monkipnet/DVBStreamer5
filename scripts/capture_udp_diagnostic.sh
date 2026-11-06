#!/usr/bin/env bash
# Passive capture + offline TS analysis; leaves application/network settings alone.
set -euo pipefail
umask 077

capture_destination=''
capture_port=''
capture_interface=any
capture_bitrate=4000000
capture_seconds=100
capture_existing=''
capture_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
usage() {
    cat <<'USAGE'
Usage: bash capture_udp_diagnostic.sh --destination IP --port PORT [options]
  --interface NAME   Linux interface name, or any (default any)
  --bitrate BPS      Full output CBR for PCR/TS-position checks (default 4000000)
  --seconds N        Capture duration, 5..900 seconds (default 100)
  --pcap PATH        Analyse an existing PCAP/PCAP.GZ instead of capturing

Requires tcpdump, Python 3, iproute2 and coreutils. FFmpeg/ffprobe are optional
for offline program/decoder reports. Only one destination is captured; all its
UDP ports/fragments are retained in PCAP, then the requested port is analysed.
Writes a new /tmp/dvb-udp-diag.* folder. Upload reports.tar.gz first; original
PCAP and extracted TS stay outside that small archive for later inspection.
USAGE
}
fail() { printf '%s\n' "$*" >&2; exit 1; }
while (($#)); do
    case "$1" in
        --destination|--port|--interface|--bitrate|--seconds|--pcap)
            (($# >= 2)) || fail "$1 needs a value"
            case "$1" in
                --destination) capture_destination="$2" ;;
                --port) capture_port="$2" ;;
                --interface) capture_interface="$2" ;;
                --bitrate) capture_bitrate="$2" ;;
                --seconds) capture_seconds="$2" ;;
                --pcap) capture_existing="$2" ;;
            esac
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "Unknown option: $1" ;;
    esac
done
capture_tools=(python3 ip timeout tar)
if [[ -z "$capture_existing" ]]; then capture_tools+=(tcpdump); fi
for capture_tool in "${capture_tools[@]}"; do
    command -v "$capture_tool" >/dev/null || fail "Required tool missing: $capture_tool"
done
python3 - "$capture_destination" "$capture_port" "$capture_bitrate" "$capture_seconds" <<'PY'
import ipaddress, sys
try:
    ipaddress.IPv4Address(sys.argv[1])
    if not 1 <= int(sys.argv[2]) <= 65535: raise ValueError('port must be 1..65535')
    if not 100000 <= int(sys.argv[3]) <= 200000000: raise ValueError('bitrate must be 100000..200000000 bit/s')
    if not 5 <= int(sys.argv[4]) <= 900: raise ValueError('seconds must be 5..900')
except ValueError as error:
    sys.exit(str(error))
PY
[[ -f "$capture_script_dir/analyze_udp_pcap.py" ]] || fail 'Run this helper from the repository scripts directory with analyze_udp_pcap.py alongside it.'
capture_root=()
if [[ -z "$capture_existing" ]] && ((EUID != 0)); then capture_root=(sudo); fi
if [[ -n "$capture_existing" ]]; then
    [[ -r "$capture_existing" ]] || fail "Cannot read capture: $capture_existing"
    capture_existing="$(realpath -- "$capture_existing")"
fi
capture_dir="$(mktemp -d /tmp/dvb-udp-diag.XXXXXXXX)"
capture_reports="$capture_dir/reports"
mkdir "$capture_reports"
capture_user="$(id -un)"
capture_stats() {
    local capture_stage="$1"
    date --iso-8601=seconds > "$capture_reports/time-${capture_stage}.txt"
    ip -j -4 address show > "$capture_reports/interfaces-${capture_stage}.json"
    ip -s link > "$capture_reports/links-${capture_stage}.txt"
    if command -v tc >/dev/null; then tc -s qdisc show > "$capture_reports/qdisc-${capture_stage}.txt" 2>&1 || true; fi
    if command -v nstat >/dev/null; then nstat -asz > "$capture_reports/nstat-${capture_stage}.txt" 2>&1 || true; fi
}
capture_stats before
if [[ -n "$capture_existing" ]]; then
    capture_input="$capture_existing"
    printf 'Analysing existing capture: %s\n' "$capture_input"
    printf 'Offline analysis; no live capture or capture-drop statistics collected.\n' > "$capture_reports/tcpdump.log"
else
capture_input="$capture_dir/udp.pcap"
printf 'Capturing %s:%s on %s for %s seconds. Directory: %s\n' \
    "$capture_destination" "$capture_port" "$capture_interface" "$capture_seconds" "$capture_dir"
capture_status=0
"${capture_root[@]}" timeout --signal=INT --kill-after=5s "$capture_seconds" \
    tcpdump -p -i "$capture_interface" -Q out -nn -s 0 -B 8192 -Z "$capture_user" \
    -w "$capture_input" "ip proto 17 and dst host $capture_destination" \
    > "$capture_reports/tcpdump.log" 2>&1 || capture_status=$?
cat "$capture_reports/tcpdump.log"
if [[ "$capture_status" != 0 && "$capture_status" != 124 && "$capture_status" != 130 ]]; then
    fail "Capture failed (status $capture_status). Reports retained: $capture_dir"
fi
fi
capture_stats after
if command -v journalctl >/dev/null; then
    "${capture_root[@]}" journalctl -b -u dvbstreamer5.service --no-pager -o cat 2>/dev/null \
        | grep -E 'NATIVE UDP OUTPUT open|UDP TS OUTPUT engine=native' \
        | tail -n 80 > "$capture_reports/udp-startup.log" || true
fi
if ! python3 "$capture_script_dir/analyze_udp_pcap.py" "$capture_input" \
    --destination "$capture_destination" --port "$capture_port" --cbr-bitrate "$capture_bitrate" \
    --extract-dir "$capture_dir/ts" --json "$capture_reports/analysis.json" \
    > "$capture_reports/analysis.txt" 2>&1; then
    printf 'Packet analysis reported an error/no matching flow; see analysis.txt.\n'
fi
cat "$capture_reports/analysis.txt"
if [[ -d "$capture_dir/ts" ]]; then
    while IFS= read -r -d '' capture_ts; do
        capture_name="$(basename -- "$capture_ts" .ts)"
        if command -v ffprobe >/dev/null; then
            capture_probe_status=0
            timeout --kill-after=5s 120 ffprobe -hide_banner -v warning -show_programs -show_streams -show_format \
                -of json "$capture_ts" > "$capture_reports/${capture_name}-probe.json" \
                2> "$capture_reports/${capture_name}-probe.log" || capture_probe_status=$?
            printf '%s\n' "$capture_probe_status" > "$capture_reports/${capture_name}-probe-exit.txt"
        fi
        if command -v ffmpeg >/dev/null; then
            capture_decode_status=0
            timeout --kill-after=5s 180 nice -n 10 ffmpeg -hide_banner -nostdin -v warning -nostats \
                -threads 1 -i "$capture_ts" -map '0:v:0?' -map '0:a:0?' \
                -progress "$capture_reports/${capture_name}-decode-progress.txt" -f null - \
                > /dev/null 2> "$capture_reports/${capture_name}-decode.log" || capture_decode_status=$?
            printf '%s\n' "$capture_decode_status" > "$capture_reports/${capture_name}-decode-exit.txt"
        fi
    done < <(find "$capture_dir/ts" -maxdepth 1 -type f -name '*.ts' -print0)
fi
tar -czf "$capture_dir/reports.tar.gz" -C "$capture_reports" .
printf '\nUpload: %s/reports.tar.gz\nExtracted TS retained: %s/ts\nOriginal PCAP: %s\n' "$capture_dir" "$capture_dir" "$capture_input"
