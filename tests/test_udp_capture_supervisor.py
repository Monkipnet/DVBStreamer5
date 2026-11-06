"""Exercise capture supervision with local processes, without network privileges."""
import importlib.util
from pathlib import Path
import signal
import sys
import unittest

SPEC = importlib.util.spec_from_file_location('udp_capture', Path(__file__).resolve().parents[1] / 'scripts/capture_udp_pcap.py')
supervisor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(supervisor)


class SupervisorTests(unittest.TestCase):
    def run_child(self, code, seconds=1, grace=1):
        return supervisor.capture([sys.executable, '-c', code], seconds, grace)

    def test_sigsegv_is_recorded_without_killing_supervisor(self):
        result, status = self.run_child('import os,resource,signal; resource.setrlimit(resource.RLIMIT_CORE,(0,0)); os.kill(os.getpid(),signal.SIGSEGV)')
        self.assertEqual(status, 139)
        self.assertEqual(result['child_returncode'], -signal.SIGSEGV)
        self.assertFalse(result['completed'])
        self.assertFalse(result['duration_reached'])

    def test_early_clean_exit_is_not_a_complete_capture(self):
        result, status = self.run_child('pass')
        self.assertNotEqual(status, 0)
        self.assertEqual(result['child_returncode'], 0)
        self.assertFalse(result['completed'])

    def test_deadline_sends_sigint_for_flush(self):
        code = 'import signal,sys,time; signal.signal(signal.SIGINT,lambda *_:sys.exit(0)); time.sleep(30)'
        result, status = self.run_child(code)
        self.assertEqual(status, 0)
        self.assertTrue(result['completed'])
        self.assertTrue(result['duration_reached'])
        self.assertFalse(result['forced_kill'])

    def test_unresponsive_child_is_killed_and_marked_incomplete(self):
        code = 'import signal,time; signal.signal(signal.SIGINT,signal.SIG_IGN); time.sleep(30)'
        result, status = self.run_child(code, grace=.1)
        self.assertEqual(status, 137)
        self.assertEqual(result['child_returncode'], -signal.SIGKILL)
        self.assertTrue(result['forced_kill'])
        self.assertFalse(result['completed'])

    def test_child_error_status_is_preserved(self):
        result, status = self.run_child('import sys; sys.exit(7)')
        self.assertEqual(status, 7)
        self.assertFalse(result['completed'])

    def test_missing_executable_is_reported(self):
        result, status = supervisor.capture(['/nonexistent/dvb-diagnostic-tcpdump'], 1)
        self.assertEqual(status, 127)
        self.assertIn('error', result)
        self.assertFalse(result['completed'])


if __name__ == '__main__':
    unittest.main()
