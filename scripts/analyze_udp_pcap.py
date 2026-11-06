#!/usr/bin/env python3
"""Inspect raw UDP/TS at a pcap capture point, without third-party packages.

Accepts classic pcap, optionally gzip-compressed. Linux `-i any` captures keep
each interface and source socket separate; do not merge their CC sequences.
Software capture timing is not a wire-level PCR accuracy measurement.
"""
import argparse
from collections import Counter
from contextlib import ExitStack
from datetime import datetime, timezone
import gzip
import ipaddress
import json
from pathlib import Path
import struct
import sys

PCR_WRAP = (1 << 33) * 300


class PidStats:
    def __init__(self, pid, cbr_bitrate=None):
        self.pid = pid
        self.packets = self.tei = self.scrambled = self.header_errors = 0
        self.cc_errors = self.duplicates = self.discontinuities = 0
        self.previous_cc = None
        self.previous_payload = None
        self.pcr_count = self.pcr_backwards = self.pcr_elapsed = self.pcr_gap = 0
        self.previous_pcr = None
        self.cbr_bitrate = cbr_bitrate
        self.phase_anchor = None
        self.phase_ticks = self.phase_samples = 0
        self.phase_min = self.phase_max = 0.0

    def push(self, packet, ts_position=None):
        self.packets += 1
        self.tei += bool(packet[1] & 0x80)
        self.scrambled += bool(packet[3] & 0xc0)
        afc = (packet[3] >> 4) & 3
        if not afc:
            self.header_errors += 1
            return
        if afc & 2:
            length = packet[4]
            if length > 183 or (afc == 3 and length > 182):
                self.header_errors += 1
                return
            if length and packet[5] & 0x80:
                self.discontinuities += 1
                self.previous_cc = self.previous_pcr = None
            if length >= 7 and packet[5] & 0x10 and not packet[1] & 0x80:
                extension = ((packet[10] & 1) << 8) | packet[11]
                if extension >= 300:
                    self.header_errors += 1
                else:
                    ticks = (((packet[6] << 25) | (packet[7] << 17) |
                              (packet[8] << 9) | (packet[9] << 1) |
                              (packet[10] >> 7)) * 300 + extension)
                    self.pcr_count += 1
                    if self.previous_pcr is not None:
                        delta = (ticks - self.previous_pcr) % PCR_WRAP
                        if delta > PCR_WRAP // 2:
                            self.pcr_backwards += 1
                            self.phase_anchor = None
                            self.phase_ticks = 0
                        else:
                            self.pcr_elapsed += delta
                            self.pcr_gap = max(self.pcr_gap, delta)
                            self.phase_ticks += delta
                    else:
                        self.phase_anchor = None
                        self.phase_ticks = 0
                    if self.cbr_bitrate and ts_position is not None:
                        if self.phase_anchor is None:
                            self.phase_anchor = ts_position
                        numerator = ((ts_position - self.phase_anchor) * 188 * 8 * 27000000
                                     - self.phase_ticks * self.cbr_bitrate)
                        phase_us = numerator * 1000000 / (self.cbr_bitrate * 27000000)
                        self.phase_samples += 1
                        self.phase_min = min(self.phase_min, phase_us)
                        self.phase_max = max(self.phase_max, phase_us)
                    self.previous_pcr = ticks
        if not afc & 1 or self.pid == 0x1fff:
            return
        cc = packet[3] & 15
        if self.previous_cc is not None and cc != (self.previous_cc + 1) % 16:
            if cc == self.previous_cc and packet == self.previous_payload:
                self.duplicates += 1
            else:
                self.cc_errors += 1
        self.previous_cc = cc
        self.previous_payload = packet

    def report(self):
        return dict(pid=self.pid, pid_hex=f'0x{self.pid:04X}', packets=self.packets,
                    cc_errors=self.cc_errors, exact_duplicates=self.duplicates,
                    tei=self.tei, scrambled=self.scrambled,
                    header_errors=self.header_errors, discontinuities=self.discontinuities,
                    pcr_count=self.pcr_count, pcr_backwards=self.pcr_backwards,
                    pcr_span_seconds=round(self.pcr_elapsed / 27000000, 6),
                    pcr_gap_max_ms=round(self.pcr_gap / 27000, 6),
                    pcr_cbr_phase_samples=self.phase_samples,
                    pcr_cbr_phase_min_us=round(self.phase_min, 6) if self.phase_samples > 1 else None,
                    pcr_cbr_phase_max_us=round(self.phase_max, 6) if self.phase_samples > 1 else None,
                    pcr_cbr_phase_span_us=round(self.phase_max - self.phase_min, 6) if self.phase_samples > 1 else None)


