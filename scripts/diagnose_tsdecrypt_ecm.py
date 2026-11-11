#!/usr/bin/env python3
"""Read-only, key-free ECM/PES health diagnostic (no credentials or ECM data logged)."""
import argparse
import collections
import re
import subprocess

DIAG = re.compile(r'\bTSDECRYPT DIAG\s+stream=([^\s]+)')
FIELD = re.compile(r'\b([a-z][a-z0-9_]*)=([^\s]+)')
SID = re.compile(r'\([0-9a-fA-F]{4}@[0-9a-fA-F]+/[0-9a-fA-F]+/([0-9a-fA-F]{4})/')

def journal(unit, since):
    r = subprocess.run(['journalctl', '-u', unit, '--since', since, '--no-pager', '-o', 'cat'],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode:
        raise RuntimeError('journalctl failed for %s (%d)' % (unit, r.returncode))
    return r.stdout.splitlines()

def summarize(diag_lines, oscam_lines):
    streams = collections.defaultdict(list)
    for line in diag_lines:
        match = DIAG.search(line)
        if match:
            streams[match.group(1)].append(dict(FIELD.findall(line)))
    out = []
    for stream, rows in streams.items():
        current = rows[-1]
        previous = rows[-2] if len(rows) > 1 else None
        def delta(field):
            if previous is None or field not in previous or field not in current:
                return '?'
            try:
                d = int(current[field]) - int(previous[field])
                return str(d) if d >= 0 else 'reset'
            except ValueError:
                return '?'
        out.append((stream, current.get('cw_health', '?'),
                    delta('clear_media_pes_encrypted_pid'),
                    delta('camd_card_denied'), delta('ecm_pid_switches'),
                    delta('camd_ecm_queue_overflows')))
    counts = collections.defaultdict(collections.Counter)
    for line in oscam_lines:
        if '(ecm)' not in line:
            continue
        match = SID.search(line)
        if not match:
            continue
        if re.search(r'\btimeout\s*\(', line):
            kind = 'timeout'
        elif re.search(r'\bnot found\s*\(', line):
            kind = 'notfound'
        elif re.search(r'\bfound\s*\(', line):
            kind = 'found'
        else:
            kind = 'other'
        counts[match.group(1).upper()][kind] += 1
    return sorted(out), sorted(counts.items())

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--since', default='-2 min')
    p.add_argument('--dvb-unit', default='dvbstreamer5.service')
    p.add_argument('--oscam-unit', default='oscam-mini.service')
    args = p.parse_args()
    samples, counts = summarize(journal(args.dvb_unit, args.since),
                                journal(args.oscam_unit, args.since))
    print('TSDECRYPT: delta between last TWO reports per stream (not window totals)')
    print('%-43s %-20s %7s %7s %7s %7s' %
          ('STREAM','CW_HEALTH','PES+','DENIED+','SWITCH+','OVERFLOW+'))
    for row in samples:
        print('%-43s %-20s %7s %7s %7s %7s' % row)
    print('\nOSCAM: result events in selected window (not unique ECM success rate)')
    print('%-7s %7s %8s %9s %7s' % ('SID','FOUND','TIMEOUT','NOTFOUND','OTHER'))
    for sid, c in counts:
        print('%-7s %7d %8d %9d %7d' %
              (sid,c['found'],c['timeout'],c['notfound'],c['other']))
    if not samples:
        print('No TSDECRYPT DIAG lines: verify journal permissions, unit and interval')

if __name__ == '__main__':
    main()
