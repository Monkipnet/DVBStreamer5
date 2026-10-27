#!/usr/bin/env python3
"""Read-only external XMLTV diagnostics; does not restart or retune streams."""
import argparse
import base64
import getpass
import json
import sys
import urllib.error
import urllib.request
import urllib.parse
from datetime import datetime, timezone

parser = argparse.ArgumentParser(description="Check DVBStreamer5 external XMLTV status and mappings")
parser.add_argument("--base", default="http://127.0.0.1:9000")
parser.add_argument("--user", default="admin", help="Control panel login (password prompted securely)")
parser.add_argument("--stream-id", help="Check one configured channel")
args = parser.parse_args()
base = args.base.rstrip("/")
password = getpass.getpass(f"DVBStreamer5 password for {args.user}: ")
auth = "Basic " + base64.b64encode(f"{args.user}:{password}".encode()).decode()

def request_json(path):
    req = urllib.request.Request(
        base + path, headers={"Authorization": auth, "Cache-Control": "no-cache"}
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as response:
            return json.load(response)
    except (urllib.error.HTTPError, urllib.error.URLError, ValueError) as exc:
        raise RuntimeError(f"{path}: {exc}") from exc

def local_time(timestamp):
    if not timestamp:
        return "—"
    return datetime.fromtimestamp(int(timestamp), tz=timezone.utc).astimezone().isoformat(timespec="seconds")

try:
    info = request_json("/api/xmltv/status")
    sources = info.get("sources", [])
    print(f"External XMLTV sources: {len(sources)}")
    for source in sources:
        print(f"  {source.get('id', '?')}: {source.get('name') or 'unnamed'}")
        print(f"    enabled={source.get('enabled')} "
              f"schedule={source.get('refresh_period', 'daily')} "
              f"weekday={source.get('refresh_weekday', 1)} "
              f"time={source.get('refresh_time', '03:00')} (server-local)")
        print(f"    channels={source.get('channel_count', 0)} "
              f"events={source.get('event_count', 0)} "
              f"download_in_progress={source.get('downloading', False)} "
              f"queued={source.get('queued', False)}")
        print(f"    last_success={local_time(source.get('last_success'))}")
        if source.get("last_error"):
            print(f"    ERROR: {source['last_error']}")
    catalog = request_json("/api/xmltv/channels")
    print(f"XMLTV catalog entries: {len(catalog)}")
    if args.stream_id:
        target_ids = [args.stream_id]
    else:
        state = request_json("/api/state")
        target_ids = [s.get("id", "") for s in state.get("streams", [])
                      if s.get("epg_enabled") and s.get("xmltv_channel_id")]
    print(f"Checked streams: {len(target_ids)}")
    for stream_id in target_ids:
        url = "/api/epg?stream_id=" + urllib.parse.quote(stream_id, safe="")
        epg = request_json(url)
        events = epg.get("events", [])
        print(f"  {stream_id}: mode={epg.get('mode')} "
              f"source={epg.get('source')} events={len(events)} "
              f"available={epg.get('available')}")
        if epg.get("now"):
            print(f"    NOW: {epg['now'].get('title', '')}")
        elif epg.get("next"):
            print(f"    NEXT: {epg['next'].get('title', '')}")
    print("PASS: XMLTV API responses read successfully")
except RuntimeError as exc:
    print(f"ERROR: {exc}", file=sys.stderr)
    sys.exit(1)
