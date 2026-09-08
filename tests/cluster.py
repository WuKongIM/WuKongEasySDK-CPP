"""Bounded three-node acceptance of public C++ binaries; no SDK source compilation."""
import argparse
import asyncio
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import tempfile
import time
import urllib.request
import zipfile
from collections import Counter, deque

SERVER_REVISION = "5f5003778ccee6786591ed9968a5185e9213ea55"
JS_REVISION = "9c03c98c725982fac224cd1d3b52456eae983975"
ARCHIVES = {
    "WuKongEasySDK-CPP-0.1.0-linux-x64-gcc13.zip": "bda2cddfa84ace5a6c864259ff749a538bb10e2bef6640da574f8d5cf582491b",
    "WuKongEasySDK-CPP-0.1.0-macos-arm64-appleclang.zip": "b7508bdf9ba5235ee04d197af418fd0d12f6784ea173359fa985035e47e69bd8",
}
HERE = Path(__file__).resolve().parent


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def command(*args, **kwargs):
    try:
        return subprocess.check_output([str(arg) for arg in args], text=True, timeout=180, **kwargs).strip()
    except subprocess.CalledProcessError as error:
        print((error.output or "")[-3000:], flush=True)
        raise


def prepare(args, root):
    """Verify release, server, JS and installed CMake provenance before launching peers."""
    expected = ARCHIVES[args.archive.name]
    assert digest(args.archive) == expected, "Public archive SHA256 mismatch"
    build = command("go", "version", "-m", args.server)
    assert f"vcs.revision={SERVER_REVISION}" in build and "vcs.modified=false" in build
    js_root = args.js_entry.parents[2]
    assert command("git", "-C", js_root, "rev-parse", "HEAD") == JS_REVISION
    assert not command("git", "-C", js_root, "status", "--porcelain"), "JS checkout must be clean"
    with zipfile.ZipFile(args.archive) as archive:
        for entry in archive.infolist():
            assert (root / entry.filename).resolve().is_relative_to(root.resolve()), "Unsafe ZIP path"
            assert (entry.external_attr >> 16) & 0o170000 != 0o120000, "ZIP symlink rejected"
        archive.extractall(root)
    bundles = list(root.glob("*/BUILD_INFO.json"))
    assert len(bundles) == 1
    bundle = bundles[0].parent
    info = json.loads(bundles[0].read_text())
    assert info["version"] == "0.1.0"
    for name, sha in json.loads((bundle / "FILES.sha256.json").read_text()).items():
        path = (bundle / name).resolve()
        assert path.is_relative_to(bundle.resolve()) and digest(path) == sha, "Bundle ledger mismatch"
    app = root / "independent consumer"
    shutil.copytree(HERE / "cluster_consumer", app)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("VCPKG_", "CMAKE_PREFIX_", "OPENSSL_", "BOOST_"))}
    clients = {}
    for mode in args.configurations:
        build_dir = root / mode
        command("cmake", "-S", app, "-B", build_dir, "-G", "Ninja",
                f"-DCMAKE_TOOLCHAIN_FILE={bundle / 'wukong-sdk.cmake'}", f"-DCMAKE_BUILD_TYPE={mode}",
                "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF", "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF", env=env)
        command("cmake", "--build", build_dir, "--parallel", "2", env=env)
        cache = (build_dir / "CMakeCache.txt").read_text().splitlines()
        for dependency in ("WuKongEasySDK_DIR", "Boost_DIR", "nlohmann_json_DIR", "OPENSSL_INCLUDE_DIR"):
            value = next(line.split("=", 1)[1] for line in cache if line.startswith(dependency + ":"))
            assert Path(value).resolve().is_relative_to(bundle.resolve()), dependency + " escaped archive"
        clients[mode] = build_dir / "wukong_cluster_peer"
    return clients, {
        "archive": args.archive.name, "archive_sha256": expected, "build_info": info,
        "server_revision": SERVER_REVISION, "server_sha256": digest(args.server),
        "js_revision": JS_REVISION, "js_entry_sha256": digest(args.js_entry),
        "node": command("node", "--version"), "cmake": command("cmake", "--version").splitlines()[0],
        "harness_revision": command("git", "-C", HERE, "rev-parse", "HEAD"),
        "harness_dirty": bool(command("git", "-C", HERE, "status", "--porcelain")),
        "harness_sha256": {str(p.relative_to(HERE)): digest(p) for p in
                           [Path(__file__), HERE / "cluster_peer.cjs", *sorted((HERE / "cluster_consumer").glob("*"))]},
    }


