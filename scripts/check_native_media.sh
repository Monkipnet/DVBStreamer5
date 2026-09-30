#!/usr/bin/env bash
set -euo pipefail
binary="${1:-build/DVBStreamer5}"
[[ -x "$binary" ]] || { echo "Binary not found/executable: $binary" >&2; exit 2; }
"$(dirname "$0")/audit_runtime_deps.sh" "$binary"
echo "PASS: native media engine binary is self-contained from obsolete media-framework runtime libraries"
