"""Standalone packet-capture diagnostics tests; no network privileges needed."""
import gzip
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('udp_pcap', Path(__file__).resolve().parents[1] / 'scripts/analyze_udp_pcap.py')
pcap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pcap)


def ts(index, discontinuity=False):
    packet = bytearray(b'\xff' * 188)
    packet[:6] = bytes((0x47, 1, 2, 0x30 | (index & 15), 7, 0x90 if discontinuity else 0x10))
    ticks = (pcap.PCR_WRAP - 10000 + index * 188 * 8 * 27000000 // 5000000) % pcap.PCR_WRAP
    base, ext = divmod(ticks, 300)
    packet[6:12] = bytes((base >> 25 & 255, base >> 17 & 255, base >> 9 & 255,
                          base >> 1 & 255, ((base & 1) << 7) | 0x7e | (ext >> 8), ext & 255))
    packet[20:24] = struct.pack('!I', index)
    return bytes(packet)


def ip_udp(payload, sport=45000, fragments=0, ttl=64, tos=0):
    udp = struct.pack('!HHHH', sport, 56444, len(payload) + 8, 0) + payload
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, tos, len(udp) + 20, 1, fragments, ttl, 17, 0,
                     bytes((192, 0, 2, 10)), bytes((227, 9, 9, 1)))
    return ip + udp


def frame(payload, link=276, interface=2, sport=45000, vlan=False, fragments=0, ttl=64, tos=0):
    ip = ip_udp(payload, sport, fragments, ttl, tos)
    protocol = 0x8100 if vlan else 0x0800
    if vlan:
        ip = struct.pack('!HH', 42, 0x0800) + ip
    if link == 276:
        return struct.pack('!HHIHBB8s', protocol, 0, interface, 1, 4, 6, b'123456\0\0') + ip
    if link == 113:
        return struct.pack('!HHH8sH', 4, 1, 6, b'123456\0\0', protocol) + ip
    if link == 1:
        return b'abcdefghijkl' + struct.pack('!H', protocol) + ip
    return ip


def capture(records, link=276, endian='<', nanos=False):
    magic = 0xa1b23c4d if nanos else 0xa1b2c3d4
    result = struct.pack(endian + 'IHHIIII', magic, 2, 4, 0, 0, 262144, link)
    for timestamp_ns, data, original in records:
        seconds, fraction = divmod(timestamp_ns, 1000000000)
        if not nanos:
            fraction //= 1000
        result += struct.pack(endian + 'IIII', seconds, fraction, len(data), original or len(data)) + data
    return result


