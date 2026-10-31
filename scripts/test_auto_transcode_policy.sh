#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(mktemp "${TMPDIR:-/tmp}/dvbstreamer5-auto-transcode.XXXXXX")"
trap 'rm -f -- "$OUT"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
  -I"$ROOT/src" \
  "$ROOT/tests/transcode/test_auto_transcode_policy.cpp" -o "$OUT"
"$OUT"
