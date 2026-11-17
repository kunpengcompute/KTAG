"""Compile actual UB executors with an in-memory transport and inference mock.

No DPDK, NIC or ONNX runtime is required. Run from any directory:
  python test/common/ub_queue_worker_check.py --cxx c++
  python test/common/ub_queue_worker_check.py --zig /path/to/zig

This checks application concurrency/ownership, not real UB performance or SDK
memory ordering. Generated sources/binaries stay under build/ub-worker-check.
"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--zig')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = root / 'build/ub-worker-check'
    build.mkdir(parents=True, exist_ok=True)
    runner = (root / 'tools/demo/pipeline/pipeline_runner.cpp').read_text(encoding='utf-8')

    def between(start, end):
        first = runner.index(start)
        return runner[first:runner.index(end, first)]

    fixture = (Path(__file__).with_suffix('.cpp')).read_text(encoding='utf-8')
    actual = '\n'.join([
        between('class worker_thread_scope {', '#include <arpa/inet.h>'),
        between('class owned_packet {', 'bool is_recoverable_truncated_capture'),
        between('bool is_recoverable_truncated_capture', 'void add_source_statistics('),
        between('uint64_t timestamp_us(', 'bool flow_key_less('),
        between('bool append_predictions(', '#if KTAG_ENABLE_DPDK_UB'),
        between('bool append_distributed_predictions(', '#endif'),
        between('bool copy_feature_records(', 'domain_store_ptr create_domain_store()'),
        between('inference_context_ptr\ncreate_inference_context(', 'schema_ptr compose_schema'),
        between('class parallel_source_executor {', '#if KTAG_ENABLE_DPDK_UB'),
        between('uint64_t ub_identity_hash(', 'bool run_ub_frontend_inputs('),
    ])
    generated = build / 'queue_workers_check.cpp'
    generated.write_text(fixture.replace('// INSERT_ACTUAL_EXECUTORS', actual), encoding='utf-8')
    executable = build / ('queue_workers_check.exe' if os.name == 'nt' else 'queue_workers_check')
    command = [str(Path(args.zig).resolve()), 'c++'] if args.zig else [args.cxx]
    command += ['-std=c++17', '-O1', '-UNDEBUG', '-DKTAG_BUILDING_DLL', '-DKTAG_ENABLE_DPDK_UB=1']
    if os.name != 'nt':
        command += ['-pthread']
    for include in ['include', 'tools/demo/pipeline', 'src/inference']:
        command += ['-I' + str(root / include)]
    command += [str(generated), str(root / 'tools/demo/pipeline/ub_feature_wire.cpp'),
                str(root / 'src/feature/feature_batch.cpp'),
                str(root / 'src/inference/prediction_batch.cpp'), '-o', str(executable)]
    environment = os.environ.copy()
    environment.setdefault('ZIG_GLOBAL_CACHE_DIR', str(build / 'cache'))
    subprocess.run(command, check=True, cwd=root, env=environment, timeout=180)
    result = subprocess.run([str(executable)], cwd=root, timeout=60,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding='utf-8', errors='replace')
    log = build / 'queue_workers_check.log'
    log.write_text(result.stdout, encoding='utf-8')
    if result.returncode:
        print(result.stdout[-10000:])
        result.check_returncode()
    for line in result.stdout.splitlines():
        if line.startswith('PASS '):
            print(line)
    print('Full execution log:', log)


if __name__ == '__main__':
    main()