def percentile(sorted_values, fraction):
    if not sorted_values:
        return None
    position = (len(sorted_values) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(sorted_values) - 1)
    return sorted_values[low] + (sorted_values[high] - sorted_values[low]) * (position - low)


class Flow:
    def __init__(self, key, cbr_bitrate=None, extracted_ts=None, writer=None):
        self.key = key
        self.first = self.last = None
        self.datagrams = self.payload_bytes = self.first_payload_size = 0
        self.lengths = Counter()
        self.ttl_counts = Counter()
        self.dscp_counts = Counter()
        self.ecn_counts = Counter()
        self.dont_fragment = 0
        self.gaps = []
        self.backwards_timestamps = self.invalid_ts = 0
        self.pids = {}
        self.cbr_bitrate = cbr_bitrate
        self.ts_position = 0
        self.extracted_ts = extracted_ts
        self.writer = writer

    def push(self, timestamp, payload, ttl, tos, dont_fragment):
        if self.writer is not None:
            self.writer.write(payload)  # retain the exact received bytes, even if malformed
        if self.first is None:
            self.first = timestamp
            self.first_payload_size = len(payload)
        if self.last is not None:
            gap = timestamp - self.last
            if gap < 0:
                self.backwards_timestamps += 1
            else:
                self.gaps.append(gap)
        self.last = timestamp
        self.datagrams += 1
        self.payload_bytes += len(payload)
        self.lengths[len(payload)] += 1
        self.ttl_counts[ttl] += 1
        self.dscp_counts[tos >> 2] += 1
        self.ecn_counts[tos & 3] += 1
        self.dont_fragment += bool(dont_fragment)
        if not payload or len(payload) % 188 or any(payload[p] != 0x47 for p in range(0, len(payload), 188)):
            self.invalid_ts += 1
            return
        for offset in range(0, len(payload), 188):
            packet = payload[offset:offset + 188]
            pid = ((packet[1] & 31) << 8) | packet[2]
            if pid not in self.pids:
                self.pids[pid] = PidStats(pid, self.cbr_bitrate)
            self.pids[pid].push(packet, self.ts_position)
            self.ts_position += 1

    def report(self):
        interface, direction, source, sport, dest, dport = self.key
        duration = (self.last - self.first) / 1e9
        gaps = sorted(self.gaps)
        ts_packets = sum(p.packets for p in self.pids.values())
        null_packets = self.pids[0x1fff].packets if 0x1fff in self.pids else 0
        return dict(interface_index=interface, packet_type=direction,
                    source_ip=source, source_port=sport, destination_ip=dest, destination_port=dport,
                    datagrams=self.datagrams, payload_bytes=self.payload_bytes,
                    payload_lengths=dict(sorted(self.lengths.items())),
                    ip_ttl_counts=dict(sorted(self.ttl_counts.items())),
                    ip_dscp_counts=dict(sorted(self.dscp_counts.items())),
                    ip_ecn_counts=dict(sorted(self.ecn_counts.items())),
                    ip_dont_fragment_datagrams=self.dont_fragment,
                    ts_packets=ts_packets, null_packets=null_packets,
                    null_percent=round(100 * null_packets / ts_packets, 6) if ts_packets else None,
                    first_utc=utc(self.first), last_utc=utc(self.last),
                    span_seconds=round(duration, 9),
                    # Exclude the first datagram, which precedes this measured interval.
                    payload_mbps=round((self.payload_bytes - self.first_payload_size) * 8 / duration / 1e6, 6) if duration > 0 else None,
                    gap_us=dict(min=round(gaps[0] / 1000, 3),
                                p50=round(percentile(gaps, .5) / 1000, 3),
                                p95=round(percentile(gaps, .95) / 1000, 3),
                                p99=round(percentile(gaps, .99) / 1000, 3),
                                max=round(gaps[-1] / 1000, 3)) if gaps else {},
                    gaps_over_ms={str(ms): sum(g > ms * 1000000 for g in gaps) for ms in (5, 10, 20, 100)},
                    gaps_under_200us=sum(g < 200000 for g in gaps),
                    backwards_capture_timestamps=self.backwards_timestamps,
                    invalid_ts_datagrams=self.invalid_ts,
                    declared_cbr_bitrate=self.cbr_bitrate,
                    extracted_ts=str(self.extracted_ts) if self.extracted_ts else None,
                    cc_errors=sum(p.cc_errors for p in self.pids.values()),
                    pids=[p.report() for _, p in sorted(self.pids.items())])


