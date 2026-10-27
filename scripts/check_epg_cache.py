#!/usr/bin/env python3
"""Read-only DVBStreamer5 10.8.204 EPG persistence diagnostics.

Shows the compressed cache files and validates the header and records.
Does not start a tuner, trigger network downloads, change configs or
require the web panel to be online.
"""
import argparse
import gzip
import json
import os
from pathlib import Path
from datetime import datetime, timezone

def diagnostic(path):
    try:
        with gzip.open(path, "rt", encoding="utf-8") as source:
            header = json.loads(next(source))
            kind = header.get("kind")
            if header.get("schema") != 1 or kind not in ("dvb", "xmltv"):
                return False, "invalid cache schema", {}
            total, future, channels = 0, 0, 0
            now = datetime.now(timezone.utc).timestamp()
            for line in source:
                data = json.loads(line)
                if data.get("kind") == "channel":
                    channels += 1
                elif data.get("kind") == "event":
                    total += 1
                    end = int(data.get("stop") or
                              (int(data.get("start") or 0) +
                               int(data.get("duration") or 0)))
                    if end > now:
                        future += 1
            return True, f"{kind}: {total} events ({future} current/future), {channels} channels", header
    except (OSError, EOFError, ValueError, StopIteration, UnicodeError) as exc:
        return False, str(exc), {}

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--directory", default=os.environ.get(
    "DVBSTREAMER5_EPG_CACHE_DIR", "/var/lib/dvbstreamer5/epg"))
args = parser.parse_args()

root = Path(args.directory)
print("EPG cache:", root)
if not root.is_dir():
    raise SystemExit("FAIL: EPG cache directory absent (check build, permissions or configured path)")
files = sorted(root.glob("*.jsonl.gz"))
if not files:
    raise SystemExit("FAIL: no EPG cache files yet (collect satellite EIT or download XMLTV)")
healthy = 0
for path in files:
    ok, message, header = diagnostic(path)
    healthy += int(ok)
    print(("OK  " if ok else "BAD "), path.name, f"{path.stat().st_size} B", message)
    if ok:
        if header.get("kind") == "xmltv":
            print("    source:", header.get("source_id"),
                  "last_success:", header.get("last_success"))
        else:
            print("    DVB key:", header.get("key", "")[:128])
print(f"Checked {len(files)} cache files; valid={healthy}")
if healthy != len(files):
    raise SystemExit(1)
