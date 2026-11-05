"""Preflight regressions for the PowerShell capture helper; no FFmpeg needed.

Set DVBSTREAMER5_POWERSHELL to a PowerShell executable, or put pwsh/powershell
on PATH. The checks reject bad interface selection before creating a capture.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

POWERSHELL = os.environ.get('DVBSTREAMER5_POWERSHELL') or shutil.which('pwsh') or shutil.which('powershell')
SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/compare_udp_srt.ps1'
MISSING_EXE = 'dvbstreamer5-test-no-such-ffmpeg-executable'


@unittest.skipUnless(POWERSHELL, 'PowerShell is not installed')
class InterfacePreflightTests(unittest.TestCase):
    def check_preflight(self, options, expected, valid=False):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'capture'
            result = subprocess.run([
                POWERSHELL, '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                '-File', str(SCRIPT), '-UdpUrl', 'udp://239.10.10.1:1234',
                '-SrtUrl', 'srt://192.0.2.10:34000?mode=caller',
                '-DurationSeconds', '5', '-OutputDirectory', str(output),
                '-Ffmpeg', MISSING_EXE, *options,
            ], capture_output=True, text=True, timeout=15)
            combined = result.stdout + result.stderr
            self.assertNotEqual(result.returncode, 0, combined)
            self.assertIn(expected, combined)
            self.assertFalse(output.exists(), 'preflight must not create a misleading capture directory')
            if not valid:
                self.assertNotIn('Get-Command', combined, 'invalid interface must fail before FFmpeg lookup')

    def test_unassigned_address_rejected_before_capture(self):
        self.check_preflight(['-InterfaceIP', '203.0.113.254'], 'not assigned to a connected local adapter')

    def test_ipv6_cannot_be_used_as_ipv4_interface(self):
        self.check_preflight(['-InterfaceIP', '::1'], 'InterfaceIP must be an IPv4 address')

    def test_conflicting_automatic_and_manual_selection_rejected(self):
        self.check_preflight(['-Ethernet', '-InterfaceIP', '127.0.0.1'], 'Use either -Ethernet or -InterfaceIP')

    def test_real_loopback_is_accepted_before_missing_executable(self):
        self.check_preflight(['-InterfaceIP', '127.0.0.1'], 'UDP interface:', valid=True)


if __name__ == '__main__':
    unittest.main()