class CaptureTests(unittest.TestCase):
    def analyse(self, data, compressed=False, cbr_bitrate=None):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / ('capture.pcap.gz' if compressed else 'capture.pcap')
            path.write_bytes(gzip.compress(data) if compressed else data)
            return pcap.analyze(path, '227.9.9.1', 56444, cbr_bitrate)

    def records(self, link=276):
        return [(1000000000 + int(i * 2105600), frame(b''.join(ts(i * 7 + n) for n in range(7)), link), None)
                for i in range(10)]

    def test_cbr_sizes_rate_cc_and_pcr_wrap(self):
        result = self.analyse(capture(self.records()), compressed=True)
        flow = result['flows'][0]
        self.assertEqual(result['counts']['records'], 10)
        self.assertEqual(result['source_sockets'], 1)
        self.assertEqual(flow['payload_lengths'], {1316: 10})
        self.assertAlmostEqual(flow['payload_mbps'], 5, places=3)
        self.assertEqual(flow['cc_errors'], 0)
        self.assertEqual(flow['pids'][0]['pcr_backwards'], 0)
        self.assertEqual(flow['pids'][0]['pcr_count'], 70)
        self.assertEqual(flow['ip_ttl_counts'], {64: 10})
        self.assertEqual(flow['null_percent'], 0)

    def test_ipv4_transport_parameters_and_null_budget(self):
        null = bytearray(ts(99)); null[1] = 0x1f; null[2] = 0xff
        records = [(0, frame(ts(0), ttl=1), None),
                   (2105600, frame(ts(1) + bytes(null), ttl=4, tos=(34 << 2) | 2, fragments=0x4000), None)]
        flow = self.analyse(capture(records))['flows'][0]
        self.assertEqual(flow['ip_ttl_counts'], {1: 1, 4: 1})
        self.assertEqual(flow['ip_dscp_counts'], {0: 1, 34: 1})
        self.assertEqual(flow['ip_ecn_counts'], {0: 1, 2: 1})
        self.assertEqual(flow['ip_dont_fragment_datagrams'], 1)
        self.assertEqual(flow['ts_packets'], 3)
        self.assertEqual(flow['null_packets'], 1)
        self.assertAlmostEqual(flow['null_percent'], 100 / 3, places=5)
        self.assertEqual(flow['cc_errors'], 0)

    def test_dropped_datagram_is_a_cc_error(self):
        records = self.records()
        del records[3]
        flow = self.analyse(capture(records))['flows'][0]
        self.assertEqual(flow['cc_errors'], 1)
        self.assertGreater(flow['gap_us']['max'], 4000)

    def test_fixed_pcr_matches_cbr_positions_through_wrap(self):
        flow = self.analyse(capture(self.records()), cbr_bitrate=5000000)['flows'][0]
        stats = flow['pids'][0]
        self.assertEqual(stats['pcr_cbr_phase_samples'], 70)
        self.assertLess(stats['pcr_cbr_phase_span_us'], .038)

    def test_cc_clean_null_insertion_can_change_pcr_byte_clock(self):
        null = bytes((0x47, 0x1f, 0xff, 0x10)) + b'\xff' * 184
        payload = ts(0) + null + ts(1)
        flow = self.analyse(capture([(0, frame(payload), None)]), cbr_bitrate=5000000)['flows'][0]
        stats = next(p for p in flow['pids'] if p['pid'] == 258)
        self.assertEqual(flow['cc_errors'], 0)
        self.assertGreater(stats['pcr_cbr_phase_span_us'], 300)
        self.assertLess(stats['pcr_cbr_phase_span_us'], 301)

    def test_pcr_phase_is_opt_in(self):
        stats = self.analyse(capture(self.records()))['flows'][0]['pids'][0]
        self.assertIsNone(stats['pcr_cbr_phase_span_us'])

    def test_extracts_exact_bytes_with_interfaces_kept_separate(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            source = tmp / 'input.pcap.gz'
            payloads = [b''.join(ts(i * 7 + n) for n in range(7)) for i in range(2)]
            records = [(i * 2105600, frame(payload, interface=iface), None)
                       for i, payload in enumerate(payloads) for iface in (2, 3)]
            source.write_bytes(gzip.compress(capture(records)))
            out = tmp / 'ts'
            result = pcap.analyze(source, '227.9.9.1', 56444, 5000000, out)
            self.assertEqual(len(result['flows']), 2)
            for flow in result['flows']:
                self.assertEqual(Path(flow['extracted_ts']).read_bytes(), b''.join(payloads))
            with self.assertRaises(FileExistsError):
                pcap.analyze(source, extract_dir=out)

    def test_interfaces_and_source_ports_are_separate(self):
        records = []
        for i in range(4):
            payload = b''.join(ts(i * 7 + n) for n in range(7))
            for interface, sport in ((2, 45000), (3, 45000), (2, 45001)):
                records.append((1000000000 + i * 2105600, frame(payload, interface=interface, sport=sport), None))
        result = self.analyse(capture(records))
        self.assertEqual(result['source_sockets'], 2)
        self.assertEqual(len(result['flows']), 3)
        self.assertTrue(all(f['cc_errors'] == 0 and f['datagrams'] == 4 for f in result['flows']))

    def test_link_headers_and_timestamp_encodings(self):
        for link in (1, 113, 276, 101, 228):
            for endian in ('<', '>'):
                for nanos in (False, True):
                    with self.subTest(link=link, endian=endian, nanos=nanos):
                        flow = self.analyse(capture(self.records(link), link, endian, nanos))['flows'][0]
                        self.assertEqual(flow['datagrams'], 10)
                        self.assertEqual(flow['cc_errors'], 0)
        for link in (1, 113, 276):
            data = frame(b''.join(ts(n) for n in range(7)), link=link, vlan=True)
            self.assertEqual(self.analyse(capture([(0, data, None)], link))['flows'][0]['datagrams'], 1)

    def test_duplicates_and_discontinuity(self):
        payload = ts(0) + ts(1) + ts(1) + ts(9, discontinuity=True) + ts(10)
        flow = self.analyse(capture([(0, frame(payload), None)]))['flows'][0]
        stats = flow['pids'][0]
        self.assertEqual(stats['cc_errors'], 0)
        self.assertEqual(stats['exact_duplicates'], 1)
        self.assertEqual(stats['discontinuities'], 1)

    def test_truncated_packets_and_fragments_are_reported(self):
        data = frame(b''.join(ts(n) for n in range(7)))
        records = [(0, data[:-20], len(data)), (1, frame(ts(0), fragments=0x2000), None)]
        result = self.analyse(capture(records))
        self.assertEqual(result['counts']['snaplen_truncated_records'], 1)
        self.assertEqual(result['counts']['malformed_or_truncated_records'], 1)
        self.assertEqual(result['counts']['ipv4_fragment_records_skipped'], 1)
        self.assertEqual(result['flows'], [])

    def test_invalid_ts_is_not_counted_as_media(self):
        result = self.analyse(capture([(0, frame(b'bad' * 10), None)]))
        self.assertEqual(result['flows'][0]['invalid_ts_datagrams'], 1)
        self.assertEqual(result['flows'][0]['pids'], [])

    def test_truncated_pcap_fails_instead_of_returning_partial_success(self):
        with self.assertRaisesRegex(ValueError, 'truncated pcap record data'):
            self.analyse(capture(self.records())[:-1])

    def test_explicit_truncated_mode_extracts_only_complete_records(self):
        records = self.records()[:3]
        tails = [capture(records[:2]) + b'partial', capture(records)[:-12]]
        for content in tails:
            for compressed in (False, True):
                with self.subTest(compressed=compressed), tempfile.TemporaryDirectory() as tmp:
                    tmp = Path(tmp)
                    source = tmp / 'capture.pcap'
                    source.write_bytes(gzip.compress(content) if compressed else content)
                    result = pcap.analyze(source, extract_dir=tmp / 'ts', allow_truncated=True)
                    self.assertTrue(result['pcap_tail_truncated'])
                    self.assertEqual(result['counts']['records'], 2)
                    self.assertEqual(result['flows'][0]['datagrams'], 2)
                    expected = b''.join(ts(i) for i in range(14))
                    self.assertEqual(Path(result['flows'][0]['extracted_ts']).read_bytes(), expected)

    def test_truncated_mode_still_rejects_invalid_global_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'invalid.pcap'
            source.write_bytes(b'not a capture')
            with self.assertRaisesRegex(ValueError, 'expected classic pcap'):
                pcap.analyze(source, allow_truncated=True)


if __name__ == '__main__':
    unittest.main()
