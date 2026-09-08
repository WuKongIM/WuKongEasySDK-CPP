"""Build a relocatable SDK from the immutable public vcpkg registry receipt."""
import argparse
from datetime import date
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PLATFORMS = {
    'x64-linux': ('linux-x64-gcc13', 'Ubuntu 24.04, GCC 13, libstdc++ (C++11 ABI), glibc 2.39+'),
    'arm64-osx': ('macos-arm64-appleclang', 'macOS 14+, Apple Clang, libc++, arm64'),
    'x64-windows': ('windows-x64-msvc143-md', 'Windows x64, Visual Studio 2022 v143, /MD Release and /MDd Debug'),
}


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, timeout=2400, **kwargs)


def release_notes(tag=None):
    version = re.search(r'project\(WuKongEasySDK VERSION (\d+\.\d+\.\d+)',
                        (ROOT / 'CMakeLists.txt').read_text())[1]
    if tag and tag != 'v' + version:
        raise ValueError('Tag must match the exact CMake project version')
    changelog = (ROOT / 'CHANGELOG.md').read_text()
    headings = re.findall(r'^## \[([^\]]+)\] - (\d{4}-\d{2}-\d{2})$', changelog, re.M)
    if (sum(item[0] == version for item in headings) != 1
            or len(re.findall(r'^## .*' + re.escape(version) + r'.*$', changelog, re.M)) != 1):
        raise ValueError('Release requires exactly one dated version section')
    date.fromisoformat(next(day for number, day in headings if number == version))
    section = re.search(r'^## \[' + re.escape(version) + r'\] - \d{4}-\d{2}-\d{2}\n(.*?)(?=^## |\Z)',
                        changelog, re.M | re.S)[1].strip()
    if not re.search(r'^- \S', section, re.M):
        raise ValueError('Release section must contain user-facing notes')
    return version, section


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--triplet', choices=PLATFORMS, required=True)
    parser.add_argument('--tag')
    parser.add_argument('--installed', type=Path, help='Reuse a matching manifest installation for local verification')
    args = parser.parse_args()
    version, notes = release_notes(args.tag)
    vcpkg = Path(os.environ['VCPKG_ROOT']).resolve()
    configuration = json.loads((ROOT / 'examples/vcpkg-consumer/vcpkg-configuration.json').read_text())
    baseline = configuration['default-registry']['baseline']
    actual = subprocess.check_output(['git', '-C', str(vcpkg), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != baseline:
        raise ValueError('Use the pinned vcpkg tool revision: ' + baseline)
    registry = configuration['registries'][0]['baseline']
    indexed = json.loads(subprocess.check_output(['git', 'show',
        registry + ':versions/w-/wukong-easy-sdk.json'], cwd=ROOT, text=True))['versions'][0]
    port_tree = subprocess.check_output(['git', 'rev-parse', 'HEAD:ports/wukong-easy-sdk'],
                                        cwd=ROOT, text=True).strip()
    if indexed['git-tree'] != port_tree:
        raise ValueError('Registry baseline does not index the candidate SDK port')
    work = ROOT / 'build/prebuilt'
    manifest = work / 'manifest'
    manifest.mkdir(parents=True, exist_ok=True)
    for filename in ('vcpkg.json', 'vcpkg-configuration.json'):
        shutil.copy2(ROOT / 'examples/vcpkg-consumer' / filename, manifest / filename)
    installed = args.installed.resolve() if args.installed else work / 'installed'
    executable = vcpkg / ('vcpkg.exe' if os.name == 'nt' else 'vcpkg')
    run(executable, 'install', '--triplet=' + args.triplet, '--x-install-root=' + str(installed), cwd=manifest)
    status = (installed / 'vcpkg/status').read_text()
    if not re.search(r'Package: wukong-easy-sdk\nVersion: ' + re.escape(version) + r'\n', status):
        raise ValueError('Installed SDK version differs from the release')
    (manifest / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(CompilerReceipt LANGUAGES CXX)
file(WRITE "${CMAKE_BINARY_DIR}/compiler.txt"
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\\n${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION} ${CMAKE_SYSTEM_PROCESSOR}\\n")
''')
    receipt = work / 'compiler-receipt'
    command = ['cmake', '-S', str(manifest), '-B', str(receipt)]
    if os.name == 'nt':
        command += ['-A', 'x64']
    run(*command)
    compiler = (receipt / 'compiler.txt').read_text().strip()
    identifier, compatibility = PLATFORMS[args.triplet]
    name = f'WuKongEasySDK-CPP-{version}-{identifier}'
    output = ROOT / 'build/dist'
    output.mkdir(parents=True, exist_ok=True)
    bundle = output / name
    if bundle.exists():
        raise ValueError('Export directory already exists; use a fresh build directory')
    run(executable, 'export', '--raw', '--dereference-symlinks', '--output=' + name,
        '--output-dir=' + str(output), '--x-install-root=' + str(installed), cwd=manifest)
    (bundle / 'wukong-sdk.cmake').write_text(f'''# Relocatable, offline integration for this prebuilt archive.
set(VCPKG_TARGET_TRIPLET "{args.triplet}" CACHE STRING "Prebuilt SDK architecture" FORCE)
set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "Use only the exported packages" FORCE)
include("${{CMAKE_CURRENT_LIST_DIR}}/scripts/buildsystems/vcpkg.cmake")
''')
    shutil.copytree(ROOT / 'examples/prebuilt-consumer', bundle / 'example')
    shutil.copy2(ROOT / 'examples/chat.cpp', bundle / 'example/chat.cpp')
    qa = bundle / 'tests'
    qa.mkdir()
    for filename in ('integration_client.cpp', 'integration.py', 'product_smoke.cpp', 'product.py', 'requirements.txt'):
        shutil.copy2(ROOT / 'tests' / filename, qa / filename)
    shutil.copy2(ROOT / 'scripts/accept_prebuilt.py', qa / 'accept_prebuilt.py')
    (qa / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(WuKongPrebuiltAcceptance LANGUAGES CXX)
find_package(WuKongEasySDK 0.1 CONFIG REQUIRED)
find_package(Threads REQUIRED)
foreach(name IN ITEMS integration_client product_smoke)
    add_executable(wukong_${name} ${name}.cpp)
    target_link_libraries(wukong_${name} PRIVATE WuKongEasySDK::WuKongEasySDK Threads::Threads)
endforeach()
''')
    for filename in ('LICENSE', 'CHANGELOG.md'):
        shutil.copy2(ROOT / filename, bundle / filename)
    shutil.copy2(ROOT / 'docs/PREBUILT.md', bundle / 'README.md')
    # Preserve export-tool licensing as well as every port's copyright/SPDX files.
    shutil.copy2(vcpkg / 'LICENSE.txt', bundle / 'VCPKG-LICENSE.txt')
    shutil.copy2(vcpkg / 'NOTICE.txt', bundle / 'VCPKG-NOTICE.txt')
    port = (ROOT / 'ports/wukong-easy-sdk/portfile.cmake').read_text()
    metadata = {
        'version': version, 'triplet': args.triplet, 'compatibility': compatibility,
        'compiler_environment': compiler,
        'packaging_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'sdk_source_commit': re.search(r'REF ([0-9a-f]{40})', port)[1],
        'sdk_source_sha512': re.search(r'SHA512 ([0-9a-f]{128})', port)[1],
        'registry_baseline': configuration['registries'][0]['baseline'], 'vcpkg_baseline': baseline,
        'configurations': ['Debug', 'Release'], 'sdk_linkage': 'static',
    }
    (bundle / 'BUILD_INFO.json').write_text(json.dumps(metadata, indent=2) + '\n')
    # A payload ledger permits checking the installed content after relocating the archive.
    checksums = {str(path.relative_to(bundle)).replace('\\', '/'): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in sorted(bundle.rglob('*')) if path.is_file()}
    (bundle / 'FILES.sha256.json').write_text(json.dumps(checksums, indent=2) + '\n')
    archive = Path(shutil.make_archive(str(output / name), 'zip', root_dir=output, base_dir=name))
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output / (name + '.sha256')).write_text(digest + '  ' + archive.name + '\n')
    (output / 'RELEASE_NOTES.md').write_text(notes + '\n\nSee the bundled README for compiler/ABI requirements, '
        'WSS CA configuration and Windows runtime deployment. SHA256SUMS covers all three archives.\n')
    print('Created ' + str(archive), flush=True)


if __name__ == '__main__':
    main()
