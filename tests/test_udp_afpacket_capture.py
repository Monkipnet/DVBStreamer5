"""Real Linux cBPF/timestamp tests using UNIX datagrams; no CAP_NET_RAW needed."""
import importlib.util
import io
from pathlib import Path
import socket
import struct
import sys
import tempfile
import time
import unittest


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


ROOT = Path(__file__).resolve().parents[1]
raw = module('afpacket', ROOT / 'scripts/capture_udp_afpacket.py')
fixture = module('pcap_fixture', ROOT / 'tests/test_udp_pcap_analysis.py')


@unittest.skipUnless(sys.platform.startswith('linux'), 'Linux cBPF ancillary packet type')
class PacketCaptureTests(unittest.TestCase):
    def setUp(self):
        self.receiver, self.sender = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.addCleanup(self.receiver.close)
        self.addCleanup(self.sender.close)
        self.receiver.settimeout(.05)
        self.destination = '227.9.9.1'
        # UNIX sk_buffs have packet type HOST=0. Production uses OUTGOING=4.
        raw.attach_filter(self.receiver, raw.packet_filter(self.destination, packet_type=0))

    def assertAccepted(self, frame):
        self.sender.send(frame)
        self.assertEqual(self.receiver.recv(65535), frame)

    def assertRejected(self, frame):
        self.sender.send(frame)
        with self.assertRaises(socket.timeout):
            self.receiver.recv(65535)

    def test_ipv4_udp_payload_is_preserved(self):
        self.assertAccepted(fixture.frame(fixture.ts(0), link=1))

    def test_other_destinations_protocols_and_short_frames_are_rejected(self):
        frame = bytearray(fixture.frame(fixture.ts(0), link=1))
        frame[14 + 19] = 2
        self.assertRejected(frame)
        frame[14 + 19] = 1
        frame[14 + 9] = 6
        self.assertRejected(frame)
        self.assertRejected(b'bad')

    def test_vlan_and_qinq_are_preserved(self):
        self.assertAccepted(fixture.frame(fixture.ts(0), link=1, vlan=True))
        ip = fixture.ip_udp(fixture.ts(1))
        for outer, inner in ((0x88a8, 0x8100), (0x8100, 0x8100)):
            frame = b'abcdefghijkl' + struct.pack('!HHHHH', outer, 1, inner, 2, 0x0800) + ip
            self.assertAccepted(frame)

    def test_noninitial_fragments_are_retained(self):
        self.assertAccepted(fixture.frame(fixture.ts(0), link=1, fragments=0x2000))
        self.assertAccepted(fixture.frame(fixture.ts(1), link=1, fragments=1))

    def test_other_ports_are_retained_for_offline_selection(self):
        frame = bytearray(fixture.frame(fixture.ts(0), link=1))
        frame[14 + 22:14 + 24] = struct.pack('!H', 4321)
        self.assertAccepted(frame)

    def test_production_filter_rejects_non_outgoing_packet_types(self):
        raw.attach_filter(self.receiver, raw.packet_filter(self.destination))
        self.assertRejected(fixture.frame(fixture.ts(0), link=1))

    def test_kernel_timestamps_and_pcap_are_readable_by_analyser(self):
        self.receiver.setsockopt(socket.SOL_SOCKET, raw.SO_TIMESTAMPNS_NEW, 1)
        stats = dict(records=0, captured_bytes=0, timestamp_fallbacks=0, truncated_frames=0)
        payloads = [b''.join(fixture.ts(i * 7 + j) for j in range(7)) for i in range(2)]
        for i, payload in enumerate(payloads):
            self.sender.send(fixture.frame(payload, link=1, vlan=bool(i)))
        output = io.BytesIO()
        deadline = time.monotonic() + 2
        raw.receive_to_pcap(self.receiver, output,
                            lambda: stats['records'] >= 2 or time.monotonic() > deadline, stats)
        self.assertEqual(stats['records'], 2)
        self.assertEqual(stats['timestamp_fallbacks'], 0)
        self.assertEqual(stats['truncated_frames'], 0)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'capture.pcap'
            path.write_bytes(output.getvalue())
            result = fixture.pcap.analyze(path, extract_dir=Path(tmp) / 'ts')
            self.assertEqual(result['link_type'], 1)
            self.assertEqual(result['timestamp_resolution_ns'], 1)
            self.assertEqual(result['counts']['records'], 2)
            flow = result['flows'][0]
            self.assertEqual(flow['cc_errors'], 0)
            self.assertEqual(Path(flow['extracted_ts']).read_bytes(), b''.join(payloads))


if __name__ == '__main__':
    unittest.main()
