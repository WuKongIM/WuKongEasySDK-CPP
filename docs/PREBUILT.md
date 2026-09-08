# Prebuilt C++ SDK 0.1.0

Download the matching ZIP and `SHA256SUMS` from
[Release v0.1.0](https://github.com/WuKongIM/WuKongEasySDK-CPP/releases/tag/v0.1.0).
Verify its SHA-256 before extracting. Each archive contains Debug and Release
SDK libraries, Boost/JSON headers, OpenSSL libraries, dependency licenses/SPDX
receipts under `installed/<triplet>/share`, an example, and bounded acceptance tests.
The SDK is static. OpenSSL is static on Unix and uses bundled DLLs on Windows.
There is no separate vcpkg installation or dependency download during consumption.

| ZIP suffix | Supported consumer environment |
| --- | --- |
| `linux-x64-gcc13` | Ubuntu 24.04 x64, GCC 13, libstdc++ C++11 ABI; glibc 2.39+ |
| `macos-arm64-appleclang` | macOS 14+ arm64, Apple Clang and libc++ |
| `windows-x64-msvc143-md` | Windows x64, Visual Studio 2022 v143, `/MD` Release or `/MDd` Debug |

Names start with `WuKongEasySDK-CPP-0.1.0-` and end in `.zip`.
These are tested build targets, not universal C++ ABI compatibility promises.
Use the source/vcpkg integration for other architectures, older operating systems,
different standard libraries, static MSVC CRT (`/MT`), or incompatible compilers.
Do not mix this archive with a different Boost, JSON or OpenSSL installation.

## Compile the included example

Install only a compatible C++ compiler, platform development tools and CMake 3.20+.
Extract the ZIP, open a terminal in the extracted directory, then run:

```sh
# Linux / macOS
cmake -S example -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/wukong-sdk.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

```powershell
# Windows / Visual Studio 2022
cmake -S example -B build -A x64 -DCMAKE_TOOLCHAIN_FILE="$PWD/wukong-sdk.cmake"
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

`wukong_example` checks linked SDK initialization and terminal shutdown without
credentials or network traffic. `wukong_chat` is the interactive messaging example:
set `WKIM_TOKEN` to a token issued by your backend, then run
`./build/wukong_chat ws://127.0.0.1:5200 alice bob` (Windows:
`./build/Release/wukong_chat.exe`). Run a second client as Bob to exchange messages.
Only use `/ws` when your listener/proxy is configured for that path.

For WSS, supply a maintained CA bundle explicitly through `Options::caFile`
(`WKIM_CA_FILE` in the chat example). The archive does not ship a root certificate
store; do not rely on OpenSSL's build-machine default CA path. Certificate and
hostname validation remain enabled. Distribute and update the trust bundle as
part of your application.

## Integrate your application

Use the same toolchain option with your own source directory. Your CMake only needs:

```cmake
find_package(WuKongEasySDK 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE WuKongEasySDK::WuKongEasySDK)
```

The bundled toolchain selects the archive's triplet and disables vcpkg manifest
installation. Keep the extracted directory intact. Use a new build directory when
switching SDK versions/toolchains. Debug and Release are selected by CMake; Windows
Debug libraries require the matching debug CRT and are for development only.
Ship Release applications with the OpenSSL DLLs copied beside the executable by
CMake and the supported Visual C++ Redistributable. The archive does not redistribute
the Microsoft runtime. Unix applications still require the documented system C/C++
runtimes. Preserve all applicable third-party license notices in your distribution.

## Verify and upgrade

On Linux/macOS use `sha256sum -c SHA256SUMS` / `shasum -a 256 -c SHA256SUMS`
when all three ZIPs are downloaded, or compare the selected ZIP's hash with its line.
On Windows use `Get-FileHash <archive.zip> -Algorithm SHA256` and compare with
`SHA256SUMS`. `BUILD_INFO.json` identifies the packaging commit, SDK source commit,
registry baseline and vcpkg baseline; `FILES.sha256.json` verifies extracted files.

For an upgrade, download the new version to a separate directory, verify hashes,
reconfigure into a fresh build directory, rebuild your application, run messaging
and WSS acceptance, and update the deployed dependencies together. Retain the old
archive/application for rollback. A prebuilt dependency security update requires
a new release; do not silently replace an existing version's assets.

The 0.1.0 binary SDK source is `3e367a908f42385ab9306f9708b7456399cace7d`,
indexed by registry baseline `63ec99d34c7605b64e2173d201639042e0e49de9`.
The release tag additionally contains packaging and acceptance tooling. Registry
and source receipts remain separate from the archive packaging commit.

## Maintainer acceptance

`release.yml` exports the immutable public Git-registry package using the pinned
vcpkg revision. Three fresh jobs download their archive, check its hash, extract
it at a new path and compile independent consumers in Debug and Release.
They do not check out the C++ SDK or install vcpkg. Every platform runs lifecycle
and 26 WS/WSS scenarios for each configuration. Linux/macOS also use the pinned
WuKongIM `132e46209d98fa0425cc0f88e7a97080cdad044d` server to check bidirectional
C++ messaging, SENDACK, reconnect, bad tokens and clean online presence in an
isolated token-authenticated single-node cluster with 256 hash slots. Windows
uses the protocol fixture; this does not claim a Windows WuKongIM server test.

A local repeat requires Python 3.12 and `pip install -r tests/requirements.txt`,
plus the `openssl` CLI for temporary TLS test certificates:

```sh
python tests/accept_prebuilt.py --bundle . --server /path/to/wukongim
```

Omit `--server` to run only the lifecycle and WS/WSS gates. The archive contains
application/test sources, not SDK implementation sources. The build workflow
uploads previews on branch changes, and publishes a versioned GitHub Release only
for a matching version tag after all independent acceptance jobs pass.
