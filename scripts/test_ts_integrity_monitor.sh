#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_BIN="$(mktemp "${TMPDIR:-/tmp}/dvbstreamer5-ts-monitor.XXXXXX")"
trap 'rm -f -- "$TEST_BIN"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
  -I"$ROOT/src/ca/backends/newcamd" \
  "$ROOT/tests/newcamd/test_ts_integrity_monitor.cpp" -o "$TEST_BIN"
"$TEST_BIN"
