# Changelog

## Unreleased

- Add the WuKongIM-maintained vcpkg Git registry for `wukong-easy-sdk` 0.1.0,
  automatic Boost/OpenSSL/JSON dependency installation, and an independent
  CMake consumer with Debug/Release validation on Linux, macOS and Windows.

- Stop sessions without reconnecting on malformed SENDACKs, oversized messages,
  or invalid WebSocket frames; valid server send rejections remain recoverable.
- Avoid deadlocks when listener capture cleanup removes another subscription;
  synchronize callback-owned destruction tests with terminal shutdown.

- Add the initial C++17 SDK: WS/WSS, JSON-RPC CONNECT, SEND/SENDACK,
  automatic RECVACK, heartbeat, capped reconnection and custom events.
- Bound request admission, queued bytes and message size; support explicit
  listener ownership, cancellation and terminal shutdown without logging secrets.
- Add CMake source/install integration, a console chat example, protocol and
  real-socket tests, real WuKongIM/JavaScript interoperability tests, and bilingual documentation.
