"""Actual KTAG frontend against a protocol peer; does not test model inference."""
import ctypes
import ctypes.util
import json
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

BENCH = str(pathlib.Path(sys.argv.pop(1)).resolve())
MAGIC = 0x4B54414750434131
HEADER = struct.Struct('>QIIQQIIII')
ZSTD = ctypes.CDLL(ctypes.util.find_library('zstd'))
ZSTD.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
ZSTD.ZSTD_decompress.restype = ctypes.c_size_t

def text(value):
    data = value.encode()
    return struct.pack('>I', len(data)) + data

def exact(sock, size):
    result = b''
    while len(result) < size:
        chunk = sock.recv(size-len(result))
        if not chunk:
            raise EOFError('peer closed')
        result += chunk
    return result

def receive(sock):
    magic, version, kind, run, seq, count, raw, length, flags = HEADER.unpack(exact(sock, HEADER.size))
    assert magic == MAGIC and version == 1 and not flags
    body = exact(sock, length)
    if kind == 11:
        raise RuntimeError('frontend ERROR: ' + body[4:].decode())
    return kind, run, seq, count, raw, body

def send(sock, kind, run=42, body=b''):
    sock.sendall(HEADER.pack(MAGIC, 1, kind, run, 0, 0, 0, len(body), 0) + body)

def pcap(path, partial=False, empty=False, short=False):
    data = struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1)
    eth = bytes.fromhex('0200000000010200000000020800')
    ip = bytes.fromhex('4500002e00000000401100000a0000010a000002')
    udp = struct.pack('>HHHH', 12345, 54321, 26, 0)
    frame = eth + ip + udp + bytes(18)
    if not empty:
        for i in range(3):
            data += struct.pack('<IIII', 100+i, 123456, len(frame), 9000 if short else len(frame)) + frame
    if partial:
        data += struct.pack('<IIII', 103, 0, len(frame), len(frame)) + frame[:3]
    path.write_bytes(data)

class Frontend(unittest.TestCase):
    def run_case(self, partial=False, disconnect=False):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            paths = [root/'one.pcap', root/'empty.pcap', root/'short.pcap']
            pcap(paths[0], partial=partial); pcap(paths[1], empty=True); pcap(paths[2], short=True)
            config = root/'config.yaml'
            config.write_text('capture:\n  packet_pool_size: 1\nfeature:\n  packet_queue_capacity: 1\n'
                              'inference:\n  queue_capacity: 1\ntransport:\n  progress_timeout_s: 2\n')
            listener = socket.socket(); listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(10)
            port = listener.getsockname()[1]
            errors, summaries, raw_stream = [], [], bytearray()
            def peer():
                try:
                    with listener.accept()[0] as sock:
                        sock.settimeout(10)
                        kind, run, _, _, _, hello = receive(sock)
                        self.assertEqual((kind, run, struct.unpack('>I', hello[-4:])[0]), (1, 0, 3))
                        send(sock, 2)
                        record_seq, batch_seq, total_records = 1, 1, 0
                        for index in range(3):
                            kind, _, _, _, _, body = receive(sock)
                            self.assertEqual((kind, struct.unpack('>I', body[:4])[0]), (3, index))
                            if disconnect:
                                return
                            send(sock, 4, body=struct.pack('>I', index))
                            records = 0
                            while True:
                                kind, run, seq, count, raw, body = receive(sock)
                                self.assertEqual(run, 42)
                                if kind == 10:
                                    self.assertEqual(struct.unpack('>I', body[:4])[0], index)
                                    send(sock, 10, body=struct.pack('>IQ', index, 0))
                                elif kind == 5:
                                    self.assertEqual(seq, batch_seq); batch_seq += 1
                                    self.assertEqual(raw, count*1613)
                                    output = ctypes.create_string_buffer(raw)
                                    self.assertEqual(ZSTD.ZSTD_decompress(output, raw, body, len(body)), raw)
                                    raw_stream.extend(output.raw)
                                    records += count; record_seq += count
                                else:
                                    self.assertEqual(kind, 6)
                                    values = struct.unpack('>II9Q', body[:80])
                                    warning_size = struct.unpack('>I', body[80:84])[0]
                                    warning = body[84:84+warning_size].decode()
                                    self.assertEqual(values[0], index)
                                    self.assertEqual(values[2], 0 if index == 1 else 3)
                                    self.assertEqual(values[9], records)
                                    self.assertEqual(bool(warning), partial and index == 0)
                                    if index == 1:
                                        self.assertEqual(records, 0)
                                    else:
                                        self.assertGreater(records, 0)
                                    summaries.append(values)
                                    send(sock, 7, body=struct.pack('>I4QI', index, records, records, 0, records, not warning))
                                    total_records += records
                                    break
                        kind, _, _, _, _, body = receive(sock)
                        self.assertEqual(kind, 8)
                        packets, captured, records, batches = struct.unpack('>4Q', body)
                        self.assertEqual((packets, captured, records, batches), (6, 360, total_records, batch_seq-1))
                        send(sock, 9, body=struct.pack('>4Q', records, records, 0, records))
                except BaseException as error:
                    errors.append(error)
                finally:
                    listener.close()
            worker = threading.Thread(target=peer, daemon=True); worker.start()
            args = [BENCH, '--role', 'frontend', '--config', str(config), '--local-ip', '127.0.0.1',
                    '--connect', f'127.0.0.1:{port}', '--stats', str(root/'stats.json'), '--feature-audit', str(root/'audit')]
            for path in paths:
                args += ['-p', str(path)]
            result = subprocess.run(args, capture_output=True, text=True, timeout=20)
            worker.join(timeout=12)
            self.assertFalse(worker.is_alive(), 'protocol peer did not stop')
            if errors:
                raise AssertionError(f'{errors!r}\n{result.stdout}\n{result.stderr}')
            stats = json.loads((root/'stats.json').read_text())
            if disconnect:
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(stats['status'], 'failed')
                self.assertIsNone(stats['input_gbit_s'])
            else:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(stats['status'], 'complete')
                self.assertEqual(stats['input_complete'], not partial)
                self.assertEqual(stats['short_captures'], 3)
                self.assertEqual((root/'audit').read_bytes(), raw_stream)
                self.assertEqual(len(stats['sources']), 3)
                self.assertEqual(bool(stats['warnings']), partial)
                if partial:
                    self.assertIn('WARNING', result.stderr)

    def test_multiple_files_tail_batches_and_snaplen(self):
        self.run_case()

    def test_partial_input_warns_and_exits_successfully(self):
        self.run_case(partial=True)

    def test_disconnect_cancels_pipeline(self):
        self.run_case(disconnect=True)

if __name__ == '__main__':
    unittest.main()
