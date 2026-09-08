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
