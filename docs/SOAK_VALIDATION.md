# Released C++ SDK resource and sustained-load acceptance

This test links an independent consumer against the checksum-pinned public
v0.1.0 archive. [Archive/JS identities](CLUSTER_VALIDATION.md) and all archive
ledger/CMake provenance checks apply. This workload pins product server
`711daef395d8d8d14f4b02886e667f38533aa180`, which fixes starvation of later
Slots/pages in the bounded Channel repair scanner. Earlier server `5f500377`
failed this workload: the affected three-replica Channel retained a dead leader
beyond the 30-second recovery bound. Slot Raft convergence alone was insufficient.
Historical `cluster.py` receipts retain their original server revision. SDK implementation sources
are never compiled. Receipts record the clean/dirty harness state and hashes;
only clean runs qualify as acceptance evidence.

## Workload and checks

Four peers join a Token-authenticated three-node cluster through verified WSS:
C++ Alice on node 1, the real JS 2.0.4 Bob on node 2, and C++ Carol plus a separate
C++ lifecycle peer on node 3. The cluster has 256 hash slots, 12 logical Slots
and three Slot replicas. All processes and TLS proxies are on one host.

After warmup, the driver paces eight concurrent C++ sends and two JS/C++ sends
per second. Eight application threads exercise the C++ public `send` API.
Every successful SENDACK must match the receiver's message ID, sequence, sender
and payload. Control round-trip latency includes driver overhead; burst members
share batch completion time, so these are not SDK-only latency measurements.

A deliberately held message callback tests `maxPendingRequests = 16`. Eight
application threads submit 64 requests before releasing the callback; exactly
16 must succeed and 48 must return `QueueFull`. The driver holds the callback
for at least 250 ms before submitting the burst. This runs before and after the
sustained workload. The callback has a three-second emergency deadline and
never waits on an SDK future. A separate C++ process replaces its SDK object
100 times, preserving the process so thread/descriptor accumulation is visible.

At one-third of the workload, the proxy withholds a delivered message's ACK,
requires the sender's three-second timeout, then cuts its connection for two
seconds. Bob and Carol keep communicating while Alice reconnects. At two-thirds,
node 1 is killed and restarted using its original data directory. Traffic pauses
for elected Slot leader convergence, then surviving peers must communicate
within 30 seconds before the node restarts. Numeric `NodeNotMatch` (18)
rejections during this recovery window are counted separately (at most 32);
each subsequent probe has a new message identity. Rejected identities must
have zero observed deliveries. Up to 16 timeout/transport outcomes in this recovery window are also recorded
individually and reconciled against observed receives, without resending them.
An unobserved receive remains unconfirmed, not proof of non-delivery. All other
errors fail the test; normal traffic after recovery must succeed. Alice must reconnect to the same node and resume
bidirectional traffic. One ambiguous SEND is reconciled as delivered; it must
not be replayed. A fixed 100,000-entry bitmap per peer detects duplicates while
payload retention stays bounded to 128 unreconciled messages.

The long workload lasts 3,600 seconds after 60 seconds of warmup; the short
workload lasts 120 seconds after ten seconds of warmup. Startup, final checks
and cleanup have an additional bounded allowance. At least five acknowledged
messages per elapsed workload second are required; this is a liveness floor,
not a throughput promise or capacity benchmark.

## Resource gates fixed before the run

`ps`/`lsof` sample owned clients and live server processes every ten seconds
(five for the short run). RSS is KiB; CPU is the operating system's `ps` value.
Server samples provide context; the following gates apply to C++ processes:

- Median RSS in the final five samples may exceed the first five warmed samples
  by at most 16 MiB. Runs of at least ten minutes also require a fitted RSS slope
  no greater than 256 KiB/minute. These thresholds detect bounded growth; they
  do not prove absence of small leaks or guarantee production memory use.
- Final connected thread count may exceed baseline by at most one, descriptors
  by at most two, and each client must have exactly one TCP connection.
- After `destroy()` and destruction of the SDK object, each still-running C++
  consumer must have exactly one application thread and zero TCP connections.
- Every future must settle, every received message must reconcile, all peers
  must exit successfully, and the public online-status API must return no routes.

Explicit consumer settings are: connection/request timeouts 3 s, heartbeat 1 s,
pong timeout 4 s, reconnect backoff 500–1,500 ms, 40 attempts, no jitter, and
16 pending requests. The released SDK defaults remain unchanged.

## Reproduce

Use the toolchain and clean server/JS preparation in
[CLUSTER_VALIDATION.md](CLUSTER_VALIDATION.md), plus `ps` and `lsof`. For this
workload, check out the server revision above before building it.
On Linux, install `lsof` if absent. Python 3.12 needs no extra packages.

```sh
python3 tests/soak.py \
  --archive /absolute/path/WuKongEasySDK-CPP-0.1.0-macos-arm64-appleclang.zip \
  --server /absolute/path/wukongim \
  --js-entry /absolute/path/WuKongEasySDK-JS/dist/cjs/index.js \
  --configuration Release --duration-seconds 3600 \
  --report /absolute/path/soak-receipt.json
```

Only durations 120 and 3600 are accepted. Default is 120 seconds, Release.
The read-only `soak.yml` runs the short workload on relevant changes, retains
receipts for 14 days, and provides explicit manual long-test dispatch.
Failure receipts record the stage and exception type without credentials,
protocol data, server logs or temporary private keys.

A passing short run is not one-hour evidence. A one-host Release run does not
establish Debug, Windows, multi-host, production-CA, high fanout, high connection
count, multi-day endurance, or exactly-once delivery guarantees. Timeouts can
represent messages already delivered; applications still own reconciliation.
