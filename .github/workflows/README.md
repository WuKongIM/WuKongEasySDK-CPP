# SDK validation

`ci.yml` builds the C++17 SDK and tests on Linux, macOS and Windows. It only
reads repository contents. Linux and macOS additionally run the bounded local
WS/WSS fixture; it generates temporary test certificates and deletes them.
No release, deployment, external messaging, package publication, or paid resource
creation is performed. Dependencies use the reviewed vcpkg baseline on Windows
and pinned JSON source when no system package is available. Actions are pinned
by full commit. Failed runs may be retried after inspecting the exact failure.

The same matrix also consumes `examples/vcpkg-consumer` as an independent
application through the public WuKongIM Git registry, checks that its pinned
baseline indexes the candidate port tree, and builds/runs Debug and Release.
It uses arm64-osx, x64-linux and x64-windows with the pinned vcpkg revision;
Unix jobs bootstrap their own temporary vcpkg checkout. The package manager
builds dependencies from source or uses its normal binary cache. This is
package-consumption verification, not publication to Microsoft's curated
registry or distribution of prebuilt SDK archives.

## Prebuilt SDK archives

`release.yml` builds exports from the exact public SDK registry and dependency
baseline, uploads three preview ZIPs, then consumes them in fresh jobs without a
C++ SDK checkout or vcpkg installation. Debug/Release lifecycle and 26 WS/WSS
scenarios run on all three platforms. Unix jobs additionally compile the pinned
product server and exercise a temporary token-authenticated 256-hash-slot
single-node cluster. These jobs use read-only permissions and bounded timeouts;
fixtures only contact loopback. Packaging-branch pushes, pull requests and manual runs produce previews only.
Source CI runs on branches/PRs; version tags run the archive pipeline, avoiding
duplicate source builds for the same release.

A `v*` tag push is the publication trigger. The tag must equal the CMake version,
and a unique nonempty dated Changelog section must pass before artifact upload.
Only after every archive passes independent acceptance may the isolated publish
job obtain `contents: write`; it downloads the exact artifacts and checks all three
archive names/hashes before creating the GitHub Release. It never executes SDK
source. Existing release assets must not be overwritten. For failed previews,
inspect the failing job before retrying; for a publication failure inspect the
existing Release/draft and assets before resuming. User authorization to ship an
SDK version covers its tag/Release, not a WuKongIM server/native-package release.
No paid resources, external user messages, signing identity or scheduled workers
are involved. See `docs/PREBUILT.md` for compatibility and upgrade boundaries.

`prebuilt-diagnostics.yml` is a temporary, manual, read-only Windows fixture probe.
It downloads the exact failed preview run 34192260675, compares three localhost
and explicit-IPv4 exchanges with the same binary and unchanged 400 ms test timeout,
and measures refused IPv6 versus reachable IPv4 loopback connections. It creates
no releases or external connections beyond dependency/artifact downloads.
