# vcpkg package distribution

This repository is both SDK source and a public, WuKongIM-maintained Git registry.
It is not part of Microsoft's curated vcpkg catalog. No prebuilt SDK archive or
Microsoft catalog acceptance is implied by publishing these registry files.

## Published identities

- Port: `wukong-easy-sdk`, version `0.1.0`, port-version `0`.
- SDK source: `3e367a908f42385ab9306f9708b7456399cace7d` (reviewed runtime).
- SDK archive SHA-512 is recorded in `ports/wukong-easy-sdk/portfile.cmake`.
- Registry baseline: `63ec99d34c7605b64e2173d201639042e0e49de9`.
- Dependency registry and tested vcpkg tool: `04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`.

The package installs a static SDK with exported target
`WuKongEasySDK::WuKongEasySDK`. Dependencies are built/linked according to the
selected triplet; `ONLY_STATIC_LIBRARY` does not force dependencies to static.
Debug and Release import locations are fixed up by vcpkg. Examples and tests
are excluded from installation, JSON FetchContent is disabled, and dependencies
must resolve through vcpkg. Headers, license, CMake config and usage are included.

Supported acceptance triplets are x64-linux, arm64-osx and x64-windows. Other
triplets require their own validation. Mobile, UWP and Emscripten are not claimed.
The consumer declares no direct Boost/OpenSSL/JSON dependency and calls compiled
SDK lifecycle code; this verifies linking and runtime dependency availability.

## Updating the registry

1. Publish a tested, immutable SDK source commit and compute its archive SHA-512.
2. Update the port source/hash and manifest version (or increment port-version
   for packaging-only fixes). Never replace an existing version's git-tree.
3. Commit the port, then obtain `git rev-parse HEAD:ports/wukong-easy-sdk`.
4. Add that tree to `versions/w-/wukong-easy-sdk.json`; advance
   `versions/baseline.json` in a second commit. Old version records remain.
5. Pin that second commit in consumer configuration and documentation. Publish
   it before running the remote registry consumer check.
6. Run `tests/check_vcpkg.py` with `VCPKG_ROOT` set for each supported triplet.
   The check rejects a consumer baseline indexing a different candidate port.

See Microsoft's [Git registry documentation](https://learn.microsoft.com/en-us/vcpkg/maintainers/registries).

## Validation record (2026-09-08)

Before publication, an independent macOS arm64 application resolved the exact
port tree through a local Git registry with the published baseline, downloaded
the checksum-pinned GitHub SDK archive, and installed all transitive dependencies
without manual Boost/OpenSSL/JSON installation. vcpkg post-build validation passed.
Both Debug and Release consumers compiled, linked and executed client shutdown.
This run used AppleClang 16, Boost 1.92.0, OpenSSL 3.6.4 and nlohmann/json 3.12.0.

The GitHub Actions matrix additionally runs the same consumer from the public
registry URL for the exact candidate port tree. Inspect that commit's CI result
for Linux/macOS/Windows evidence; source-only CI from earlier commits does not
validate this distribution path.
