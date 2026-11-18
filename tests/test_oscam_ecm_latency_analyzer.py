#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path

script = Path(__file__).resolve().parent.parent / "scripts" / "diagnose_oscam_ecm_latency.py"
input_data = """unrelated (ecm) hash=AAAAAAAAAAAAAAAAAAAA
DVB5 ECM LATENCY stage=ingress id=1 caid=098D sid=0B54
DVB5 ECM LATENCY stage=upstream_sent id=1 reader=icam caid=098D sid=0B54 prequeue_ms=2 queue_ms=5 send=ok
DVB5 ECM LATENCY stage=client_timeout id=1 caid=098D sid=0B54 elapsed_ms=5001
DVB5 ECM LATENCY stage=upstream_reply id=1 reader=icam caid=098D sid=0B54 upstream_ms=5501 total_ms=5508 result=found late=1
DVB5 ECM LATENCY stage=ingress id=2 caid=098D sid=0B54
DVB5 ECM LATENCY stage=upstream_sent id=2 reader=icam caid=098D sid=0B54 prequeue_ms=3 queue_ms=7 send=failed
"""
p = subprocess.Popen(
    [sys.executable, str(script)],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    universal_newlines=True)
out, err = p.communicate(input_data)
assert p.returncode == 0, err
assert "0B54" in out
line = [l for l in out.splitlines() if l.startswith("0B54")][0]
fields = line.split()
assert fields[:7] == ["0B54", "2", "2", "1", "1", "1", "1"], fields
assert fields[7:] == ["2", "5", "5501", "5501"], fields
assert "AAAA" not in out
print("PASS OSCam ECM journal analyzer")

# Guard the opt-in receive-side diagnostic hooks against accidental removal.
source_root = Path(__file__).resolve().parent.parent / "third_party" / "oscam-mini"
work_source = (source_root / "oscam-work.c").read_text()
reader_source = (source_root / "oscam-reader.c").read_text()
assert "stage=upstream_rx_matched" in work_source
assert "stage=upstream_rx_unmatched" in work_source
assert "reader->typ == R_NEWCAMD && dvb5_ecm_latency_enabled()" in work_source
assert "stage=ecm_pending_duplicate" in reader_source
assert "mode=cached" in reader_source and "mode=pending" in reader_source
assert "dvb5_diag_id" in work_source and "dvb5_diag_id" in reader_source
print("PASS OSCam Newcamd receive/duplicate diagnostic hooks")
