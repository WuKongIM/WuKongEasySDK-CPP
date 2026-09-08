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
fixtures only contact loopback. Packaging-branch pushes, pull requests and manual runs with `publish=false`
produce previews only. An explicitly authorized manual `publish=true` run must
use `main`; it creates the version tag only after every acceptance gate passes
and refuses to replace an existing tag.
Source CI runs on branches/PRs; version tags run the archive pipeline, avoiding
duplicate source builds for the same release.

A `v*` tag push is the alternative publication trigger. The tag must equal the CMake version,
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

## Released SDK three-node acceptance

`cluster.yml` downloads the immutable public v0.1.0 Linux/macOS archives and
checks the exact SHA256 and file ledger before compiling independent consumer
code. It never builds C++ SDK implementation sources. The pinned product server
and reference JS SDK run only on loopback with temporary Tokens and a test CA.
Debug and Release each exercise a three-node cluster with 256 hash slots,
12 logical Slots and three Slot replicas: cross-node messaging, withheld-ACK
timeout/transport loss, reconnect without SEND replay, ingress process crash,
surviving-node traffic, same-directory restart, and online-route cleanup.

The workflow is read-only, bounded to 20 minutes, and runs on relevant changes
or explicit manual dispatch. Only its owned loopback connections and processes
are interrupted. It publishes an acceptance receipt, never a package, Release,
credential or server log. It provisions no cloud resources. Inspect the exact
failed stage before retrying. See `docs/CLUSTER_VALIDATION.md` for reproduction
and evidence boundaries.
