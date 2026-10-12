"""Run both real roles with the supplied Legacy V1 model; no replay or root."""
import csv
import json
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time

def pcap(path, empty=False, partial=False):
    data = struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1)
    frame = (bytes.fromhex('0200000000010200000000020800') +
             bytes.fromhex('4500002e00000000401100000a0000010a000002') +
             struct.pack('>HHHH', 12345, 54321, 26, 0) + bytes(18))
    if not empty:
        for i in range(32):
            data += struct.pack('<IIII', 100+i, 123456, len(frame), len(frame)) + frame
    if partial:
        data += struct.pack('<IIII', 132, 0, len(frame), len(frame)) + frame[:3]
    path.write_bytes(data)

def main():
    if len(sys.argv) != 3:
        raise SystemExit('Usage: test_loopback.py /path/ktag_tcp_pcap_bench /path/legacy_model_package')
    binary, model = [str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:]]
    with tempfile.TemporaryDirectory(prefix='ktag-pcap-loopback-') as directory:
        root = pathlib.Path(directory)
        full, empty, partial = [root/name for name in ['full.pcap', 'empty.pcap', 'partial.pcap']]
        pcap(full); pcap(empty, empty=True); pcap(partial, partial=True)
        config = root/'config.yaml'
        config.write_text('capture:\n  packet_pool_size: 2\nfeature:\n  packet_queue_capacity: 1\n'
                          'inference:\n  queue_capacity: 1\ntransport:\n  progress_timeout_s: 10\n')
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
        backend = None
        with (root/'backend.log').open('w') as blog, (root/'frontend.log').open('w') as flog:
            try:
                backend = subprocess.Popen([binary, '--role', 'backend', '--config', str(config),
                    '--bind-ip', '127.0.0.1', '--port', str(port), '--model', model,
                    '--result', str(root/'result.csv'), '--stats', str(root/'backend.json'),
                    '--feature-audit', str(root/'backend.frames')], stdout=blog, stderr=subprocess.STDOUT)
                deadline = time.monotonic()+60
                while ' listening ' not in (root/'backend.log').read_text(errors='replace'):
                    if backend.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('backend failed to initialize/listen')
                    time.sleep(0.05)
                front = subprocess.run([binary, '--role', 'frontend', '--config', str(config),
                    '--local-ip', '127.0.0.1', '--connect', f'127.0.0.1:{port}',
                    '-p', str(full), '-p', str(empty), '-p', str(partial),
                    '--stats', str(root/'frontend.json'), '--feature-audit', str(root/'frontend.frames')],
                    stdout=flog, stderr=subprocess.STDOUT, timeout=60)
                if front.returncode != 0 or backend.wait(timeout=20) != 0:
                    raise RuntimeError('real frontend/backend failed')
                frontend, server = [json.loads((root/f'{role}.json').read_text()) for role in ['frontend', 'backend']]
                for stats in [frontend, server]:
                    assert stats['status'] == 'complete' and stats['input_complete'] is False
                    assert stats['packets'] == 64 and stats['bytes'] == 3840
                    assert stats['records'] > 0 and stats['records'] == stats['submitted']
                    assert stats['submitted'] == stats['filtered'] + stats['predictions']
                    assert len(stats['sources']) == 3 and stats['sources'][1]['records'] == 0
                    assert len(stats['warnings']) == 1 and stats['sources'][2]['input_complete'] is False
                    assert stats['core_ns'] > 0 and stats['replay_cpu_ns'] is None
                for key in ['records', 'submitted', 'filtered', 'predictions', 'batches', 'raw_bytes', 'compressed_bytes']:
                    assert frontend[key] == server[key], key
                assert (root/'frontend.frames').read_bytes() == (root/'backend.frames').read_bytes()
                with (root/'result.csv').open() as result:
                    assert len(list(csv.DictReader(result))) == server['predictions']
            except BaseException as error:
                details = '\n'.join(f'{name}:\n{(root/name).read_text(errors="replace")[-6000:]}' for name in ['backend.log', 'frontend.log'])
                raise RuntimeError(f'{error}\n{details}') from error
            finally:
                if backend is not None and backend.poll() is None:
                    backend.terminate()
                    try:
                        backend.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        backend.kill(); backend.wait()
    print('PASS: real KTAG/ONNX loopback, EOF UDP, empty/multiple files, partial input, tiny pools, audit and counts')

if __name__ == '__main__':
    main()
