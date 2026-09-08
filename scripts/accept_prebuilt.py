"""Verify and consume a relocated archive without SDK sources or a vcpkg installation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, timeout=180, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bundle', required=True, type=Path)
    parser.add_argument('--server', type=Path)
    args = parser.parse_args()
    bundle = args.bundle.resolve()
    info = json.loads((bundle / 'BUILD_INFO.json').read_text())
    ledger = json.loads((bundle / 'FILES.sha256.json').read_text())
    for filename, digest in ledger.items():
        path = (bundle / filename).resolve()
        if not path.is_relative_to(bundle) or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError('Payload checksum mismatch: ' + filename)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('VCPKG_', 'CMAKE_PREFIX_', 'OPENSSL_', 'BOOST_'))}
    with tempfile.TemporaryDirectory(prefix='wukong independent consumer ') as directory:
        workspace = Path(directory)
        shutil.copytree(bundle / 'example', workspace / 'app')
        shutil.copytree(bundle / 'tests', workspace / 'qa')
        # Use an unrelated path containing spaces to expose hard-coded build-machine paths.
        for config in ('Debug', 'Release'):
            for project in ('app', 'qa'):
                build = workspace / (project + '-' + config)
                command = ['cmake', '-S', workspace / project, '-B', build,
                    '-DCMAKE_TOOLCHAIN_FILE=' + str(bundle / 'wukong-sdk.cmake'),
                    '-DCMAKE_BUILD_TYPE=' + config,
                    '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF']
                if sys.platform == 'win32':
                    command += ['-A', 'x64']
                run(*command, env=env)
                run('cmake', '--build', build, '--config', config, '--parallel', '2', env=env)
                cache = (build / 'CMakeCache.txt').read_text()
                for dependency in ('WuKongEasySDK_DIR', 'nlohmann_json_DIR', 'Boost_DIR'):
                    value = next(line.split('=', 1)[1] for line in cache.splitlines()
                                 if line.startswith(dependency + ':'))
                    if not Path(value).resolve().is_relative_to(bundle):
                        raise ValueError(dependency + ' escaped the prebuilt archive')
                if project == 'app':
                    run('ctest', '--test-dir', build, '-C', config, '--output-on-failure', env=env)
                else:
                    binary_dir = build / config if sys.platform == 'win32' else build
                    suffix = '.exe' if sys.platform == 'win32' else ''
                    run(sys.executable, workspace / 'qa/integration.py', '--client',
                        binary_dir / ('wukong_integration_client' + suffix), env=env)
                    if args.server:
                        run(sys.executable, workspace / 'qa/product.py', '--server', args.server.resolve(),
                            '--client', binary_dir / ('wukong_product_smoke' + suffix), env=env)
        print('Relocated prebuilt acceptance passed: ' + info['triplet'] + ', Debug + Release', flush=True)


if __name__ == '__main__':
    main()
