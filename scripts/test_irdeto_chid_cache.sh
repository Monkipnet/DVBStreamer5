#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_BIN="$(mktemp "${TMPDIR:-/tmp}/dvbstreamer5-chid-preference.XXXXXX")"
trap 'rm -f -- "$TEST_BIN"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
  -pthread -I"$ROOT/src/ca/backends/newcamd" \
  "$ROOT/tests/newcamd/test_irdeto_chid_cache.cpp" -o "$TEST_BIN"
"$TEST_BIN"
