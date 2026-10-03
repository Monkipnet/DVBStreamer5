#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/third_party"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fetch_commit() {
    local repo="$1" commit="$2" name="$3"
    local url="https://github.com/${repo}/archive/${commit}.tar.gz"
    local archive="$TMP/${name}.tar.gz"
    local out="$DEST/${name}"
    echo "Downloading ${repo}@${commit}"
    curl -fL --retry 3 --connect-timeout 15 -o "$archive" "$url"
    rm -rf "$out"
    mkdir -p "$out"
    tar -xzf "$archive" --strip-components=1 -C "$out"
    printf 'Upstream: %s\nCommit: %s\n' "$repo" "$commit" > "$out/DVBSTREAMER5_VENDOR_INFO.txt"
}

fetch_commit cisco/openh264 652bdb7719f30b52b08e506645a7322ff1b2cc6f openh264
fetch_commit ittiam-systems/libavc 6d5853425d1697d6241a3e60ca6fd6c6c064cde6 ittiam-libavc
fetch_commit strukturag/libde265 ba62bf4cfb3242f3bf0a45617ff09e35236e4d82 libde265
fetch_commit ultravideo/kvazaar 6040962bed5cc68c5ad01234c38c08b8b2822068 kvazaar
fetch_commit mstorsjo/fdk-aac 716f4394641d53f0d79c9ddac3fa93b03a49f278 fdk-aac
fetch_commit ittiam-systems/libmpeg2 e2dbb98d7819a8225687d3a0b7f3d818784e451c ittiam-libmpeg2
fetch_commit phoboslab/pl_mpeg c871f2be022ece7ef4f64230b4fb8e1fb9eb6023 pl_mpeg

echo "Vendored native codec sources into $DEST"
echo "No FFmpeg, libav*, or GStreamer dependency is used."
