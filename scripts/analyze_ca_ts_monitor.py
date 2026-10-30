#!/usr/bin/env python3
"""Analyze V238 read-only CA TS monitor logs from every active DVB service.

Read journalctl -o short-iso from stdin or a saved text file. No TS, ECM, CW
or decrypted video bytes are read or stored. Display anomalies per SID and
coincidences across channels within a two-second window.
"""
import argparse
import collections
import datetime as dt
import re
import sys

RECORD = re.compile(r"^(\S+).*?CA V238 MONITOR:\s+(.*)$")
PAIR = re.compile(r"([a-z][a-z0-9_]*)=([^\s]+)")
ERROR_KEYS = (
    "in_tei", "in_bad_afc", "in_cc_gaps", "no_key_packets",
    "post_still_scrambled", "post_pes_bad", "nal_bad",
)
WARN_KEYS = ("in_cc_repeats", "ecm_not_found", "ecm_over_500ms")

def records(lines):
    for line in lines:
        match = RECORD.search(line)
        if not match:
            continue
        try:
            at = dt.datetime.fromisoformat(match.group(1))
        except ValueError:
            continue
        fields = dict(PAIR.findall(match.group(2)))
        if "stream" not in fields or "sid" not in fields:
            continue
        values = {}
        for key, value in fields.items():
            if key in ("stream", "sid"):
                continue
            try:
                values[key] = int(value)
            except ValueError:
                pass
        yield at, fields["stream"], fields["sid"], values

def report(items, max_windows=25):
    counts = collections.defaultdict(collections.Counter)
    by_bucket = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    n = 0
    for at, stream, sid, values in items:
        n += 1
        ident = (sid, stream)
        for key, val in values.items():
            # A peak is cumulative, and is displayed as its maximum.
            if key == "ecm_peak_ms_total":
                counts[ident][key] = max(counts[ident][key], val)
            else:
                counts[ident][key] += val
        anomaly = {k: values.get(k, 0) for k in ERROR_KEYS + WARN_KEYS
                   if values.get(k, 0) > 0}
        if anomaly:
            # Two-second windows tolerate per-service 1s log skew.
            bucket = int(at.timestamp()) // 2
            for key, value in anomaly.items():
                by_bucket[bucket][ident][key] += value

    if not n:
        print("No V238 monitor records found. Check DVBSTREAMER5_CA_TS_MONITOR=1.")
        return 1
    print("V238 CA MONITOR — records:", n, "channels:", len(counts))
    print("ERRORS: CC gaps/TEI/AFC, missing CW, still-scrambled post-CSA, PES/NAL syntax")
    print("WARNINGS: repeated CC, ECM failures or >500ms responses; not proof of video breakup")
    print()
    print("SID   INPUT      CC_GAP TEI KEY_MISS SCRAM_LEFT PES_BAD NAL_BAD ECM_>500 PEAK_MS")
    for (sid, stream), c in sorted(counts.items(),
                                   key=lambda kv: (int(kv[0][0]), kv[0][1])):
        print(f"{sid:5} {c['in_packets']:10} {c['in_cc_gaps']:6}"
              f" {c['in_tei']:3} {c['no_key_packets']:8}"
              f" {c['post_still_scrambled']:10} {c['post_pes_bad']:7}"
              f" {c['nal_bad']:7} {c['ecm_over_500ms']:8}"
              f" {c['ecm_peak_ms_total']:7}  {stream}")
    overlapping = []
    for bucket, services in sorted(by_bucket.items()):
        if len(services) > 1:
            overlapping.append((bucket, services))
    print()
    print("COINCIDENT ANOMALY/WARNING WINDOWS (2s):", len(overlapping))
    for bucket, services in overlapping[:max_windows]:
        timestamp = dt.datetime.fromtimestamp(bucket * 2, tz=dt.timezone.utc)
        details = []
        for (sid, _), values in sorted(services.items()):
            details.append(sid + ":" + ",".join(f"{k}={v}" for k,v in values.items()))
        print(timestamp.strftime("%Y-%m-%dT%H:%M:%SZ"), " | ".join(details))
    if len(overlapping) > max_windows:
        print("... plus", len(overlapping)-max_windows, "additional windows")
    print()
    print("IMPORTANT: errors are observed inside the Newcamd CA plugin before")
    print("downstream remuxing / UDP / SRT delivery. AVC/HEVC NAL checks are")
    print("syntactic indicators and cannot prove complete frame correctness.")
    return 0

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logfile", nargs="?", default="-",
                        help="journalctl -o short-iso output; '-' for stdin")
    parser.add_argument("--max-windows", type=int, default=25)
    args = parser.parse_args()
    if args.logfile == "-":
        return report(records(sys.stdin), args.max_windows)
    with open(args.logfile, "r", encoding="utf-8", errors="replace") as source:
        return report(records(source), args.max_windows)

if __name__ == "__main__":
    sys.exit(main())
