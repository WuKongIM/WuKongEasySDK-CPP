# WuKongEasySDK-CPP

[中文说明](README_zh.md)

A C++17 communication SDK for WuKongIM, following
[WuKongEasySDK-JS](https://github.com/WuKongIM/WuKongEasySDK-JS) `v2.0.4`
(`9c03c98c725982fac224cd1d3b52456eae983975`). It provides WebSocket JSON-RPC
CONNECT, online messaging, automatic RECVACK, heartbeat, bounded reconnection,
and custom event notifications.

The project version is **0.1.0**. Use the WuKongIM-maintained vcpkg Git registry below or CMake source/install
integration. No prebuilt SDK archive is published.

## Recommended: vcpkg + CMake

Install [vcpkg](https://learn.microsoft.com/en-us/vcpkg/get_started/get-started)
and set `VCPKG_ROOT` to its directory. Keep Git, CMake 3.20+ and a C++17 compiler
available (Visual Studio 2022 on Windows, Xcode command-line tools on macOS,
GCC/Clang on Linux). The tested vcpkg revision is
`04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`.

In your application directory, create `vcpkg.json`:

```json
{"dependencies": ["wukong-easy-sdk"]}
```

`vcpkg-configuration.json`:

```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4"
  },
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/WuKongIM/WuKongEasySDK-CPP.git",
      "baseline": "63ec99d34c7605b64e2173d201639042e0e49de9",
      "packages": [
        "wukong-easy-sdk"
      ]
    }
  ]
}
```

Add the following `CMakeLists.txt` next to your own `main.cpp`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)
find_package(WuKongEasySDK 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE WuKongEasySDK::WuKongEasySDK)
```
```sh
# Linux / macOS
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
```

```powershell
# Windows / Visual Studio 2022
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release --parallel 2
```

vcpkg installs the SDK, Boost, OpenSSL and JSON automatically. The first build
may compile dependencies and take several minutes; this is not a prebuilt
archive. The SDK port is a static library; on Windows, distribute dependency
DLLs copied beside the application when using `x64-windows`.

This public Git registry is maintained by WuKongIM in this repository. It is
not Microsoft's curated catalog: copy the registry configuration as well as
the dependency manifest. SDK source is pinned to
`3e367a908f42385ab9306f9708b7456399cace7d`, independently of the registry baseline.
Commit both JSON files to reproduce dependency selection. Existing projects
should merge these entries into their manifests instead of overwriting them.

[Independent consumer example](examples/vcpkg-consumer) · [Registry maintenance](docs/VCPKG.md)

## Alternative: build from source

Requirements: C++17, CMake 3.20+, Boost 1.74+, OpenSSL 1.1.1+, and
nlohmann/json 3.11+. Use supported, patched dependency versions in your product.
CMake uses an installed JSON package or fetches the exact upstream 3.11.3 commit.
To require entirely preinstalled dependencies, set `WUKONG_FETCH_JSON=OFF`.

```sh
# macOS
brew install cmake boost openssl@3 nlohmann-json
# Ubuntu / Debian
sudo apt-get install g++ cmake libboost-dev libssl-dev nlohmann-json3-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

Windows (Visual Studio 2022, CMake and vcpkg):

```powershell
git -C "$env:VCPKG_ROOT" fetch origin 04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

The manifest pins the vcpkg baseline. The Windows build links the standard
Winsock libraries. Dynamic vcpkg dependencies must accompany your executable.

## Connect and send

Your trusted application backend supplies the Gateway URL, UID and Token.
Clients must not invoke Product HTTP management endpoints. Production uses WSS.
The default server example accepts `ws://127.0.0.1:5200/`; use `/ws` only when
your listener or reverse proxy is configured for that path.

```cpp
#include <wukong/wkim.hpp>
#include <cstdlib>
#include <iostream>

int main() {
    const char* token = std::getenv("WKIM_TOKEN");
    if (!token) return 2;
    try {
        wukong::WKIM im("ws://127.0.0.1:5200", {"alice", token});
        const auto listener = im.on(wukong::WKIMEvent::Message,
            [](const wukong::Json& message) {
                // Dispatch message.at("payload") to your application's UI/event queue.
                (void)message;
            });
        im.connect().get();
        auto ack = im.send("bob", wukong::WKIMChannelType::Person,
                          {{"type", 1}, {"content", "Hello from C++!"}}).get();
        if (ack.reasonCode == 1) std::cout << "SEND completed\n";
        im.off(listener);
        im.disconnect().get();
        im.destroy().get();
    } catch (const wukong::Error&) {
        std::cerr << "EasySDK operation failed\n";
        return 1;
    }
}
```

`connect()` completes only after successful CONNECT authentication. A SENDACK
means the server accepted the send; it does not prove delivery, display, or
application processing. EasySDK has no local message database, conversations,
unread counts, offline recovery, push, file upload, or subscription API.

For an interactive Alice/Bob test, run the built example in two terminals, each
with its own backend-issued `WKIM_TOKEN`:

```sh
./build/wukong_chat ws://127.0.0.1:5200 alice bob
./build/wukong_chat ws://127.0.0.1:5200 bob alice
```

Both terminals must print `Connected` before sending. Enter `/quit` to clean up.
On Windows the executable is `build/Release/wukong_chat.exe`. The example
intentionally renders incoming message content; the SDK itself emits no logs.

## Integrate with CMake

As a source subdirectory:

```cmake
add_subdirectory(external/WuKongEasySDK-CPP)
target_link_libraries(my_app PRIVATE WuKongEasySDK::WuKongEasySDK)
```

Or install and consume the exported package:

```sh
cmake --install build --config Release --prefix /path/to/sdk-prefix
cmake -S /path/to/my_app -B /path/to/my_app/build -DCMAKE_PREFIX_PATH=/path/to/sdk-prefix
```

```cmake
find_package(WuKongEasySDK 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE WuKongEasySDK::WuKongEasySDK)
```

The exported static library preserves its Boost, OpenSSL, Threads and JSON
dependencies. Build examples and tests are disabled by default when used as a
subdirectory. See [the downstream consumer](tests/consumer).

## API and lifecycle

| API | Behavior |
| --- | --- |
| `WKIM(url, AuthOptions, Options)` / `WKIM::init(...)` | One independent identity, one I/O thread; no global singleton |
| `connect()` → `future<ConnectResult>` | Concurrent calls join the same attempt; connected calls return the current result |
| `send(channel, ChannelType, Json, SendOptions)` → `future<SendResult>` | JSON object/array → UTF-8 Base64 SEND; no offline queue or automatic resend |
| `on(Event, callback)` → `ListenerId` / `off(id)` | Token-based listener ownership |
| `disconnect()` → `future<void>` | Cancel transport, pending requests, heartbeat and reconnect; reusable afterwards |
| `destroy()` → `future<void>` | Terminal shutdown; later operations reject; repeated calls are safe |
| `state()` / `isConnected()` | Thread-safe state snapshots |
| Destructor | Shuts down and joins the worker; callback-owned destruction safely defers worker exit |

All public operations can be called from application threads. Callbacks execute
serially on the SDK's I/O thread and may enqueue SDK operations. **Never call
`.get()`/`.wait()` on an SDK future from a callback.** Dispatch slow work and UI
updates to your own executor. A blocking callback also delays heartbeat,
timeouts and shutdown. `off()` prevents future dispatches; a callback already
copied for dispatch can finish. Captured application state must outlive client
shutdown. Callback exceptions are contained. Avoid ownership cycles by capturing
`weak_ptr` instead of a owning `shared_ptr` to the same client.

`disconnect()` and `destroy()` cancel the socket directly, so cleanup does not
wait for a peer's WebSocket close handshake. Destruction from a callback cannot
join its own thread; that thread exits after the current callback and queued
cleanup complete. Create a new client to change identities, tokens or URL.

| Event | JSON data |
| --- | --- |
| `Connect` | CONNECT result, including `reasonCode` |
| `Message` | `header`, string `messageId`, integer `messageSeq`, `timestamp` (seconds), `channelId`, `channelType`, `fromUid`, decoded `payload` |
| `SendAck` | string `messageId`, `messageSeq`, `reasonCode` |
| `CustomEvent` | `id`, `type`, `timestamp` (milliseconds), `data`; JSON-string data is parsed |
| `Reconnecting` | `attempt` (1-based), `delay` (milliseconds) |
| `Disconnect` | Sanitized disconnect information |
| `Error` | Numeric `code`, sanitized `message` |

Object payloads are preserved. JSON text and Base64-encoded JSON payloads are
decoded; unknown opaque strings remain strings. Message IDs never pass through
floating point. Automatic RECVACK includes both identifiers and the received
header, and is queued before application delivery. This is a transport receipt,
not a business read receipt. Unknown notifications are ignored; malformed
protocol input closes the session and reports a sanitized error.

## Bounds, reconnect and TLS

See [`Options`, `Header` and `SendOptions`](include/wukong/wkim.hpp) for complete
fields and defaults. Device flags are APP `0`, WEB `1`, PC/Desktop `2` (default).
An empty `deviceId` gets a UUID retained across reconnects. `clientMsgNo` defaults
to a new UUID; supply your own stable value for application reconciliation.
`Header::redDot` defaults to true and respects an explicit false value.

Defaults: 10-second DNS/TCP/TLS/Upgrade/CONNECT deadline, 15-second request
limit, 25-second ping interval and 10-second pong limit. A correlated
`result: null` satisfies the heartbeat; an unrelated response or `pong`
notification does not. After a previously authenticated transport is lost,
retry at most five times with capped exponential delay (1–30 seconds) and
half-to-full jitter. Authentication rejection, server-requested disconnect,
malformed protocol and manual shutdown stop retries. Initial connect failure
is returned to the caller. Successful reauthentication resets the retry budget.

Admission is bounded to 1,024 requests, including commands waiting for the
worker. Queued commands and WebSocket writes each have a 4 MiB byte budget;
individual wire messages are limited to 1 MiB with a 64-level inbound JSON nesting cap. One reserved control slot allows
disconnect under request saturation. Capacity failures return `QueueFull`.
Timeouts and lost connections can leave send delivery **unknown**; reconcile
using `clientMsgNo` before deciding whether to retry. The SDK never silently
resends a pending message.

WSS verifies the chain and hostname, uses system OpenSSL trust roots, and
requires TLS 1.2+. `Options::caFile` optionally adds a PEM CA bundle. There is no
insecure verification switch. The console also reads `WKIM_CA_FILE`. The SDK
never logs Tokens, payloads, frames, URLs, server response text or transport
error objects. Positive server reason codes are retained in `Error::code()`;
local errors use the negative `ErrorCode` values.

## Validation

```sh
python3 -m venv .venv
.venv/bin/pip install -r tests/requirements.txt
cmake -S . -B build -DWUKONG_INTEGRATION_TESTS=ON -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
# Optional compiler memory/undefined-behavior instrumentation
cmake -S . -B build-asan -DWUKONG_SANITIZE=ON
```

The integration suite uses real loopback WebSocket sockets and temporary WSS
certificates. For real-product and real-JS interoperability, build a WuKongIM
binary and the pinned JS SDK, then run:

```sh
python3 tests/product.py --server /path/to/wukongim --client build/tests/wukong_product_smoke \
  --js-entry /path/to/WuKongEasySDK-JS/dist/cjs/index.js
```

The script owns a temporary single-node cluster with 256 hash slots, enables
Token authentication, provisions test credentials through loopback Product
HTTP, verifies C++/C++ exchange, reconnect, invalid-token rejection and presence
cleanup, optionally runs C++/JS exchange, then stops only its own processes.
These administration operations belong to the test harness, never to SDK code.
See [validation evidence](docs/VALIDATION.md) for exact scope and revisions.

## License

MIT; see [LICENSE](LICENSE). Dependency licenses remain their respective owners'.
