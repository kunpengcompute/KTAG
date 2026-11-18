"""Run the real traffic Provider, Schema, DFA and statistics contract tests.

Requires a compiler and a GoogleTest source tree; does not download dependencies.
Example:
  python test/common/traffic_plugin_contract_check.py --zig /path/to/zig \
      --gtest test/googletest-release-1.8.0 --yaml-cpp /path/to/yaml-cpp
"""
import argparse
import os
from pathlib import Path
import subprocess
import shutil
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--zig')
    parser.add_argument('--gtest', required=True, type=Path)
    parser.add_argument('--yaml-cpp', required=True, type=Path)
    parser.add_argument('--pcap-include', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = root / 'build/traffic-contract-check'
    build.mkdir(parents=True, exist_ok=True)
    gtest = args.gtest.resolve() / 'googletest'
    yaml_cpp = args.yaml_cpp.resolve()
    sources = [
        *sorted((root / 'plugins/traffic_features').rglob('*.cpp')),
        *[root / 'src/feature' / name for name in (
            'feature_schema.cpp', 'traffic_schema.cpp', 'provider_registry.cpp',
            'feature_batch.cpp', 'feature_view.cpp', 'feature_pipeline.cpp',
            'feature_context.cpp', 'dns_observer.cpp', 'domain_store.cpp')],
        root / 'src/core/packet_decode_internal.cpp',
        root / 'src/capture/flow_key.cpp', root / 'src/capture/packet_buffer.cpp',
        root / 'tools/KFlowLens/pipeline/ub_feature_wire.cpp',
        root / 'tools/KFlowLens/pipeline/pipeline_config.cpp',
        *sorted((yaml_cpp / 'src').glob('*.cpp')),
        *[root / 'test/common/functest' / name for name in (
            'func_feature_schema.cpp', 'func_feature_extract.cpp',
            'func_rawbow_token_dfa.cpp', 'func_legacy_plugin_algorithms.cpp',
            'func_traffic_ub_contract.cpp', 'func_integration_config.cpp')],
        gtest / 'src/gtest-all.cc', gtest / 'src/gtest_main.cc',
    ]
    command = [str(Path(args.zig).resolve()), 'c++'] if args.zig else [args.cxx]
    command += ['-std=c++17', '-O1', '-UNDEBUG', '-DKTAG_BUILDING_DLL',
                '-DKTAG_TRAFFIC_FEATURES_BUILDING_DLL', '-DYAML_CPP_STATIC_DEFINE']
    if args.pcap_include:
        command += ['-I' + str(args.pcap_include.resolve())]
    if os.name == 'nt':
        # Only adapt the platform byte-order header; all tested algorithms and
        # Provider/Schema implementations are the production sources above.
        compat = build / 'compat'
        (compat / 'arpa').mkdir(parents=True, exist_ok=True)
        (compat / 'arpa/inet.h').write_text('#include <winsock2.h>\n', encoding='utf-8')
        (compat / 'netinet').mkdir(exist_ok=True)
        (compat / 'netinet/in.h').write_text('#include <winsock2.h>\n', encoding='utf-8')
        (compat / 'netinet/tcp.h').write_text(
            '#define TH_FIN 1\n#define TH_RST 4\n#define TH_ACK 16\n', encoding='utf-8')
        command += ['-I' + str(compat), '-lws2_32']
    else:
        command += ['-pthread']
    for include in [root / 'include', root / 'src', root / 'plugins/traffic_features',
                    root / 'tools/KFlowLens/pipeline', yaml_cpp / 'include', yaml_cpp / 'src',
                    gtest / 'include', gtest]:
        command += ['-I' + str(include)]
    executable = build / ('traffic_contract_check.exe' if os.name == 'nt'
                          else 'traffic_contract_check')
    command += [str(source) for source in sources] + ['-o', str(executable)]
    environment = os.environ.copy()
    environment.setdefault('ZIG_GLOBAL_CACHE_DIR', str(build / 'cache'))
    compilation = subprocess.run(command, cwd=root, env=environment, timeout=300,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    diagnostics = compilation.stdout.decode('utf-8', errors='replace')
    (build / 'compile.log').write_text(diagnostics, encoding='utf-8')
    if compilation.returncode:
        print('\n'.join(line for line in diagnostics.splitlines() if 'error:' in line))
        raise SystemExit(compilation.returncode)
    with tempfile.TemporaryDirectory(prefix='ktag-config-check-') as temporary:
        # yaml-cpp uses narrow Windows file APIs. Stage configs under an ASCII
        # temporary root when the repository path contains non-ASCII characters.
        config_root = Path(temporary)
        (config_root / 'tools/KFlowLens').mkdir(parents=True)
        for name in ('config.yaml', 'config-ub-1to1.yaml', 'proxy_config.yaml'):
            shutil.copyfile(root / 'tools/KFlowLens' / name, config_root / 'tools/KFlowLens' / name)
        environment['KTAG_TEST_CONFIG_ROOT'] = str(config_root)
        result = subprocess.run([str(executable)], cwd=root, env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    output = result.stdout.decode('utf-8', errors='replace')
    (build / 'results.log').write_text(output, encoding='utf-8')
    print('\n'.join(line for line in output.splitlines()
                    if '[==========]' in line or '[  PASSED  ]' in line or
                    '[  FAILED  ]' in line or 'error:' in line))
    if result.returncode:
        print(output[-12000:])
    result.check_returncode()


if __name__ == '__main__':
    main()
