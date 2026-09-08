# Released C++ SDK three-node WSS acceptance

This harness consumes the public **v0.1.0 archive**, verifies its fixed SHA256 and
internal file ledger, and builds an independent application with
`find_package(WuKongEasySDK)`. It never compiles C++ SDK implementation sources.
Debug and Release run separately against fresh three-node clusters.

## Exact inputs

| Input | Identity |
| --- | --- |
| Public release | [v0.1.0](https://github.com/WuKongIM/WuKongEasySDK-CPP/releases/tag/v0.1.0) |
| Binary SDK source | `3e367a908f42385ab9306f9708b7456399cace7d` |
| Archive packaging commit | `88c855db0e707a15e56b569a0c9ac937907ebdf8` |
| Linux x64 archive SHA256 | `bda2cddfa84ace5a6c864259ff749a538bb10e2bef6640da574f8d5cf582491b` |
| macOS arm64 archive SHA256 | `b7508bdf9ba5235ee04d197af418fd0d12f6784ea173359fa985035e47e69bd8` |
| Product server | `5f5003778ccee6786591ed9968a5185e9213ea55` |
| Reference JS SDK 2.0.4 | `9c03c98c725982fac224cd1d3b52456eae983975` |

Server binaries must carry the exact clean Go VCS stamp; the reference JS
checkout must also be clean and at the pinned revision. The receipt records
archive/file identities, server and JS hashes, harness commit/file hashes and
working-tree state. A receipt with `harness_dirty: true` is development evidence
only. A source-only run cannot replace this archive acceptance.

## What is checked

Each cluster has 256 hash slots, 12 logical Slots, three Slot replicas and Token
authentication enabled. C++ Alice, JS Bob and C++ Carol connect through separate
WSS endpoints to nodes 1, 2 and 3. CONNECT results prove those actual ingress
node IDs. Temporary TLS proxies terminate WSS with certificate and hostname
verification enabled and forward opaque bytes to the product WebSocket Gateway.
They never synthesize CONNECT, SENDACK, messages or other protocol responses.

1. All six peer directions exchange Unicode/2 KB payloads. Each successful
   SENDACK's message ID and sequence must match the receiving client's event.
2. Withhold Alice's downstream bytes after a SEND reaches Carol. The request
   must fail with `ErrorCode::Timeout` at the configured 1.2-second deadline.
3. Repeat with an active connection cut: the pending SEND must fail with
   `ErrorCode::Transport`. A one-second endpoint outage exercises bounded
   reconnect. Both faults require restored messaging and one observed delivery
   of the ambiguous SEND during the bounded post-reconnect observation window.
4. Kill only node 1's owned process. The remaining nodes must agree on live
   elected Slot leaders with quorum and continue JS/C++ traffic in both
   directions. Restart node 1 using its same data directory, require all-node
   Slot convergence and Alice's automatic reconnection to node 1, then repeat
   all six message directions.
5. Destroy each client and require its process to exit. The public
   `/user/onlinestatus` endpoint first proves three active routes and eventually
   returns an empty array after shutdown: this legacy API omits offline users.
   The harness stops all owned proxies/nodes and deletes temporary Tokens,
   certificates and data. Reports contain no Tokens, keys or server logs.

This is bounded correctness evidence, not a capacity or long-duration soak
result. It does not establish multi-host network-partition behavior, production
CA/proxy configuration, Windows product-server operation, automatic migration
to another Gateway URL, offline history synchronization, or exactly-once
delivery. A timed-out SEND may already have been committed and delivered; the
SDK does not queue or replay it automatically. Applications must reconcile
uncertain outcomes using their message identity and backend history policy.

## Recorded acceptance (2026-09-08)

The [macOS arm64 receipt](CLUSTER_RECEIPT_MACOS.json) records a clean
`8f014bcb831e1f0dc1716d834028526d652e82fd` harness run against the public archive.
Both Debug and Release passed all stages, each with 20 message attempts, three
initial active routes and zero routes after shutdown. The two ambiguous SENDs
per configuration each produced one observed delivery; all successful SENDACKs
matched their receiving events. Recovery durations are observations from this
one-host run, not latency guarantees. Hosted Linux/macOS evidence is reported
by the exact commit's `Released SDK three-node WSS acceptance` workflow.

## Reproduce

Use Python 3.12, Node.js 22.15, Go 1.25.11, CMake 3.20+, Ninja, the OpenSSL CLI,
and the compatible compiler described in [PREBUILT.md](PREBUILT.md). No Python
packages or vcpkg installation are needed by this harness. The C++ SDK and its
dependencies come exclusively from the archive.

Clone the server and JS reference outside any other Git checkout. Check out
the exact revisions above, build the server with
`GOWORK=off go build -buildvcs=true -trimpath -o /absolute/path/wukongim ./cmd/wukongim`,
then run `npm ci --no-audit --no-fund && npm run build` in the JS checkout.
Some Go installations do not identify `.git` files in linked worktrees, which
can omit the VCS stamp or incorrectly use an outer repository's state; use a
normal clean clone if the provenance gate rejects such a binary.

Download the matching published archive and run from this SDK repository:

```sh
python3 tests/cluster.py \
  --archive /absolute/path/WuKongEasySDK-CPP-0.1.0-macos-arm64-appleclang.zip \
  --server /absolute/path/wukongim \
  --js-entry /absolute/path/WuKongEasySDK-JS/dist/cjs/index.js \
  --report /absolute/path/three-node-receipt.json
```

Use the `linux-x64-gcc13.zip` archive on Ubuntu 24.04 x64. Both Debug and Release
are checked by default; `--configurations Release` selects one configuration
for diagnosis. The scenario has a five-minute total deadline plus bounded
build commands and cleanup. `cluster.yml` runs the same test on Linux and
macOS, retaining each successful receipt as a 14-day workflow artifact. Inspect
the exact commit's result before attributing hosted acceptance to a change.
