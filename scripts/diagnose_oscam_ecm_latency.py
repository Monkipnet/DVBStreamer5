#!/usr/bin/env python3
"""Summarize opt-in DVB5 ECM LATENCY journal lines. Python 3.6+.

Usage:
  journalctl -u oscam-mini.service --since '-5 min' -o cat --no-pager |
      python3 scripts/diagnose_oscam_ecm_latency.py

Only aggregate numeric timings and SID; never show hashes or control words.
"""
import re
import sys
from collections import Counter, defaultdict

FIELDS = re.compile(r"([a-z_]+)=([^\s]+)")
PER_SID = defaultdict(lambda: {"stages": Counter(), "prequeue": [],
                               "queue": [], "upstream": [], "late": 0,
                               "failed": 0})


def percentile(samples, percentage):
    if not samples:
        return "-"
    samples = sorted(samples)
    index = (len(samples) * percentage + 99) // 100 - 1
    return str(samples[max(0, min(len(samples) - 1, index))])


for line in sys.stdin:
    if "DVB5 ECM LATENCY " not in line:
        continue
    fields = dict(FIELDS.findall(line.split("DVB5 ECM LATENCY ", 1)[1]))
    sid, stage = fields.get("sid", ""), fields.get("stage", "")
    if not re.match(r"^[0-9A-Fa-f]{4}$", sid):
        continue
    data = PER_SID[sid.upper()]
    data["stages"][stage] += 1
    if stage == "upstream_sent":
        if fields.get("send") != "ok":
            data["failed"] += 1
            continue
        for key, field in (("prequeue", "prequeue_ms"), ("queue", "queue_ms")):
            if fields.get(field, "").isdigit():
                data[key].append(int(fields[field]))
    elif stage == "upstream_reply":
        if fields.get("upstream_ms", "").isdigit():
            data["upstream"].append(int(fields["upstream_ms"]))
        data["late"] += fields.get("late") == "1"

print("SID   IN  SENT REPLY TIMEOUT LATE FAIL PREQ_P95 Q_P95 UP_P50 UP_P95")
for sid in sorted(PER_SID):
    d = PER_SID[sid]
    c = d["stages"]
    print("{:<5} {:>3} {:>5} {:>5} {:>7} {:>4} {:>4} {:>8} {:>5} {:>6} {:>6}".format(
        sid, c["ingress"], c["upstream_sent"], c["upstream_reply"],
        c["client_timeout"], d["late"], d["failed"],
        percentile(d["prequeue"], 95), percentile(d["queue"], 95),
        percentile(d["upstream"], 50), percentile(d["upstream"], 95)))
