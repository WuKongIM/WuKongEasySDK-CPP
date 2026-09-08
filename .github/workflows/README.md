# SDK validation

`ci.yml` builds the C++17 SDK and tests on Linux, macOS and Windows. It only
reads repository contents. Linux and macOS additionally run the bounded local
WS/WSS fixture; it generates temporary test certificates and deletes them.
No release, deployment, external messaging, package publication, or paid resource
creation is performed. Dependencies use the reviewed vcpkg baseline on Windows
and pinned JSON source when no system package is available. Actions are pinned
by full commit. Failed runs may be retried after inspecting the exact failure.
