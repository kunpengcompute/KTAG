"""Check the real UB bytes example using an in-memory transport (no SDK needed).

  python test/common/ub_bytes_example_check.py --cxx c++
  python test/common/ub_bytes_example_check.py --zig /path/to/zig

This checks protocol, file bytes and ownership; real two-node SDK validation is
still required. Sources/binaries/logs stay under build/ub-bytes-check.
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
    build = root / 'build/ub-bytes-check'
    build.mkdir(parents=True, exist_ok=True)
    executable = build / ('ub_bytes_check.exe' if os.name == 'nt' else 'ub_bytes_check')
    command = [str(Path(args.zig).resolve()), 'c++'] if args.zig else [args.cxx]
    command += ['-std=c++17', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror']
    if os.name != 'nt':
        command += ['-pthread']
    command += ['-I' + str(root / 'tools/KFlowLens/pipeline'),
                str(Path(__file__).with_suffix('.cpp')), '-o', str(executable)]
    env = os.environ.copy()
    env.setdefault('ZIG_GLOBAL_CACHE_DIR', str(build / 'cache'))
    subprocess.run(command, check=True, cwd=root, env=env, timeout=180)
    result = subprocess.run([str(executable)], cwd=build, timeout=30,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding='utf-8', errors='replace')
    log = build / 'ub_bytes_check.log'
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