def utc(ns):
    return datetime.fromtimestamp(ns / 1e9, timezone.utc).isoformat()


def network_packet(frame, link_type):
    """Return (ifindex, packet_type, IP bytes) without merging virtual links."""
    if link_type == 276:  # Linux cooked v2
        if len(frame) < 20:
            raise ValueError('short Linux SLL2 header')
        protocol = struct.unpack_from('!H', frame)[0]
        interface = struct.unpack_from('!I', frame, 4)[0]
        direction, offset = frame[10], 20
    elif link_type == 113:  # Linux cooked v1
        if len(frame) < 16:
            raise ValueError('short Linux SLL header')
        protocol = struct.unpack_from('!H', frame, 14)[0]
        direction, interface, offset = struct.unpack_from('!H', frame)[0], None, 16
    elif link_type == 1:  # Ethernet, including 802.1Q / QinQ
        if len(frame) < 14:
            raise ValueError('short Ethernet header')
        protocol = struct.unpack_from('!H', frame, 12)[0]
        direction, interface, offset = None, None, 14
    elif link_type in (101, 228):  # Raw IPv4
        protocol, direction, interface, offset = 0x0800, None, None, 0
    else:
        raise ValueError(f'unsupported link type {link_type}; supported: Ethernet, SLL, SLL2, raw IPv4')
    while protocol in (0x8100, 0x88a8, 0x9100):
        if len(frame) < offset + 4:
            raise ValueError('short VLAN header')
        protocol = struct.unpack_from('!H', frame, offset + 2)[0]
        offset += 4
    return (interface, direction, frame[offset:]) if protocol == 0x0800 else None


