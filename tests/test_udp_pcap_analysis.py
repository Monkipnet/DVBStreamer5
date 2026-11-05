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
    ticks = (pcap.PCR_WRAP - 10000 + index * 8122) % pcap.PCR_WRAP
    base, ext = divmod(ticks, 300)
    packet[6:12] = bytes((base >> 25 & 255, base >> 17 & 255, base >> 9 & 255,
                          base >> 1 & 255, ((base & 1) << 7) | 0x7e | (ext >> 8), ext & 255))
    packet[20:24] = struct.pack('!I', index)
    return bytes(packet)


def ip_udp(payload, sport=45000, fragments=0):
    udp = struct.pack('!HHHH', sport, 56444, len(payload) + 8, 0) + payload
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, len(udp) + 20, 1, fragments, 64, 17, 0,
                     bytes((192, 0, 2, 10)), bytes((227, 9, 9, 1)))
    return ip + udp


def frame(payload, link=276, interface=2, sport=45000, vlan=False, fragments=0):
    ip = ip_udp(payload, sport, fragments)
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
    def analyse(self, data, compressed=False):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / ('capture.pcap.gz' if compressed else 'capture.pcap')
            path.write_bytes(gzip.compress(data) if compressed else data)
            return pcap.analyze(path, '227.9.9.1', 56444)

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

    def test_dropped_datagram_is_a_cc_error(self):
        records = self.records()
        del records[3]
        flow = self.analyse(capture(records))['flows'][0]
        self.assertEqual(flow['cc_errors'], 1)
        self.assertGreater(flow['gap_us']['max'], 4000)

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


if __name__ == '__main__':
    unittest.main()
