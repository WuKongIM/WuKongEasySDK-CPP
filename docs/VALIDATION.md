# Initial C++ SDK validation

Date: 2026-09-08. This record describes source in this Git tree, project version
0.1.0. It is not a receipt for a published binary or package-registry artifact.

## Reference revisions

- WuKongEasySDK-JS v2.0.4: `9c03c98c725982fac224cd1d3b52456eae983975`.
- WuKongIM server: `132e46209d98fa0425cc0f88e7a97080cdad044d`.
- Fetched nlohmann/json 3.11.3: `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03`.
- Windows dependency baseline: `04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4` (vcpkg).

## Locally executed checks

- macOS arm64, AppleClang 16.0.0, CMake 4.4.3, Boost 1.92.0,
  OpenSSL 3.6.3, nlohmann/json 3.11.3: Debug build, protocol unit tests,
  and 23 real WS/WSS integration scenarios passed.
- AddressSanitizer and UndefinedBehaviorSanitizer build: the same unit and
  WS/WSS suite passed without sanitizer reports.
- Ubuntu 24.04 amd64 container, GCC 13.3.0, OpenSSL 3.0.13, system Boost
  and JSON: Release build, unit/WS/WSS tests, install and downstream consumer passed.
- `cmake --install`, a separate `find_package(WuKongEasySDK)` consumer build,
  and the consumer executable passed.
- A separately built real WuKongIM process with 256 hash slots and
  `gateway.token_auth_on=true` passed C++/C++ Unicode bidirectional messaging,
  exact SENDACK/RECV identifiers, reconnect, wrong-token rejection, and public
  `/user/onlinestatus` disconnect cleanup.
- The real JS SDK built from the pinned source above then exchanged a Unicode
  JSON payload in both directions with the C++ client on the same server.
  Temporary credentials were provisioned by the loopback test harness; the
  SDK never called Product HTTP. The harness stopped its own processes and
  removed temporary storage after completion.

## Reproduction

See README build commands and `tests/requirements.txt`. Run:

```sh
ctest --test-dir build --output-on-failure
python3 tests/product.py --server /path/to/wukongim --client build/tests/wukong_product_smoke \
  --js-entry /path/to/WuKongEasySDK-JS/dist/cjs/index.js
```

`tests/integration.py` verifies CONNECT rejection/timeout/cancellation,
SEND rejection/timeout, Unicode and large IDs, duplicate/unknown response IDs,
RECVACK fields, custom events, correlated null heartbeat, uncorrelated pong
rejection, reconnect exhaustion/cancellation, server disconnect, malformed and
oversize messages, bounded request admission, listener cleanup and exceptions,
callback-owned destruction, HTTP Upgrade timeout, trusted WSS, untrusted WSS,
and certificate hostname mismatch. All subprocesses and waits have deadlines.

The read-only GitHub Actions matrix builds Linux/macOS/Windows and the installed
consumer; Unix jobs also run WS/WSS integration. Hosted results are available
in this repository's Actions tab and must be checked for the exact commit.

These checks do not establish physical-device behavior, production CA/proxy
configuration, offline recovery, capacity, token expiry policy, or a packaged
release. WSS evidence comes from local test certificates, not production endpoints.

## Lifecycle review follow-up (2026-09-08)

The initial implementation at `8a3218cd5c9465f06df371f040a3641188bd4900`
was reviewed against the pinned JS implementation and documented lifecycle contract.
Before correction, new real-socket tests reproduced missing disconnect on malformed
SENDACK and unwanted reconnect on oversized/invalid WebSocket frames. A separate
bounded executable also reproduced deadlocks when a listener capture's destructor
called `off()` during both explicit listener removal and terminal shutdown.

After correction, macOS arm64 ASan/UBSan unit tests and 26 WS/WSS scenarios passed.
Regression coverage includes both reentrant capture-cleanup paths, malformed
SENDACK, valid rejected SENDACK, oversized messages with automatic reconnect
still enabled, and invalid WebSocket opcodes. Callback-owned destruction now
waits on terminal shutdown before releasing captured application state.
Real C++/C++ and C++/JS product acceptance was rerun against the same pinned
server and JS revisions above. The GitHub Actions matrix must still be checked
for this follow-up commit; initial hosted results do not certify a newer tree.
