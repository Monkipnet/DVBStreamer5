#!/usr/bin/env bash
# DVBStreamer5 optional TSDecrypt A/B test runner (V10.8.456).
# Does not touch DVBStreamer5 runtime, OSCam-mini service or running tiles.
set -Eeuo pipefail
usage() {
  cat <<'HELP'
TSDecrypt isolated trial - test one MPEG-TS feed via a CAMD server.

Usage (run from the DVBStreamer5 source root):
  TSDECRYPT_CAMD_USER=user TSDECRYPT_CAMD_PASS=pass TSDECRYPT_CAMD_DES_KEY=14-byte-hex \
    bash scripts/tsdecrypt_trial.sh \
      --input 239.10.1.1:5000 --output 127.0.0.1:56000 \
      --service-id 401 --caid 098D --ecm-pid 1234 \
      --camd-server 127.0.0.1:12001 --camd-proto NEWCAMD --execute

Flags:
  --input TS_INPUT       Existing UDP multicast or file:// input.
  --output TS_OUTPUT     Separate test UDP destination or file:// output.
  --service-id SID       Service ID in decimal (not OSCam log CA identifier).
  --caid HEX             CAID hex, default 098D.
  --ecm-pid PID          Optional single ECM PID in decimal.
  --camd-server HOST:PORT  CAMD server, default 127.0.0.1:12001.
  --camd-proto PROTO     NEWCAMD or CS378X, default NEWCAMD.
  --binary FILE          tsdecrypt binary, default tsdecrypt from PATH.
  --execute              Execute: by default preflight only.
  --help                 Display this help.

Notes:
  * Experimental external test pipeline, NOT a DVBStreamer5 per-tile CA engine.
  * No dual ECM: upstream tsdecrypt selects one ECM PID.
  * A working CAMD server is still needed to obtain CW.
  * CAMD credentials become visible in tsdecrypt process arguments; use a
    dedicated low-privilege account and avoid shared hosts for secret accounts.
HELP
}
INPUT='' OUTPUT='' SERVICE='' CAID='098D' ECM_PID='' SERVER='127.0.0.1:12001' PROTO='NEWCAMD' BINARY='tsdecrypt' EXECUTE=0
while (($#)); do
  case "$1" in
    --input|--output|--service-id|--caid|--ecm-pid|--camd-server|--camd-proto|--binary)
      (($#>=2)) || { echo "Missing argument for $1" >&2; exit 2; }
      case "$1" in
        --input) INPUT="$2";; --output) OUTPUT="$2";; --service-id) SERVICE="$2";;
        --caid) CAID="$2";; --ecm-pid) ECM_PID="$2";;
        --camd-server) SERVER="$2";; --camd-proto) PROTO="$2";; --binary) BINARY="$2";;
      esac
      shift 2 ;;
    --execute) EXECUTE=1; shift;;
    --help|-h) usage; exit 0;;
    --dual-ecm) echo "Dual ECM is not implemented by upstream tsdecrypt; refusing." >&2; exit 2;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2;;
  esac
done
[[ -n "$INPUT" && -n "$OUTPUT" && -n "$SERVICE" ]] || { usage >&2; exit 2; }
[[ "$INPUT" != "$OUTPUT" ]] || { echo "Input and output must differ" >&2; exit 2; }
[[ "$SERVICE" =~ ^[0-9]+$ ]] && (( 10#$SERVICE >= 1 && 10#$SERVICE <= 65535 )) || { echo "Invalid decimal service ID" >&2; exit 2; }
[[ "$CAID" =~ ^[0-9a-fA-F]{4}$ ]] || { echo "CAID must be 4 hex digits" >&2; exit 2; }
[[ -z "$ECM_PID" || "$ECM_PID" =~ ^[0-9]+$ ]] || { echo "Invalid ECM PID" >&2; exit 2; }
if [[ -n "$ECM_PID" ]]; then
  (( 10#$ECM_PID >= 1 && 10#$ECM_PID <= 8190 )) || { echo "ECM PID out of range" >&2; exit 2; }
fi
[[ "$PROTO" == "NEWCAMD" || "$PROTO" == "CS378X" ]] || { echo "Protocol must be NEWCAMD or CS378X" >&2; exit 2; }
[[ "$SERVER" =~ ^[^[:space:]]+:[0-9]+$ ]] || { echo "Invalid CAMD host:port" >&2; exit 2; }
[[ "$INPUT" == file://* || "$INPUT" =~ ^[0-9]+[.][0-9]+[.][0-9]+[.][0-9]+:[0-9]+$ ]] || { echo "Input must be file:// or UDP IPv4:port" >&2; exit 2; }
[[ "$OUTPUT" == file://* || "$OUTPUT" =~ ^[0-9]+[.][0-9]+[.][0-9]+[.][0-9]+:[0-9]+$ ]] || { echo "Output must be file:// or UDP IPv4:port" >&2; exit 2; }
command -v "$BINARY" >/dev/null 2>&1 || { echo "tsdecrypt binary not found: $BINARY" >&2; exit 1; }
args=(-I "$INPUT" -O "$OUTPUT" -M "$SERVICE" -C "$CAID" -c VIDEOGUARD -A "$PROTO" -s "$SERVER" -K)
if [[ -n "$ECM_PID" ]]; then args+=(-X "$ECM_PID"); fi
echo "TSDecrypt trial: input=$INPUT output=$OUTPUT service=$SERVICE caid=$CAID ecm_pid=${ECM_PID:-auto} camd=$SERVER protocol=$PROTO"
if (( ! EXECUTE )); then echo "Preflight OK (not started; use --execute for isolated test)."; exit 0; fi
: "${TSDECRYPT_CAMD_USER:?TSDECRYPT_CAMD_USER is required}"
: "${TSDECRYPT_CAMD_PASS:?TSDECRYPT_CAMD_PASS is required}"
args+=(-U "$TSDECRYPT_CAMD_USER" -P "$TSDECRYPT_CAMD_PASS")
if [[ "$PROTO" == "NEWCAMD" ]]; then
  : "${TSDECRYPT_CAMD_DES_KEY:?TSDECRYPT_CAMD_DES_KEY is required for NEWCAMD}"
  [[ "$TSDECRYPT_CAMD_DES_KEY" =~ ^[0-9a-fA-F]{28}$ ]] || { echo "DES key must have 28 hex digits" >&2; exit 2; }
  args+=(-B "$TSDECRYPT_CAMD_DES_KEY")
fi
echo "WARNING: tsdecrypt exposes CAMD credentials in its process argument list. Use only a trusted test environment." >&2
exec "$BINARY" "${args[@]}"