def analyze(path, destination=None, port=None, cbr_bitrate=None, extract_dir=None, allow_truncated=False):
    if cbr_bitrate is not None and not 100000 <= cbr_bitrate <= 200000000:
        raise ValueError('declared CBR bitrate must be 100000..200000000 bit/s')
    counts = Counter()
    flows = {}
    with ExitStack() as stack:
        raw = stack.enter_context(open(path, 'rb'))
        compressed = raw.read(2) == b'\x1f\x8b'
        raw.seek(0)
        f = stack.enter_context(gzip.GzipFile(fileobj=raw)) if compressed else raw
        header = f.read(24)
        magic_formats = {b'\xd4\xc3\xb2\xa1': ('<', 1000), b'\xa1\xb2\xc3\xd4': ('>', 1000),
                         b'\x4d\x3c\xb2\xa1': ('<', 1), b'\xa1\xb2\x3c\x4d': ('>', 1)}
        if len(header) != 24 or header[:4] not in magic_formats:
            raise ValueError('expected classic pcap (optionally gzip); pcapng is not supported')
        endian, ns_per_unit = magic_formats[header[:4]]
        major, minor, _, _, snaplen, link_type = struct.unpack(endian + 'HHIIII', header[4:])
        link_type &= 0xffff
        if (major, minor) != (2, 4):
            raise ValueError(f'unsupported pcap version {major}.{minor}')
        if link_type not in (1, 101, 113, 228, 276):
            raise ValueError(f'unsupported pcap link type {link_type}')
        if extract_dir is not None:
            extract_dir = Path(extract_dir)
            extract_dir.mkdir(parents=True, exist_ok=False)
        while True:
            record = f.read(16)
            if not record:
                break
            if len(record) != 16:
                if allow_truncated:
                    counts['truncated_pcap_tail_header'] += 1
                    break
                raise ValueError('truncated pcap record header')
            seconds, fraction, captured, original = struct.unpack(endian + 'IIII', record)
            if captured > 16 * 1024 * 1024:
                raise ValueError('unreasonable pcap record length')
            frame = f.read(captured)
            if len(frame) != captured:
                if allow_truncated:
                    counts['truncated_pcap_tail_data'] += 1
                    break
                raise ValueError('truncated pcap record data')
            counts['records'] += 1
            if captured < original:
                counts['snaplen_truncated_records'] += 1
            try:
                net = network_packet(frame, link_type)
                if net is None:
                    counts['non_ipv4_records'] += 1
                    continue
                interface, direction, ip = net
                if len(ip) < 20 or ip[0] >> 4 != 4:
                    raise ValueError('invalid IPv4 header')
                ihl = (ip[0] & 15) * 4
                total = struct.unpack_from('!H', ip, 2)[0]
                if ihl < 20 or total < ihl or len(ip) < ihl:
                    raise ValueError('invalid IPv4 lengths')
                if ip[9] != 17:
                    counts['non_udp_records'] += 1
                    continue
                source, dest = str(ipaddress.IPv4Address(ip[12:16])), str(ipaddress.IPv4Address(ip[16:20]))
                if destination and dest != destination:
                    counts['other_destination_records'] += 1
                    continue
                fragments = struct.unpack_from('!H', ip, 6)[0]
                if fragments & 0x3fff:
                    counts['ipv4_fragment_records_skipped'] += 1
                    continue  # A port-only tcpdump filter may omit later fragments.
                if len(ip) < total or total < ihl + 8:
                    raise ValueError('truncated IPv4/UDP packet')
                sport, dport, udp_len, _ = struct.unpack_from('!HHHH', ip, ihl)
                if port and dport != port:
                    counts['other_port_records'] += 1
                    continue
                if udp_len < 8 or ihl + udp_len > total:
                    raise ValueError('invalid UDP length')
                payload = ip[ihl + 8:ihl + udp_len]
                key = (interface, direction, source, sport, dest, dport)
                if key not in flows:
                    extracted_ts = writer = None
                    if extract_dir is not None:
                        extracted_ts = extract_dir / f'if{interface}-type{direction}-{source}-{sport}-to-{dest}-{dport}.ts'
                        writer = stack.enter_context(extracted_ts.open('xb'))
                    flows[key] = Flow(key, cbr_bitrate, extracted_ts, writer)
                flows[key].push(seconds * 1000000000 + fraction * ns_per_unit,
                                payload, ip[8], ip[1], fragments & 0x4000)
                counts['selected_udp_records'] += 1
            except ValueError as error:
                counts['malformed_or_truncated_records'] += 1
                counts['error: ' + str(error)] += 1
    reports = [flow.report() for flow in flows.values()]
    sockets = {(flow['source_ip'], flow['source_port']) for flow in reports}
    return dict(input=str(path), link_type=link_type, timestamp_resolution_ns=ns_per_unit,
                snaplen=snaplen, counts=dict(counts), source_sockets=len(sockets), flows=reports,
                pcap_tail_truncated=bool(counts['truncated_pcap_tail_header'] or counts['truncated_pcap_tail_data']),
                notes=['Interfaces and source sockets are analysed separately to avoid counting bridge/bond copies as CC errors.',
                       'Capture timestamps do not prove NIC wire timing or receiver delivery; checksums are not checked because of TX offload.',
                       'CC errors are sequence violations, not an exact count of lost packets. PCR gaps use media timestamps.',
                       'PCR CBR phase compares relative PCR values with TS byte positions at the explicitly declared rate, not capture timestamps. Capture/TS losses invalidate this comparison; it is not a wire-level jitter certification.',
                       'Record counts alone do not establish that the capture had no drops; also inspect the tcpdump log.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--destination', type=str)
    parser.add_argument('--port', type=int)
    parser.add_argument('--cbr-bitrate', type=int, help='declared full TS bitrate in bit/s for PCR/packet-position checks')
    parser.add_argument('--extract-dir', type=Path, help='new directory for exact per-flow UDP payload files')
    parser.add_argument('--allow-truncated', action='store_true', help='report complete records before a truncated PCAP tail, explicitly marking the capture incomplete')
    parser.add_argument('--json', type=Path, help='write detailed JSON here')
    args = parser.parse_args()
    try:
        result = analyze(args.capture, args.destination, args.port, args.cbr_bitrate, args.extract_dir, args.allow_truncated)
    except (OSError, EOFError, ValueError) as error:
        parser.exit(1, f'Cannot analyse capture: {error}\n')
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f"Records={result['counts'].get('records', 0)} selected_UDP={result['counts'].get('selected_udp_records', 0)} source_sockets={result['source_sockets']}")
    print('Capture warnings:', {k: v for k, v in result['counts'].items() if k not in ('records', 'selected_udp_records')})
    if result['pcap_tail_truncated']:
        print('WARNING: PCAP tail is truncated. Only complete preceding records are analysed; this is an incomplete capture.')
    for flow in result['flows']:
        print(f"\nInterface={flow['interface_index']} packet_type={flow['packet_type']} {flow['source_ip']}:{flow['source_port']} -> {flow['destination_ip']}:{flow['destination_port']}")
        print(f"Datagrams={flow['datagrams']} span_s={flow['span_seconds']} TS_Mbps={flow['payload_mbps']} lengths={flow['payload_lengths']}")
        print(f"IP_TTL={flow['ip_ttl_counts']} DSCP={flow['ip_dscp_counts']} ECN={flow['ip_ecn_counts']} DF_datagrams={flow['ip_dont_fragment_datagrams']} NULL_percent={flow['null_percent']}")
        print('Datagram gaps (us):', flow['gap_us'], 'gaps over ms:', flow['gaps_over_ms'])
        print(f"CC_errors={flow['cc_errors']} invalid_TS_datagrams={flow['invalid_ts_datagrams']} backwards_capture_timestamps={flow['backwards_capture_timestamps']}")
        print('PID      Packets     CC    Dup    TEI    PCR   PCR_gap_ms  PCR_backwards')
        for pid in flow['pids']:
            print(f"{pid['pid_hex']} {pid['packets']:9} {pid['cc_errors']:6} {pid['exact_duplicates']:6} {pid['tei']:6} {pid['pcr_count']:6} {pid['pcr_gap_max_ms']:12.3f} {pid['pcr_backwards']:6}")
            if pid['pcr_cbr_phase_span_us'] is not None:
                print(f"  PCR/CBR relative phase: min={pid['pcr_cbr_phase_min_us']} max={pid['pcr_cbr_phase_max_us']} span={pid['pcr_cbr_phase_span_us']} us (declared {args.cbr_bitrate} bit/s)")
        if flow['extracted_ts']:
            print('Extracted TS bytes:', flow['extracted_ts'])
    if args.json:
        print('\nDetailed report:', args.json)
    if not result['flows']:
        parser.exit(2, 'No matching complete IPv4 UDP datagrams were found.\n')


if __name__ == '__main__':
    main()
