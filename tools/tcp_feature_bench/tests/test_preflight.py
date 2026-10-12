"""Functional input validation against the actual executable; no NIC/model needed."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

BENCH = str(pathlib.Path(sys.argv.pop(1)).resolve())

class Preflight(unittest.TestCase):
    def run_case(self, link=1, caplen=60, original=60, empty=False):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'input.pcap'
            data = struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, link)
            if not empty:
                frame = b'\x02\x00\x00\x00\x00\x01\x02\x00\x00\x00\x00\x02\x08\x00' + bytes(max(0, caplen-14))
                data += struct.pack('<IIII', 1, 123, caplen, original) + frame[:caplen]
            path.write_bytes(data)
            result = subprocess.run([BENCH, '--role', 'backend', '--prepare-only', '-p', str(path)],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(path.read_bytes(), data, 'original PCAP was modified')
            return result

    def test_complete_ethernet(self):
        result = self.run_case()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PCAP preflight passed', result.stdout)

    def test_truncated_record(self):
        self.assertNotEqual(self.run_case(original=100).returncode, 0)

    def test_non_ethernet(self):
        self.assertNotEqual(self.run_case(link=12).returncode, 0)

    def test_empty(self):
        self.assertNotEqual(self.run_case(empty=True).returncode, 0)

    def test_short_ethernet_header(self):
        self.assertNotEqual(self.run_case(caplen=10, original=10).returncode, 0)

if __name__ == '__main__':
    unittest.main()
