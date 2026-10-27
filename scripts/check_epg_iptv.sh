#!/usr/bin/env bash
# Non-destructive XMLTV/IPTV smoke test: never retunes or restarts streams.
set -Eeuo pipefail
BASE="http://127.0.0.1:9000"
if [[ $# -ge 1 ]]; then BASE="$1"; fi
BASE="$(printf '%s' "$BASE" | sed 's@/$@@')"
TEMP="$(mktemp -d)"
trap 'rm -rf -- "$TEMP"' EXIT

echo "== XMLTV =="
curl --fail --silent --show-error --max-time 30 \
    "$BASE/epg.xml" -o "$TEMP/epg.xml"

echo "== Multi-output IPTV M3U =="
curl --fail --silent --show-error --max-time 30 \
    "$BASE/playlist.m3u" -o "$TEMP/playlist.m3u"

echo "== Optional XMLTV gzip =="
STATUS="$(curl --silent --show-error --max-time 30 \
    -o "$TEMP/epg.xml.gz" -w '%{http_code}' "$BASE/epg.xml.gz")"
if [[ "$STATUS" == 200 ]]; then
    python3 - "$TEMP/epg.xml" "$TEMP/epg.xml.gz" <<'PY'
import gzip, pathlib, sys
plain = pathlib.Path(sys.argv[1]).read_bytes()
compressed = gzip.decompress(pathlib.Path(sys.argv[2]).read_bytes())
if plain != compressed:
    raise SystemExit("ERROR: gzip payload differs from the uncompressed XMLTV")
print("gzip matches XMLTV; compressed bytes:", len(pathlib.Path(sys.argv[2]).read_bytes()))
PY
else
    echo "gzip HTTP $STATUS (optional; install zlib1g-dev and rebuild for /epg.xml.gz)"
fi

python3 - "$TEMP/epg.xml" "$TEMP/playlist.m3u" <<'PY'
import pathlib, re, sys, xml.etree.ElementTree as ET

xml_path, playlist_path = map(pathlib.Path, sys.argv[1:])
root = ET.parse(xml_path).getroot()
assert root.tag == "tv", "Invalid XMLTV root"
channels = {item.attrib["id"] for item in root.findall("channel")}
programmes = root.findall("programme")
for programme in programmes:
    assert programme.attrib.get("channel") in channels, (
        "XMLTV programme refers to an unknown channel")
    assert programme.attrib.get("start") and programme.attrib.get("stop")
playlist = playlist_path.read_text(encoding="utf-8-sig")
lines = playlist.splitlines()
assert lines and lines[0].startswith("#EXTM3U"), "Missing M3U header"
assert 'url-tvg="' in lines[0], "M3U missing url-tvg"
ids = re.findall(r'#EXTINF:[^\r\n]*\btvg-id="([^"]*)"', playlist)
unmatched = set(ids) - channels
print(f"XMLTV: {len(channels)} channels, {len(programmes)} programmes")
print(f"IPTV: {len(ids)} output links, {len(set(ids))} unique channels")
print(f"IPTV links without EPG (may have EPG disabled): {len(unmatched)}")
if programmes:
    first = programmes[0]
    print("First event:", first.attrib.get("start"), first.findtext("title"))
else:
    print("No programmes yet: tune an EPG-enabled DVB channel and retry.")
print("PASS: XMLTV and IPTV playlist structure")
PY