def free_ports(count):
    sockets = [socket.socket() for _ in range(count)]
    try:
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        return [sock.getsockname()[1] for sock in sockets]
    finally:
        for sock in sockets:
            sock.close()


async def request(base, path, data=None):
    def read():
        req = urllib.request.Request(base + path, data=None if data is None else json.dumps(data).encode(),
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=2) as response:
            raw = response.read()
            return json.loads(raw) if raw else None
    return await asyncio.to_thread(read)


async def eventually(predicate, seconds=30):
    async with asyncio.timeout(seconds):
        while not predicate():
            await asyncio.sleep(0.05)


async def stop(process):
    if process is None or process.returncode is not None:
        return
    process.terminate()
    try:
        await asyncio.wait_for(process.wait(), 8)
    except TimeoutError:
        process.kill()
        await asyncio.wait_for(process.wait(), 5)


class Proxy:
    """Opaque TLS termination with bounded streams; faults never fabricate protocol frames."""
    def __init__(self, upstream):
        self.upstream = upstream
        self.blocked = self.withhold = False
        self.writers, self.tasks = set(), set()
        self.server = None

    async def handle(self, reader, writer):
        current = asyncio.current_task()
        self.tasks.add(current)
        self.writers.add(writer)
        other, pumps = None, []
        try:
            if self.blocked:
                return
            remote, other = await asyncio.open_connection("127.0.0.1", self.upstream)
            self.writers.add(other)

            async def pump(source, target, downstream=False):
                while data := await source.read(65536):
                    if downstream and self.withhold:
                        continue
                    target.write(data)
                    await target.drain()
            pumps = [asyncio.create_task(pump(reader, other)), asyncio.create_task(pump(remote, writer, True))]
            done, _ = await asyncio.wait(pumps, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except (OSError, ConnectionError):
            pass
        finally:
            for task in pumps:
                task.cancel()
            await asyncio.gather(*pumps, return_exceptions=True)
            for stream in (writer, other):
                if stream:
                    stream.transport.abort()
                    self.writers.discard(stream)
            self.tasks.discard(current)

    def abort(self):
        assert self.writers, "No active connection for fault injection"
        for writer in list(self.writers):
            writer.transport.abort()

    async def close(self):
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        for writer in list(self.writers):
            writer.transport.abort()
        tasks = list(self.tasks)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


class Cluster:
    """Own only this run's three loopback product processes and their durable directories."""
    def __init__(self, root, binary):
        self.root, self.binary = root, binary
        ports = free_ports(15)
        self.ports = [ports[i:i + 5] for i in range(0, 15, 5)]
        self.processes, self.outputs, self.proxies = [None] * 3, [], []
        self.ca = root / "ca.pem"
        key = root / "key.pem"
        command("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                "-keyout", key, "-out", self.ca, "-subj", "/CN=localhost",
                "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1", stderr=subprocess.DEVNULL)
        self.tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.tls.minimum_version = ssl.TLSVersion.TLSv1_2
        self.tls.load_cert_chain(self.ca, key)

    def api(self, i):
        return f"http://127.0.0.1:{self.ports[i][1]}"

    def url(self, i):
        return f"wss://127.0.0.1:{self.ports[i][4]}/ws"

    async def start(self):
        nodes = ",".join(f'{{id={i + 1},addr="127.0.0.1:{p[0]}"}}' for i, p in enumerate(self.ports))
        for i, (cluster, api, gateway, manager, tls) in enumerate(self.ports):
            node = self.root / f"node{i + 1}"
            node.mkdir()
            (node / "wukongim.toml").write_text(f'''[node]
id = {i + 1}
data_dir = "{node / 'data'}"
[cluster]
id = "cpp-released-three-node"
listen_addr = "127.0.0.1:{cluster}"
nodes = [{nodes}]
hash_slot_count = 256
initial_slot_count = 12
slot_replica_n = 3
[api]
listen_addr = "127.0.0.1:{api}"
[manager]
listen_addr = "127.0.0.1:{manager}"
[gateway]
token_auth_on = true
listeners = [{{name="ws",network="websocket",address="127.0.0.1:{gateway}",transport="gnet",protocol="wsmux",path="/ws"}}]
[delivery]
enable = true
[plugin]
enable = false
[observability]
metrics_enable = true
[log]
level = "error"
console = false
dir = "{node / 'logs'}"
''')
            await self.spawn(i)
            proxy = Proxy(gateway)
            self.proxies.append(proxy)
            proxy.server = await asyncio.start_server(proxy.handle, "127.0.0.1", tls, ssl=self.tls)
        await asyncio.gather(*(self.ready(i) for i in range(3)))
        await self.stable([0, 1, 2])

    async def spawn(self, i):
        node = self.root / f"node{i + 1}"
        output = (node / "process.log").open("ab")
        self.outputs.append(output)
        self.processes[i] = await asyncio.create_subprocess_exec(
            str(self.binary), "-config", str(node / "wukongim.toml"), cwd=node,
            env={k: v for k, v in os.environ.items() if not k.startswith(("WK_", "WKIM_"))},
            stdout=output, stderr=output)

    async def ready(self, i):
        async with asyncio.timeout(60):
            while True:
                assert self.processes[i].returncode is None, f"Node {i + 1} exited before readiness"
                try:
                    await request(self.api(i), "/readyz")
                    return
                except OSError:
                    await asyncio.sleep(0.1)

    async def stable(self, alive):
        """Require identical elected leaders and quorum on every live Slot replica."""
        previous, samples = None, 0
        async with asyncio.timeout(45):
            while True:
                fingerprints = []
                try:
                    for i in alive:
                        base = f"http://127.0.0.1:{self.ports[i][3]}"
                        value = await request(base, f"/manager/slots?node_id={i + 1}")
                        assert value["total"] == len(value["items"]) == 12
                        leaders = []
                        for slot in value["items"]:
                            runtime, local = slot["runtime"], slot["node_log"]
                            assert sorted(runtime["current_voters"]) == [1, 2, 3]
                            assert runtime["has_quorum"] and runtime["healthy_voters"] >= 2
                            assert runtime["leader_id"] in [j + 1 for j in alive]
                            assert local["leader_id"] == runtime["leader_id"]
                            leaders.append((slot["slot_id"], runtime["leader_id"]))
                        assert len(set(s for s, _ in leaders)) == 12
                        fingerprints.append(sorted(leaders))
                    assert all(f == fingerprints[0] for f in fingerprints)
                    samples = samples + 1 if previous == fingerprints[0] else 1
                    previous = fingerprints[0]
                    if samples >= 5:
                        return previous
                except (OSError, AssertionError, KeyError, TypeError):
                    samples = 0
                await asyncio.sleep(0.1)

    async def close(self):
        await asyncio.gather(*(proxy.close() for proxy in self.proxies))
        await asyncio.gather(*(stop(p) for p in self.processes))
        for output in self.outputs:
            output.close()
        assert all(p is None or p.returncode is not None for p in self.processes)
        assert all(not p.tasks and not p.writers for p in self.proxies)


class Peer:
    """Bounded NDJSON control for independent C++/JS processes; no protocol emulation."""
    call_timeout = 12
    def __init__(self, uid):
        self.uid = uid
        self.process = self.reader = None
        self.pending, self.messages = {}, {}
        self.counts = Counter()
        self.connects = deque(maxlen=128)
        self.connected = self.ready = False
        self.errors, self.reconnecting, self.next_id = 0, 0, 0

    async def start(self, argv, env, expected_node):
        self.process = await asyncio.create_subprocess_exec(*map(str, argv), env=env,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
        self.reader = asyncio.create_task(self.read())
        await eventually(lambda: (self.ready and self.connects) or self.reader.done(), 15)
        if self.reader.done():
            self.reader.result()
        assert self.ready and self.connects[-1]["nodeId"] == expected_node, "Wrong ingress node"

    async def read(self):
        while line := await self.process.stdout.readline():
            item = json.loads(line)
            kind = item["kind"]
            if kind == "reply":
                self.pending[item["id"]].set_result(item)
            elif kind == "message":
                self.record_message(item["message"])
            elif kind == "connect":
                self.connected = True
                self.connects.append(item["result"])
            elif kind in ("disconnect", "reconnecting"):
                self.connected = False
                self.reconnecting += kind == "reconnecting"
            elif kind == "ready":
                self.ready = True
            elif kind == "error":
                self.errors += 1
            else:
                self.extra_event(kind)

    def record_message(self, value):
        counter = value["payload"]["counter"]
        assert len(self.messages) < 128, "Receive budget exceeded"
        self.messages[counter] = value
        self.counts[counter] += 1

    def extra_event(self, kind):
        raise AssertionError(f"Peer failure: {kind}")

    async def call(self, kind, **fields):
        self.next_id += 1
        identity = self.next_id
        future = asyncio.get_running_loop().create_future()
        self.pending[identity] = future
        try:
            self.process.stdin.write(json.dumps(dict(id=identity, kind=kind, **fields)).encode() + b"\n")
            await self.process.stdin.drain()
            async with asyncio.timeout(self.call_timeout):
                while not future.done():
                    if self.reader.done():
                        self.reader.result()
                        raise AssertionError("Peer exited before replying")
                    await asyncio.sleep(0.02)
            return future.result()
        finally:
            self.pending.pop(identity, None)

    async def message(self, counter):
        await eventually(lambda: counter in self.messages or self.reader.done(), 12)
        if self.reader.done():
            self.reader.result()
        return self.messages[counter]

    async def close(self):
        await stop(self.process)
        if self.reader:
            self.reader.cancel()
            await asyncio.gather(self.reader, return_exceptions=True)


async def scenario(args, root, client, mode):
    root.mkdir()
    cluster, peers = Cluster(root, args.server), []
    report = {"configuration": mode, "stage": "startup", "faults": []}
    counter = 0

    def stage(name):
        report["stage"] = name
        print(json.dumps({"configuration": mode, "stage": name}), flush=True)

    async def send(sender, receiver, payload):
        return await sender.call("send", uid=receiver.uid, payload=payload,
                                 clientMsgNo=f"cpp-cluster-{mode}-{payload['counter']}")

    async def exchange(sender, receiver):
        nonlocal counter
        counter += 1
        payload = {"counter": counter, "type": 1, "content": "C++ / JS 跨节点 🌍 " + "x" * 2000}
        result = await send(sender, receiver, payload)
        assert result["ok"], f"SEND failed: {result.get('code')}"
        ack, message = result["ack"], await receiver.message(counter)
        assert ack["reasonCode"] == 1 and message["fromUid"] == sender.uid
        assert message["payload"] == payload
        assert (ack["messageId"], ack["messageSeq"]) == (message["messageId"], message["messageSeq"])

    async def all_pairs():
        for sender, receiver in ((alice, bob), (bob, alice), (alice, carol), (carol, alice), (bob, carol), (carol, bob)):
            await exchange(sender, receiver)

    try:
        stage("startup")
        await cluster.start()
        for i, uid in enumerate(("cpp-cluster-alice", "cpp-cluster-bob", "cpp-cluster-carol")):
            token = os.urandom(24).hex()
            await request(cluster.api(0), "/user/token", {"uid": uid, "token": token, "device_flag": 2, "device_level": 1})
            peer = Peer(uid)
            peers.append(peer)
            env = {k: v for k, v in os.environ.items() if not k.startswith(("WKIM_", "NODE_TLS_"))}
            env.update(WKIM_URL=cluster.url(i), WKIM_UID=uid, WKIM_TOKEN=token,
                       WKIM_CA_FILE=str(cluster.ca), NODE_EXTRA_CA_CERTS=str(cluster.ca), WKIM_JS_ENTRY=str(args.js_entry))
            await peer.start(["node", HERE / "cluster_peer.cjs"] if i == 1 else [client], env, i + 1)
        alice, bob, carol = peers
        # This legacy endpoint returns active routes only; offline users are omitted.
        online = await request(cluster.api(1), "/user/onlinestatus", [p.uid for p in peers])
        assert len(online) == 3 and {r["uid"] for r in online} == {p.uid for p in peers}
        assert all(r["online"] == 1 and r["device_flag"] == 2 for r in online)
        stage("cross_node_messages")
        await all_pairs()
        proxy = cluster.proxies[0]
        for fault, expected_error in (("withheld_ack_timeout", -3), ("withheld_ack_transport_cut", -4)):
            stage(fault)
            counter += 1
            payload = {"counter": counter, "type": 1, "content": "Delivered with unavailable SENDACK"}
            connections = len(alice.connects)
            proxy.withhold = True
            started = time.monotonic()
            pending = asyncio.create_task(send(alice, carol, payload))
            try:
                message = await carol.message(counter)
                assert message["payload"] == payload and not pending.done(), "ACK was not withheld"
                if expected_error == -4:
                    proxy.blocked = True
                    proxy.abort()
                result = await pending
                elapsed = time.monotonic() - started
                assert not result["ok"] and result["code"] == expected_error, result
                if expected_error == -3:
                    assert 0.9 <= elapsed < 5, "Request did not respect its bounded timeout"
                    proxy.blocked = True
                    proxy.abort()
                await asyncio.sleep(1)
                proxy.blocked = proxy.withhold = False
                await eventually(lambda: alice.connected and len(alice.connects) > connections)
                assert alice.connects[-1]["nodeId"] == 1
                await exchange(alice, carol)
                await exchange(carol, alice)
                # Observe beyond reconnect: an ambiguous SEND must not be silently replayed.
                await asyncio.sleep(1)
                assert carol.counts[payload["counter"]] == 1
                report["faults"].append({"kind": fault, "send_error": expected_error,
                    "request_seconds": round(elapsed, 3), "recovery_seconds": round(time.monotonic() - started, 3),
                    "ambiguous_message_deliveries": carol.counts[payload["counter"]]})
            finally:
                proxy.blocked = proxy.withhold = False
                pending.cancel()
                await asyncio.gather(pending, return_exceptions=True)
        stage("ingress_crash_restart")
        connects, started = len(alice.connects), time.monotonic()
        cluster.processes[0].kill()
        await asyncio.wait_for(cluster.processes[0].wait(), 5)
        await eventually(lambda: not alice.connected, 10)
        await cluster.stable([1, 2])
        await exchange(bob, carol)
        await exchange(carol, bob)
        await cluster.spawn(0)
        await cluster.ready(0)
        await cluster.stable([0, 1, 2])
        await eventually(lambda: alice.connected and len(alice.connects) > connects, 30)
        assert alice.connects[-1]["nodeId"] == 1
        await all_pairs()
        report["faults"].append({"kind": "ingress_crash_restart", "node_id": 1,
            "surviving_nodes_bidirectional_messages": 2, "recovery_seconds": round(time.monotonic() - started, 3)})
        stage("shutdown_presence")
        for peer in peers:
            result = await peer.call("stop")
            assert result["ok"] and result["destroyed"]
            assert await asyncio.wait_for(peer.process.wait(), 5) == 0
        async with asyncio.timeout(20):
            while True:
                values = await request(cluster.api(1), "/user/onlinestatus", [p.uid for p in peers])
                assert isinstance(values, list)
                if not values:
                    break
                await asyncio.sleep(0.1)
        assert all(count == 1 for p in peers for count in p.counts.values()), "Raw duplicate delivery observed"
        report.update(stage="passed", message_attempts=counter, initial_online_routes=3, presence_cleared=True,
                      peers=[{"uid": p.uid, "node_ids": [c["nodeId"] for c in p.connects],
                              "received": sum(p.counts.values()), "reconnecting_events": p.reconnecting} for p in peers])
        return report
    except Exception:
        print(json.dumps({"configuration": mode, "failed_stage": report["stage"],
                          "process_returncodes": [p.returncode if p else None for p in cluster.processes]}), flush=True)
        raise
    finally:
        await asyncio.gather(*(p.close() for p in peers))
        await cluster.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--js-entry", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--configurations", nargs="+", choices=["Debug", "Release"], default=["Debug", "Release"])
    args = parser.parse_args()
    args.archive, args.server, args.js_entry = args.archive.resolve(), args.server.resolve(), args.js_entry.resolve()
    with tempfile.TemporaryDirectory(prefix="wk-cpp-cluster-") as directory:
        root = Path(directory)
        clients, report = prepare(args, root)

        async def run():
            results = []
            async with asyncio.timeout(300):
                for mode, client in clients.items():
                    results.append(await scenario(args, root / (mode + "-cluster"), client, mode))
            return results
        report["results"] = asyncio.run(run())
        report["topology"] = {"nodes": 3, "hash_slots": 256, "logical_slots": 12, "slot_replicas": 3,
                              "token_auth": True, "transport": "WSS through loopback TLS termination"}
        report["cleanup"] = "owned clients, proxies and nodes stopped; temporary keys/data removed"
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print("Released C++ three-node WSS acceptance passed: " + ", ".join(args.configurations), flush=True)


if __name__ == "__main__":
    main()
