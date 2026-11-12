"""Exercise actual queue-reader code against a deterministic mock PMD.

No NIC/DPDK runtime is required. This checks state isolation, ownership,
registration, RSS validation and counters; hardware RSS/order needs a real NIC.
Run: python test/common/dpdk_rx_worker_check.py --zig /path/to/zig
"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = root / 'build/ub-worker-check'
    build.mkdir(parents=True, exist_ok=True)
    source = (root / 'src/capture_dpdk/dpdk_capture.cpp').read_text(encoding='utf-8')
    state = (root / 'src/capture_dpdk/dpdk_capture_internal.h').read_text(encoding='utf-8')
    actual = state[state.index('struct ktag_dpdk_queue_counters'):state.index('void ktag_dpdk_set_error_internal')]
    actual += source[source.index('KTAG_API ktag_status ktag_dpdk_capture_next('):source.index('KTAG_API uint32_t ktag_dpdk_capture_get_link_type')]
    actual += source[source.index('KTAG_API ktag_status\nktag_dpdk_capture_get_stats'):source.index('KTAG_API const char *ktag_dpdk_capture_get_last_error')]
    fixture = Path(__file__).with_suffix('.cpp').read_text(encoding='utf-8')
    generated = build / 'rx_workers_check.cpp'
    generated.write_text(fixture.replace('// INSERT_ACTUAL_READERS', actual), encoding='utf-8')
    executable = build / ('rx_workers_check.exe' if os.name == 'nt' else 'rx_workers_check')
    command = [str(Path(args.zig).resolve()), 'c++'] if args.zig else [args.cxx]
    command += ['-std=c++17', '-O1', '-UNDEBUG', '-DKTAG_BUILDING_DLL', '-I' + str(root / 'include')]
    if os.name != 'nt':
        command += ['-pthread']
    command += [str(generated), '-o', str(executable)]
    environment = os.environ.copy()
    environment.setdefault('ZIG_GLOBAL_CACHE_DIR', str(build / 'cache'))
    subprocess.run(command, check=True, cwd=root, env=environment, timeout=180)
    result = subprocess.run([str(executable)], cwd=root, timeout=60, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    (build / 'rx_workers_check.log').write_text(result.stdout, encoding='utf-8')
    print(result.stdout)
    result.check_returncode()


if __name__ == '__main__':
    main()
