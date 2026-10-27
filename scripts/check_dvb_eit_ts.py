#!/usr/bin/env python3
"""Read-only DVB EIT PID 0x12 validator for captured MPEG-TS (188-byte packets).

Example: timeout 35 curl -sS http://HOST:PORT/CHANNEL/mpegts -o /tmp/wisi.ts
         python3 scripts/check_dvb_eit_ts.py /tmp/wisi.ts
"""
import argparse
from collections import Counter
from pathlib import Path

def crc32_mpeg(data):
    crc = 0xffffffff
    for x in data:
        crc ^= x << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
    return crc

def sections_from_ts(data):
    active = {}
    expect_cc = {}
    for i in range(0, len(data) - 187, 188):
        pkt = data[i:i+188]
        if pkt[0] != 0x47:
            continue
        pid = ((pkt[1] & 0x1f) << 8) | pkt[2]
        if pid not in (0x0000, 0x0011, 0x0012):
            continue
        payload = (pkt[3] >> 4) & 3
        if not (payload & 1):
            continue
        at = 4
        if payload & 2:
            at += pkt[4] + 1
        if at >= 188:
            continue
        cc = pkt[3] & 15
        if pid in expect_cc and cc != (expect_cc[pid] + 1) % 16:
            active.pop(pid, None)
        expect_cc[pid] = cc
        if pkt[1] & 0x40:
            pointer = pkt[at]
            at += 1 + pointer
            active[pid] = bytearray()
        if at >= 188:
            continue
        if pid not in active:
            continue
        active[pid].extend(pkt[at:])
        while len(active[pid]) >= 3:
            buf = active[pid]
            if buf[0] == 0xff:
                active.pop(pid, None)
                break
            wanted = 3 + (((buf[1] & 15) << 8) | buf[2])
            if wanted < 8 or wanted > 4096:
                active.pop(pid, None)
                break
            if len(buf) < wanted:
                break
            section = bytes(buf[:wanted])
            yield pid, section
            active[pid] = bytearray(buf[wanted:])

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    args = parser.parse_args()
    path = Path(args.file)
    data = path.read_bytes()
    print("TS:", path, "bytes:", len(data))
    if len(data) < 188 or data[0] != 0x47:
        raise SystemExit("ERROR: expected raw MPEG-TS starting at sync byte 0x47")
    tables = Counter()
    crc_errors = []
    pat = set()
    sdt = set()
    eit = set()
    flags = {}
    for pid, section in sections_from_ts(data):
        if crc32_mpeg(section) != 0:
            crc_errors.append((pid, section[0]))
            continue
        table = section[0]
        tables[(pid, table)] += 1
        if pid == 0 and table == 0x00 and len(section) >= 16:
            tsid = int.from_bytes(section[3:5], "big")
            for off in range(8, len(section) - 4, 4):
                sid = int.from_bytes(section[off:off+2], "big")
                if sid:
                    pat.add((sid, tsid))
        elif pid == 0x11 and table == 0x42 and len(section) >= 20:
            tsid = int.from_bytes(section[3:5], "big")
            onid = int.from_bytes(section[8:10], "big")
            off = 11
            while off + 5 <= len(section) - 4:
                sid = int.from_bytes(section[off:off+2], "big")
                flags[sid] = section[off+2] & 3
                sdt.add((sid, tsid, onid))
                desc = ((section[off+3] & 15) << 8) | section[off+4]
                off += 5 + desc
        elif pid == 0x12 and (table == 0x4e or 0x50 <= table <= 0x5f):
            sid = int.from_bytes(section[3:5], "big")
            tsid = int.from_bytes(section[8:10], "big")
            onid = int.from_bytes(section[10:12], "big")
            eit.add((sid, tsid, onid))
    print("DVB table sections:", {f"{pid:04x}:{table:02x}": n
                                     for (pid, table), n in sorted(tables.items())})
    print("PAT (SID, TSID):", sorted(pat))
    print("SDT (SID, TSID, ONID):", sorted(sdt))
    print("EIT (SID, TSID, ONID):", sorted(eit))
    print("SDT EIT flags by SID:", flags)
    print("CRC errors:", len(crc_errors))
    if not eit:
        raise SystemExit("FAIL: No DVB EIT actual sections on PID 0x12")
    if crc_errors:
        raise SystemExit("FAIL: PSI/SI CRC errors found")
    for sid, tsid, onid in eit:
        if (sid, tsid) not in pat:
            raise SystemExit(f"FAIL: EIT SID={sid} TSID={tsid} not in PAT")
        if (sid, tsid, onid) not in sdt:
            raise SystemExit(f"FAIL: EIT SID={sid} TSID={tsid} ONID={onid} not in SDT")
        if flags.get(sid) != 3:
            raise SystemExit(f"FAIL: SDT EIT flags not advertised for SID={sid}")
    print("PASS: CRC-valid DVB EIT SID/TSID/ONID match output PAT and SDT")

if __name__ == "__main__":
    main()
